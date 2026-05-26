#include "memory/memory_internal.h"

#include "sc/contract.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>

typedef struct markdown_memory {
    sc_allocator *alloc;
    sc_string path;
    sc_memory_store store;
} markdown_memory;

static sc_status markdown_put(void *impl, const sc_memory_record *record);
static sc_status markdown_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out);
static sc_status markdown_search(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_memory_result *out);
static sc_status markdown_forget(void *impl, sc_str namespace_name, sc_str key);
static sc_status markdown_redact(void *impl, sc_str namespace_name, sc_str key);
static sc_status markdown_purge_namespace(void *impl, sc_str namespace_name);
static sc_status markdown_purge_session(void *impl, sc_str namespace_name, sc_str session_id);
static sc_status markdown_export_snapshot(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_string *out);
static sc_status markdown_export_snapshot_ex(void *impl,
                                             const sc_memory_query *query,
                                             const sc_memory_export_options *options,
                                             sc_allocator *alloc,
                                             sc_string *out);
static void markdown_destroy(void *impl);
static sc_status markdown_load(markdown_memory *memory);
static sc_status read_file(sc_allocator *alloc, sc_str path, sc_string *out, bool *exists);
static sc_status markdown_rewrite(markdown_memory *memory);
static sc_status append_markdown_escaped(sc_string_builder *builder, sc_str value);
static bool find_snapshot_comment(sc_str text, sc_str *out);

static const sc_memory_vtab markdown_vtab = {
    .struct_size = sizeof(sc_memory_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "markdown",
    .display_name = "Markdown memory",
    .feature_flag = "SC_MEMORY_MARKDOWN",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .put = markdown_put,
    .get = markdown_get,
    .search = markdown_search,
    .forget = markdown_forget,
    .purge_namespace = markdown_purge_namespace,
    .purge_session = markdown_purge_session,
    .export_snapshot = markdown_export_snapshot,
    .destroy = markdown_destroy,
    .config_schema_ref = "sc.schema.memory.markdown.v1",
    .memory_capabilities = SC_MEMORY_BACKEND_CAP_STORE |
                           SC_MEMORY_BACKEND_CAP_RETRIEVE |
                           SC_MEMORY_BACKEND_CAP_DELETE |
                           SC_MEMORY_BACKEND_CAP_REDACT |
                           SC_MEMORY_BACKEND_CAP_SNAPSHOTS |
                           SC_MEMORY_BACKEND_CAP_IMPORT_EXPORT,
    .persistence_type = SC_MEMORY_PERSISTENCE_FILE,
    .migration_version = 2,
    .supports_migrations = false,
    .redact = markdown_redact,
    .export_snapshot_ex = markdown_export_snapshot_ex,
};

sc_status sc_memory_markdown_open(sc_allocator *alloc, sc_str path, sc_memory **out)
{
    markdown_memory *impl = nullptr;
    sc_status status;

    if (out == nullptr || path.ptr == nullptr || path.len == 0) {
        return sc_status_invalid_argument("sc.memory_markdown.invalid_argument");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(markdown_memory));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (markdown_memory){.alloc = alloc};
    sc_memory_store_init(&impl->store, alloc);

    status = sc_string_from_str(alloc, path, &impl->path);
    if (sc_status_is_ok(status)) {
        status = markdown_load(impl);
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_new(alloc, &markdown_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        markdown_destroy(impl);
    }
    return status;
}

static sc_status markdown_put(void *impl, const sc_memory_record *record)
{
    markdown_memory *memory = impl;
    sc_status status;

    if (memory == nullptr || record == nullptr) {
        return sc_status_invalid_argument("sc.memory_markdown.invalid_argument");
    }
    status = sc_memory_store_put(&memory->store, record);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return markdown_rewrite(memory);
}

static sc_status markdown_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out)
{
    markdown_memory *memory = impl;
    return sc_memory_store_get(memory == nullptr ? nullptr : &memory->store, namespace_name, key, alloc, out);
}

static sc_status markdown_search(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_memory_result *out)
{
    markdown_memory *memory = impl;
    return sc_memory_store_search(memory == nullptr ? nullptr : &memory->store, query, alloc, out);
}

static sc_status markdown_forget(void *impl, sc_str namespace_name, sc_str key)
{
    markdown_memory *memory = impl;
    sc_status status = sc_memory_store_forget(memory == nullptr ? nullptr : &memory->store, namespace_name, key);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return markdown_rewrite(memory);
}

static sc_status markdown_redact(void *impl, sc_str namespace_name, sc_str key)
{
    markdown_memory *memory = impl;
    sc_status status = sc_memory_store_redact(memory == nullptr ? nullptr : &memory->store, namespace_name, key);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return markdown_rewrite(memory);
}

static sc_status markdown_purge_namespace(void *impl, sc_str namespace_name)
{
    markdown_memory *memory = impl;
    sc_status status = sc_memory_store_purge_namespace(memory == nullptr ? nullptr : &memory->store, namespace_name);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return markdown_rewrite(memory);
}

static sc_status markdown_purge_session(void *impl, sc_str namespace_name, sc_str session_id)
{
    markdown_memory *memory = impl;
    sc_status status = sc_memory_store_purge_session(memory == nullptr ? nullptr : &memory->store, namespace_name, session_id);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return markdown_rewrite(memory);
}

static sc_status markdown_export_snapshot(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_string *out)
{
    return markdown_export_snapshot_ex(impl, query, nullptr, alloc, out);
}

static sc_status markdown_export_snapshot_ex(void *impl,
                                             const sc_memory_query *query,
                                             const sc_memory_export_options *options,
                                             sc_allocator *alloc,
                                             sc_string *out)
{
    markdown_memory *memory = impl;
    return sc_memory_store_export_snapshot(memory == nullptr ? nullptr : &memory->store, query, options, alloc, out);
}

static void markdown_destroy(void *impl)
{
    markdown_memory *memory = impl;
    if (memory == nullptr) {
        return;
    }
    sc_memory_store_clear(&memory->store);
    sc_string_clear(&memory->path);
    sc_free(memory->alloc, memory, sizeof(*memory), _Alignof(markdown_memory));
}

static sc_status markdown_load(markdown_memory *memory)
{
    sc_string text = {0};
    sc_str snapshot = {0};
    bool exists = false;
    sc_status status;

    if (memory == nullptr) {
        return sc_status_invalid_argument("sc.memory_markdown.invalid_argument");
    }
    status = read_file(memory->alloc, sc_string_as_str(&memory->path), &text, &exists);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (!exists) {
        return markdown_rewrite(memory);
    }
    if (text.len == 0) {
        sc_string_clear(&text);
        return markdown_rewrite(memory);
    }
    if (find_snapshot_comment(sc_string_as_str(&text), &snapshot)) {
        status = sc_memory_store_import_snapshot(&memory->store,
                                                 snapshot,
                                                 &(sc_memory_import_options){.struct_size = sizeof(sc_memory_import_options),
                                                                            .allow_secrets = true});
    } else if (strncmp(text.ptr, "# SmolClaw memory", strlen("# SmolClaw memory")) == 0) {
        status = markdown_rewrite(memory);
    } else {
        status = sc_status_parse("sc.memory_markdown.snapshot_missing");
    }
    sc_string_clear(&text);
    return status;
}

static sc_status read_file(sc_allocator *alloc, sc_str path, sc_string *out, bool *exists)
{
    FILE *file = nullptr;
    long length = 0;
    char *buffer = nullptr;
    size_t allocation_size = 0;
    sc_status status = sc_status_ok();

    if (out == nullptr || exists == nullptr || path.ptr == nullptr) {
        return sc_status_invalid_argument("sc.memory_markdown.read_invalid_argument");
    }
    *exists = false;
    file = fopen(path.ptr, "rb");
    if (file == nullptr) {
        if (errno == ENOENT) {
            goto cleanup;
        }
        status = sc_status_io("sc.memory_markdown.open_failed");
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        status = sc_status_io("sc.memory_markdown.seek_failed");
        goto cleanup;
    }
    length = ftell(file);
    if (length < 0) {
        status = sc_status_io("sc.memory_markdown.tell_failed");
        goto cleanup;
    }
    if (length > SC_MEMORY_IMPORT_MAX_BYTES) {
        status = sc_status_invalid_argument("sc.memory_markdown.file_too_large");
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        status = sc_status_io("sc.memory_markdown.seek_failed");
        goto cleanup;
    }
    allocation_size = (size_t)length + 1U;
    buffer = sc_alloc(alloc, allocation_size, _Alignof(char));
    if (buffer == nullptr) {
        status = sc_status_no_memory();
        goto cleanup;
    }
    if (length > 0 && fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        status = sc_status_io("sc.memory_markdown.read_failed");
        goto cleanup;
    }
    if (fclose(file) != 0) {
        file = nullptr;
        status = sc_status_io("sc.memory_markdown.close_failed");
        goto cleanup;
    }
    file = nullptr;
    buffer[length] = '\0';
    *out = (sc_string){.ptr = buffer, .len = (size_t)length, .alloc = alloc};
    buffer = nullptr;
    *exists = true;

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    if (buffer != nullptr) {
        sc_free(alloc, buffer, allocation_size, _Alignof(char));
    }
    return status;
}

static bool find_snapshot_comment(sc_str text, sc_str *out)
{
    const char *prefix = "<!-- sc-memory-snapshot: ";
    const char *start = nullptr;
    const char *end = nullptr;

    if (out == nullptr || text.ptr == nullptr) {
        return false;
    }
    start = strstr(text.ptr, prefix);
    if (start == nullptr || start >= text.ptr + text.len) {
        return false;
    }
    start += strlen(prefix);
    end = strstr(start, " -->");
    if (end == nullptr || end < start || end > text.ptr + text.len) {
        return false;
    }
    *out = sc_str_from_parts(start, (size_t)(end - start));
    return true;
}

static sc_status markdown_rewrite(markdown_memory *memory)
{
    sc_string_builder builder = {0};
    sc_status status;
    sc_string text = {0};
    sc_string snapshot = {0};
    FILE *file = nullptr;
    sc_memory_export_options raw_export = {
        .struct_size = sizeof(raw_export),
        .include_raw_sensitive_content = true,
        .include_redacted = true,
        .include_deleted = true,
    };
    sc_memory_query all_records = {
        .struct_size = sizeof(all_records),
        .include_redacted = true,
        .include_deleted = true,
    };

    if (memory == nullptr) {
        return sc_status_invalid_argument("sc.memory_markdown.invalid_argument");
    }

    status = sc_memory_store_export_snapshot(&memory->store, &all_records, &raw_export, memory->alloc, &snapshot);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    sc_string_builder_init(&builder, memory->alloc);
    status = sc_string_builder_append_cstr(&builder, "# SmolClaw memory\n\n");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "<!-- sc-memory-snapshot: ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, sc_string_as_str(&snapshot));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, " -->\n\n");
    }
    status = sc_status_is_ok(status)
                 ? sc_string_builder_append_cstr(&builder,
                                                 "| id | namespace | session | category | key | content | redaction | score | importance | superseded_by |\n")
                 : status;
    status = sc_status_is_ok(status)
                 ? sc_string_builder_append_cstr(&builder,
                                                 "|---|---|---|---|---|---|---:|---:|---:|---|\n")
                 : status;
    for (size_t i = 0; sc_status_is_ok(status) && i < memory->store.entries.len; i += 1) {
        const sc_memory_entry *entry = sc_vec_at_const(&memory->store.entries, i);
        char number[96] = {0};
        int written = 0;

        status = sc_string_builder_append_cstr(&builder, "| ");
        if (sc_status_is_ok(status)) {
            status = append_markdown_escaped(&builder, sc_string_as_str(&entry->id));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " | ");
        }
        if (sc_status_is_ok(status)) {
            status = append_markdown_escaped(&builder, sc_string_as_str(&entry->namespace_name));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " | ");
        }
        if (sc_status_is_ok(status)) {
            status = append_markdown_escaped(&builder, sc_string_as_str(&entry->session_id));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " | ");
        }
        if (sc_status_is_ok(status)) {
            status = append_markdown_escaped(&builder, sc_string_as_str(&entry->category));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " | ");
        }
        if (sc_status_is_ok(status)) {
            status = append_markdown_escaped(&builder, sc_string_as_str(&entry->key));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " | ");
        }
        if (sc_status_is_ok(status)) {
            status = append_markdown_escaped(&builder, sc_string_as_str(&entry->content));
        }
        written = snprintf(number,
                           sizeof(number),
                           " | %d | %.6f | %lld | ",
                           (int)entry->redaction_state,
                           entry->score,
                           (long long)entry->importance);
        if (sc_status_is_ok(status)) {
            status = written > 0 && (size_t)written < sizeof(number)
                         ? sc_string_builder_append_cstr(&builder, number)
                         : sc_status_io("sc.memory_markdown.format_failed");
        }
        if (sc_status_is_ok(status)) {
            status = append_markdown_escaped(&builder, sc_string_as_str(&entry->superseded_by_id));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " |\n");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &text);
    }
    sc_string_builder_clear(&builder);
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&snapshot);
        return status;
    }

    file = fopen(memory->path.ptr, "wb");
    if (file == nullptr) {
        sc_string_clear(&text);
        sc_string_clear(&snapshot);
        return sc_status_io("sc.memory_markdown.open_failed");
    }
    if (text.len > 0 && fwrite(text.ptr, 1, text.len, file) != text.len) {
        (void)fclose(file);
        sc_string_clear(&text);
        sc_string_clear(&snapshot);
        return sc_status_io("sc.memory_markdown.write_failed");
    }
    if (fclose(file) != 0) {
        sc_string_clear(&text);
        sc_string_clear(&snapshot);
        return sc_status_io("sc.memory_markdown.close_failed");
    }
    sc_string_clear(&text);
    sc_string_clear(&snapshot);
    return sc_status_ok();
}

static sc_status append_markdown_escaped(sc_string_builder *builder, sc_str value)
{
    sc_status status = sc_status_ok();

    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        char ch = value.ptr[i];
        if (ch == '\\' || ch == '|') {
            char escaped[2] = {'\\', ch};
            status = sc_string_builder_append(builder, sc_str_from_parts(escaped, sizeof(escaped)));
        } else if (ch == '\n') {
            status = sc_string_builder_append_cstr(builder, "\\n");
        } else if (ch == '\r') {
            status = sc_string_builder_append_cstr(builder, "\\r");
        } else {
            status = sc_string_builder_append(builder, sc_str_from_parts(value.ptr + i, 1));
        }
    }
    return status;
}
