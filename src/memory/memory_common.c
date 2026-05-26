#include "memory/memory_internal.h"

#include "sc/api.h"
#include "sc/json.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool str_is_empty(sc_str value);
static sc_str string_or_empty(const sc_string *string);
static sc_status copy_entry_from_record(sc_allocator *alloc, const sc_memory_record *record, sc_memory_entry *out);
static sc_status copy_string_field(sc_allocator *alloc, sc_str value, sc_string *out);
static sc_status validate_text_field(sc_str value, size_t max_len, const char *error_key);
static bool key_looks_secret(sc_str key);
static bool redaction_state_valid(sc_memory_redaction_state state);
static bool str_contains_casefold(sc_str haystack, sc_str needle);
static void store_remove_at(sc_memory_store *store, size_t index);
static bool namespace_key_equal(const sc_memory_entry *entry, sc_str namespace_name, sc_str key);
static int compare_str(sc_str left, sc_str right);
void sc_memory_result_init(sc_memory_result *result, sc_allocator *alloc)
{
    if (result == nullptr) {
        return;
    }
    result->alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_vec_init(&result->entries, result->alloc, sizeof(sc_memory_entry));
}

sc_status sc_memory_result_push(sc_memory_result *result, const sc_memory_entry *entry)
{
    sc_memory_entry copy = {0};
    sc_status status;

    if (result == nullptr || entry == nullptr || result->entries.item_size != sizeof(sc_memory_entry)) {
        return sc_status_invalid_argument("sc.memory_result.invalid_argument");
    }

    copy.struct_size = sizeof(copy);
    copy.timestamp = entry->timestamp;
    copy.score = entry->score;
    copy.importance = entry->importance;
    copy.redaction_state = entry->redaction_state;
    copy.retention_policy = entry->retention_policy;
    copy.expires_at = entry->expires_at;

    status = copy_string_field(result->alloc, string_or_empty(&entry->id), &copy.id);
    if (sc_status_is_ok(status)) {
        status = copy_string_field(result->alloc, string_or_empty(&entry->namespace_name), &copy.namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(result->alloc, string_or_empty(&entry->session_id), &copy.session_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(result->alloc, string_or_empty(&entry->category), &copy.category);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(result->alloc, string_or_empty(&entry->key), &copy.key);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(result->alloc, string_or_empty(&entry->content), &copy.content);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(result->alloc, string_or_empty(&entry->superseded_by_id), &copy.superseded_by_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(result->alloc, string_or_empty(&entry->source), &copy.source);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(result->alloc, string_or_empty(&entry->metadata_json), &copy.metadata_json);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(result->alloc, string_or_empty(&entry->content_ref), &copy.content_ref);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&result->entries, &copy);
    }
    if (!sc_status_is_ok(status)) {
        sc_memory_entry_clear(&copy);
        return status;
    }

    return sc_status_ok();
}

void sc_memory_entry_clear(sc_memory_entry *entry)
{
    if (entry == nullptr) {
        return;
    }

    sc_string_clear(&entry->id);
    sc_string_clear(&entry->namespace_name);
    sc_string_clear(&entry->session_id);
    sc_string_clear(&entry->category);
    sc_string_clear(&entry->key);
    sc_string_clear(&entry->content);
    sc_string_clear(&entry->superseded_by_id);
    sc_string_clear(&entry->source);
    sc_string_clear(&entry->metadata_json);
    sc_string_clear(&entry->content_ref);
    *entry = (sc_memory_entry){0};
}

void sc_memory_result_clear(sc_memory_result *result)
{
    if (result == nullptr) {
        return;
    }

    for (size_t i = 0; i < result->entries.len; i += 1) {
        sc_memory_entry *entry = sc_vec_at(&result->entries, i);
        sc_memory_entry_clear(entry);
    }
    sc_vec_clear(&result->entries);
    *result = (sc_memory_result){0};
}

sc_status sc_memory_apply_retention(sc_memory *memory, sc_str namespace_name, size_t max_entries)
{
    sc_memory_result result = {0};
    sc_memory_query query = {
        .struct_size = sizeof(query),
        .namespace_name = namespace_name,
        .limit = 0,
    };
    sc_status status;

    if (memory == nullptr || str_is_empty(namespace_name)) {
        return sc_status_invalid_argument("sc.memory.retention_invalid_argument");
    }
    status = sc_memory_search(memory, &query, sc_allocator_heap(), &result);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    for (size_t i = max_entries; sc_status_is_ok(status) && i < result.entries.len; i += 1) {
        sc_memory_entry *entry = sc_vec_at(&result.entries, i);
        status = sc_memory_forget(memory, namespace_name, sc_string_as_str(&entry->key));
    }
    sc_memory_result_clear(&result);
    return status;
}

sc_status sc_memory_find_duplicate(sc_memory *memory,
                                   sc_str namespace_name,
                                   sc_str content,
                                   sc_allocator *alloc,
                                   sc_string *out_key)
{
    sc_memory_result result = {0};
    sc_memory_query query = {
        .struct_size = sizeof(query),
        .namespace_name = namespace_name,
        .limit = 0,
    };
    sc_status status;

    if (memory == nullptr || out_key == nullptr || str_is_empty(namespace_name) || str_is_empty(content)) {
        return sc_status_invalid_argument("sc.memory.duplicate_invalid_argument");
    }
    status = sc_memory_search(memory, &query, alloc, &result);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = sc_status_invalid_argument("sc.memory.duplicate_not_found");
    for (size_t i = 0; i < result.entries.len; i += 1) {
        const sc_memory_entry *entry = sc_vec_at_const(&result.entries, i);
        if (entry != nullptr && sc_str_equal(sc_string_as_str(&entry->content), content)) {
            status = sc_string_from_str(alloc, sc_string_as_str(&entry->key), out_key);
            break;
        }
    }
    sc_memory_result_clear(&result);
    return status;
}

sc_status sc_memory_hydrate_snapshot(sc_memory *memory, sc_str snapshot_json)
{
    return sc_memory_import_snapshot(memory, snapshot_json, nullptr);
}

sc_status sc_memory_import_snapshot(sc_memory *memory, sc_str snapshot_json, const sc_memory_import_options *options)
{
    sc_json_value *root = nullptr;
    sc_status status;
    size_t max_bytes = SC_MEMORY_IMPORT_MAX_BYTES;
    size_t max_records = SC_MEMORY_IMPORT_MAX_RECORDS;
    bool allow_secrets = false;

    if (memory == nullptr || str_is_empty(snapshot_json)) {
        return sc_status_invalid_argument("sc.memory.hydrate_invalid_argument");
    }
    if (options != nullptr) {
        if (options->max_bytes > 0) {
            max_bytes = options->max_bytes;
        }
        if (options->max_records > 0) {
            max_records = options->max_records;
        }
        allow_secrets = options->allow_secrets;
    }
    if (snapshot_json.len > max_bytes) {
        return sc_status_invalid_argument("sc.memory.import_too_large");
    }
    status = sc_json_parse(sc_allocator_heap(), snapshot_json, &root, nullptr);
    if (sc_status_is_ok(status) && sc_json_type_of(root) != SC_JSON_ARRAY) {
        status = sc_status_parse("sc.memory.hydrate_expected_array");
    }
    if (sc_status_is_ok(status) && sc_json_array_len(root) > max_records) {
        status = sc_status_invalid_argument("sc.memory.import_too_many_records");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(root); i += 1) {
        status = sc_memory_hydrate_entry(memory, sc_json_array_get(root, i), allow_secrets);
    }
    sc_json_destroy(root);
    return status;
}

sc_status sc_memory_store_import_snapshot(sc_memory_store *store,
                                          sc_str snapshot_json,
                                          const sc_memory_import_options *options)
{
    sc_json_value *root = nullptr;
    sc_status status;
    size_t max_bytes = SC_MEMORY_IMPORT_MAX_BYTES;
    size_t max_records = SC_MEMORY_IMPORT_MAX_RECORDS;
    bool allow_secrets = false;

    if (store == nullptr || str_is_empty(snapshot_json)) {
        return sc_status_invalid_argument("sc.memory.hydrate_invalid_argument");
    }
    if (options != nullptr) {
        if (options->max_bytes > 0) {
            max_bytes = options->max_bytes;
        }
        if (options->max_records > 0) {
            max_records = options->max_records;
        }
        allow_secrets = options->allow_secrets;
    }
    if (snapshot_json.len > max_bytes) {
        return sc_status_invalid_argument("sc.memory.import_too_large");
    }
    status = sc_json_parse(sc_allocator_heap(), snapshot_json, &root, nullptr);
    if (sc_status_is_ok(status) && sc_json_type_of(root) != SC_JSON_ARRAY) {
        status = sc_status_parse("sc.memory.hydrate_expected_array");
    }
    if (sc_status_is_ok(status) && sc_json_array_len(root) > max_records) {
        status = sc_status_invalid_argument("sc.memory.import_too_many_records");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(root); i += 1) {
        status = sc_memory_hydrate_store_entry(store, sc_json_array_get(root, i), allow_secrets);
    }
    sc_json_destroy(root);
    return status;
}

sc_status sc_memory_validate_embedding(const sc_memory_vtab *vtab,
                                       const sc_memory_embedding_spec *expected,
                                       const sc_memory_embedding_spec *actual)
{
    if (vtab == nullptr || expected == nullptr || actual == nullptr) {
        return sc_status_invalid_argument("sc.memory.embedding_invalid_argument");
    }
    if ((vtab->memory_capabilities & SC_MEMORY_BACKEND_CAP_EMBEDDINGS) == 0) {
        return sc_status_unsupported("sc.memory.embedding_unsupported");
    }
    if (str_is_empty(expected->model_id) || str_is_empty(actual->model_id) ||
        !sc_str_equal(expected->model_id, actual->model_id)) {
        return sc_status_invalid_argument("sc.memory.embedding_model_mismatch");
    }
    if (expected->dimensions == 0 || actual->dimensions == 0 || expected->dimensions != actual->dimensions) {
        return sc_status_invalid_argument("sc.memory.embedding_dimension_mismatch");
    }
    if (actual->batch_size == 0 || (expected->batch_size > 0 && actual->batch_size > expected->batch_size)) {
        return sc_status_invalid_argument("sc.memory.embedding_batch_too_large");
    }
    if (actual->contains_secret) {
        return sc_status_security_denied("sc.memory.embedding_secret_denied");
    }
    return sc_status_ok();
}

bool sc_memory_key_is_reserved(sc_str key)
{
    static const char *reserved[] = {
        "__smolclaw_autosave",
        "__smolclaw_autosave_context",
        "smolclaw.autosave",
        "autosave.context",
    };

    for (size_t i = 0; i < SC_ARRAY_LEN(reserved); i += 1) {
        if (sc_str_equal(key, sc_str_from_cstr(reserved[i]))) {
            return true;
        }
    }
    return false;
}

void sc_memory_store_init(sc_memory_store *store, sc_allocator *alloc)
{
    if (store == nullptr) {
        return;
    }
    store->alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_vec_init(&store->entries, store->alloc, sizeof(sc_memory_entry));
}

sc_status sc_memory_store_put(sc_memory_store *store, const sc_memory_record *record)
{
    sc_memory_entry entry = {0};
    sc_status status;

    if (store == nullptr || record == nullptr || str_is_empty(record->namespace_name) || str_is_empty(record->key)) {
        return sc_status_invalid_argument("sc.memory_store.invalid_record");
    }
    status = sc_memory_validate_record(record);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    status = copy_entry_from_record(store->alloc, record, &entry);
    if (!sc_status_is_ok(status)) {
        sc_memory_entry_clear(&entry);
        return status;
    }

    for (size_t i = 0; i < store->entries.len; i += 1) {
        sc_memory_entry *existing = sc_vec_at(&store->entries, i);
        if (existing != nullptr && namespace_key_equal(existing, record->namespace_name, record->key)) {
            sc_memory_entry_clear(existing);
            *existing = entry;
            return sc_status_ok();
        }
    }

    status = sc_vec_push(&store->entries, &entry);
    if (!sc_status_is_ok(status)) {
        sc_memory_entry_clear(&entry);
        return status;
    }
    return sc_status_ok();
}

sc_status sc_memory_store_get(const sc_memory_store *store,
                              sc_str namespace_name,
                              sc_str key,
                              sc_allocator *alloc,
                              sc_string *out)
{
    const sc_memory_entry *entries = nullptr;

    if (store == nullptr || out == nullptr || str_is_empty(namespace_name) || str_is_empty(key)) {
        return sc_status_invalid_argument("sc.memory_store.invalid_argument");
    }

    entries = store->entries.ptr;
    for (size_t i = 0; i < store->entries.len; i += 1) {
        if (namespace_key_equal(&entries[i], namespace_name, key) && entries[i].superseded_by_id.len == 0) {
            if (entries[i].redaction_state != SC_MEMORY_REDACTION_NONE) {
                continue;
            }
            return sc_string_from_str(alloc, sc_string_as_str(&entries[i].content), out);
        }
    }
    return sc_status_invalid_argument("sc.memory_store.not_found");
}

sc_status sc_memory_store_search(const sc_memory_store *store,
                                 const sc_memory_query *query,
                                 sc_allocator *alloc,
                                 sc_memory_result *out)
{
    const sc_memory_entry *entries = nullptr;
    sc_status status;

    if (store == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.memory_store.invalid_argument");
    }
    status = sc_memory_validate_query(query);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    sc_memory_result_init(out, alloc);
    entries = store->entries.ptr;
    for (size_t i = 0; i < store->entries.len; i += 1) {
        if (sc_memory_query_matches_entry(query, &entries[i])) {
            status = sc_memory_result_push(out, &entries[i]);
            if (!sc_status_is_ok(status)) {
                sc_memory_result_clear(out);
                return status;
            }
        }
    }

    if (out->entries.len > 1) {
        qsort(out->entries.ptr, out->entries.len, sizeof(sc_memory_entry), sc_memory_entry_compare_desc);
    }
    if (query != nullptr) {
        size_t max_results = query->max_results > 0 ? query->max_results : query->limit;
        if (max_results > 0 && out->entries.len > max_results) {
            for (size_t i = max_results; i < out->entries.len; i += 1) {
                sc_memory_entry *entry = sc_vec_at(&out->entries, i);
                sc_memory_entry_clear(entry);
            }
            out->entries.len = max_results;
        }
    }
    if (query == nullptr && out->entries.len > SC_MEMORY_DEFAULT_MAX_RESULTS) {
        for (size_t i = SC_MEMORY_DEFAULT_MAX_RESULTS; i < out->entries.len; i += 1) {
            sc_memory_entry *entry = sc_vec_at(&out->entries, i);
            sc_memory_entry_clear(entry);
        }
        out->entries.len = SC_MEMORY_DEFAULT_MAX_RESULTS;
    }
    if (query != nullptr && query->max_total_result_bytes > 0) {
        size_t total = 0;
        for (size_t i = 0; i < out->entries.len;) {
            sc_memory_entry *entry = sc_vec_at(&out->entries, i);
            size_t bytes = entry == nullptr ? 0 : entry->content.len + entry->metadata_json.len;
            if (total + bytes > query->max_total_result_bytes) {
                for (size_t j = i; j < out->entries.len; j += 1) {
                    sc_memory_entry *removed = sc_vec_at(&out->entries, j);
                    sc_memory_entry_clear(removed);
                }
                out->entries.len = i;
                break;
            }
            total += bytes;
            i += 1;
        }
    }

    return sc_status_ok();
}

sc_status sc_memory_store_forget(sc_memory_store *store, sc_str namespace_name, sc_str key)
{
    if (store == nullptr || str_is_empty(namespace_name) || str_is_empty(key)) {
        return sc_status_invalid_argument("sc.memory_store.invalid_argument");
    }

    for (size_t i = 0; i < store->entries.len; i += 1) {
        sc_memory_entry *entry = sc_vec_at(&store->entries, i);
        if (entry != nullptr && namespace_key_equal(entry, namespace_name, key)) {
            sc_string_clear(&entry->content);
            sc_string_clear(&entry->metadata_json);
            entry->redaction_state = SC_MEMORY_REDACTION_DELETED;
            return sc_string_redacted(store->alloc, &entry->content);
        }
    }
    return sc_status_ok();
}

sc_status sc_memory_store_redact(sc_memory_store *store, sc_str namespace_name, sc_str key)
{
    if (store == nullptr || str_is_empty(namespace_name) || str_is_empty(key)) {
        return sc_status_invalid_argument("sc.memory_store.invalid_argument");
    }

    for (size_t i = 0; i < store->entries.len; i += 1) {
        sc_memory_entry *entry = sc_vec_at(&store->entries, i);
        if (entry != nullptr && namespace_key_equal(entry, namespace_name, key)) {
            sc_string_clear(&entry->content);
            sc_string_clear(&entry->metadata_json);
            entry->redaction_state = SC_MEMORY_REDACTION_REDACTED;
            return sc_string_redacted(store->alloc, &entry->content);
        }
    }
    return sc_status_ok();
}

sc_status sc_memory_store_purge_namespace(sc_memory_store *store, sc_str namespace_name)
{
    if (store == nullptr || str_is_empty(namespace_name)) {
        return sc_status_invalid_argument("sc.memory_store.invalid_argument");
    }

    for (size_t i = 0; i < store->entries.len;) {
        sc_memory_entry *entry = sc_vec_at(&store->entries, i);
        if (entry != nullptr && sc_str_equal(sc_string_as_str(&entry->namespace_name), namespace_name)) {
            store_remove_at(store, i);
        } else {
            i += 1;
        }
    }
    return sc_status_ok();
}

sc_status sc_memory_store_purge_session(sc_memory_store *store, sc_str namespace_name, sc_str session_id)
{
    if (store == nullptr || str_is_empty(namespace_name) || str_is_empty(session_id)) {
        return sc_status_invalid_argument("sc.memory_store.invalid_argument");
    }

    for (size_t i = 0; i < store->entries.len;) {
        sc_memory_entry *entry = sc_vec_at(&store->entries, i);
        if (entry != nullptr &&
            sc_str_equal(sc_string_as_str(&entry->namespace_name), namespace_name) &&
            sc_str_equal(sc_string_as_str(&entry->session_id), session_id)) {
            store_remove_at(store, i);
        } else {
            i += 1;
        }
    }
    return sc_status_ok();
}

sc_status sc_memory_store_export_snapshot(const sc_memory_store *store,
                                          const sc_memory_query *query,
                                          const sc_memory_export_options *options,
                                          sc_allocator *alloc,
                                          sc_string *out)
{
    sc_memory_result result = {0};
    sc_string_builder builder = {0};
    sc_status status;

    if (store == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.memory_store.invalid_argument");
    }

    status = sc_memory_store_search(store, query, alloc, &result);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "[");
    for (size_t i = 0; sc_status_is_ok(status) && i < result.entries.len; i += 1) {
        const sc_memory_entry *entry = sc_vec_at_const(&result.entries, i);
        if (i > 0) {
            status = sc_string_builder_append_cstr(&builder, ",");
        }
        if (sc_status_is_ok(status)) {
            status = sc_memory_append_entry_json(&builder, entry, options);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "]");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }

    sc_string_builder_clear(&builder);
    sc_memory_result_clear(&result);
    return status;
}

void sc_memory_store_clear(sc_memory_store *store)
{
    if (store == nullptr) {
        return;
    }

    for (size_t i = 0; i < store->entries.len; i += 1) {
        sc_memory_entry *entry = sc_vec_at(&store->entries, i);
        sc_memory_entry_clear(entry);
    }
    sc_vec_clear(&store->entries);
    *store = (sc_memory_store){0};
}

bool sc_memory_query_matches_entry(const sc_memory_query *query, const sc_memory_entry *entry)
{
    if (entry == nullptr) {
        return false;
    }
    if (query == nullptr) {
        return entry->superseded_by_id.len == 0 && entry->redaction_state == SC_MEMORY_REDACTION_NONE;
    }
    if (!query->include_superseded && entry->superseded_by_id.len > 0) {
        return false;
    }
    if (!query->include_redacted && entry->redaction_state == SC_MEMORY_REDACTION_REDACTED) {
        return false;
    }
    if (!query->include_deleted && entry->redaction_state == SC_MEMORY_REDACTION_DELETED) {
        return false;
    }
    if (!str_is_empty(query->namespace_name) &&
        !sc_str_equal(sc_string_as_str(&entry->namespace_name), query->namespace_name)) {
        return false;
    }
    if (!str_is_empty(query->session_id) &&
        !sc_str_equal(sc_string_as_str(&entry->session_id), query->session_id)) {
        return false;
    }
    if (!str_is_empty(query->category) &&
        !sc_str_equal(sc_string_as_str(&entry->category), query->category)) {
        return false;
    }
    if (!str_is_empty(query->query) &&
        !str_contains_casefold(sc_string_as_str(&entry->key), query->query) &&
        !str_contains_casefold(sc_string_as_str(&entry->content), query->query)) {
        return false;
    }
    return true;
}

int sc_memory_entry_compare_desc(const void *left, const void *right)
{
    const sc_memory_entry *a = left;
    const sc_memory_entry *b = right;
    int key_cmp = 0;

    if (a->score < b->score) {
        return 1;
    }
    if (a->score > b->score) {
        return -1;
    }
    if (a->importance < b->importance) {
        return 1;
    }
    if (a->importance > b->importance) {
        return -1;
    }
    if (a->timestamp.unix_ns < b->timestamp.unix_ns) {
        return 1;
    }
    if (a->timestamp.unix_ns > b->timestamp.unix_ns) {
        return -1;
    }
    key_cmp = compare_str(sc_string_as_str(&a->key), sc_string_as_str(&b->key));
    if (key_cmp != 0) {
        return key_cmp;
    }
    return compare_str(sc_string_as_str(&a->id), sc_string_as_str(&b->id));
}

static bool str_is_empty(sc_str value)
{
    return value.ptr == nullptr || value.len == 0;
}

static sc_str string_or_empty(const sc_string *string)
{
    if (string == nullptr || string->ptr == nullptr) {
        return sc_str_from_cstr("");
    }
    return sc_string_as_str(string);
}

static sc_status copy_entry_from_record(sc_allocator *alloc, const sc_memory_record *record, sc_memory_entry *out)
{
    sc_status status;
    sc_wall_time now = {0};

    *out = (sc_memory_entry){
        .struct_size = sizeof(*out),
        .score = record->score,
        .importance = record->importance,
        .redaction_state = record->redaction_state,
        .retention_policy = record->retention_policy,
        .expires_at = record->expires_at,
    };

    if (str_is_empty(record->id)) {
        status = sc_uuid_v4(alloc, &out->id);
    } else {
        status = copy_string_field(alloc, record->id, &out->id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(alloc, record->namespace_name, &out->namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(alloc, record->session_id, &out->session_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(alloc, record->category, &out->category);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(alloc, record->key, &out->key);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(alloc, record->value, &out->content);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(alloc, record->superseded_by_id, &out->superseded_by_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(alloc, record->source, &out->source);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(alloc, record->metadata_json, &out->metadata_json);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(alloc, record->content_ref, &out->content_ref);
    }
    if (sc_status_is_ok(status)) {
        status = sc_clock_wall(&now);
    }
    if (sc_status_is_ok(status)) {
        out->timestamp = now;
    }

    return status;
}

sc_status sc_memory_validate_record(const sc_memory_record *record)
{
    sc_status status;

    if (record == nullptr || str_is_empty(record->namespace_name) || str_is_empty(record->key)) {
        return sc_status_invalid_argument("sc.memory.record_invalid_argument");
    }
    if (sc_memory_key_is_reserved(record->key) || (!record->allow_sensitive_content && key_looks_secret(record->key))) {
        return sc_status_security_denied("sc.memory.secret_denied");
    }
    if (!redaction_state_valid(record->redaction_state)) {
        return sc_status_invalid_argument("sc.memory.invalid_redaction_state");
    }

    status = validate_text_field(record->namespace_name, SC_MEMORY_MAX_NAMESPACE_BYTES, "sc.memory.namespace_invalid");
    if (sc_status_is_ok(status)) {
        status = validate_text_field(record->session_id, SC_MEMORY_MAX_SESSION_BYTES, "sc.memory.session_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = validate_text_field(record->category, SC_MEMORY_MAX_CATEGORY_BYTES, "sc.memory.category_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = validate_text_field(record->key, SC_MEMORY_MAX_KEY_BYTES, "sc.memory.key_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = validate_text_field(record->value, SC_MEMORY_MAX_CONTENT_BYTES, "sc.memory.content_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = validate_text_field(record->source, SC_MEMORY_MAX_SOURCE_BYTES, "sc.memory.source_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = validate_text_field(record->metadata_json, SC_MEMORY_MAX_METADATA_BYTES, "sc.memory.metadata_invalid");
    }
    if (sc_status_is_ok(status) && !str_is_empty(record->metadata_json)) {
        sc_json_value *metadata = nullptr;
        status = sc_json_parse(sc_allocator_heap(), record->metadata_json, &metadata, nullptr);
        if (sc_status_is_ok(status) && sc_json_type_of(metadata) != SC_JSON_OBJECT) {
            status = sc_status_parse("sc.memory.metadata_expected_object");
        }
        sc_json_destroy(metadata);
    }
    return status;
}

sc_status sc_memory_validate_query(const sc_memory_query *query)
{
    sc_status status;

    if (query == nullptr) {
        return sc_status_ok();
    }
    if (query->max_query_bytes > 0 && query->query.len > query->max_query_bytes) {
        return sc_status_invalid_argument("sc.memory.query_too_large");
    }
    status = validate_text_field(query->namespace_name, SC_MEMORY_MAX_NAMESPACE_BYTES, "sc.memory.query_namespace_invalid");
    if (sc_status_is_ok(status)) {
        status = validate_text_field(query->session_id, SC_MEMORY_MAX_SESSION_BYTES, "sc.memory.query_session_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = validate_text_field(query->category, SC_MEMORY_MAX_CATEGORY_BYTES, "sc.memory.query_category_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = validate_text_field(query->query, SC_MEMORY_MAX_QUERY_BYTES, "sc.memory.query_invalid");
    }
    return status;
}

static sc_status copy_string_field(sc_allocator *alloc, sc_str value, sc_string *out)
{
    if (value.ptr == nullptr) {
        value = sc_str_from_cstr("");
    }
    return sc_string_from_str(alloc, value, out);
}

static sc_status validate_text_field(sc_str value, size_t max_len, const char *error_key)
{
    if (value.ptr == nullptr) {
        return sc_status_ok();
    }
    if (value.len > max_len) {
        return sc_status_invalid_argument(error_key);
    }
    if (!sc_str_is_valid_utf8(value)) {
        return sc_status_parse(error_key);
    }
    return sc_status_ok();
}

static bool key_looks_secret(sc_str key)
{
    static const char *needles[] = {"api_key", "apikey", "bot_token", "token", "secret", "password", "authorization", "cookie"};

    for (size_t i = 0; i < SC_ARRAY_LEN(needles); i += 1) {
        if (str_contains_casefold(key, sc_str_from_cstr(needles[i]))) {
            return true;
        }
    }
    return false;
}

static bool redaction_state_valid(sc_memory_redaction_state state)
{
    return state == SC_MEMORY_REDACTION_NONE ||
           state == SC_MEMORY_REDACTION_REDACTED ||
           state == SC_MEMORY_REDACTION_DELETED;
}

sc_status sc_memory_append_json_string(sc_string_builder *builder, sc_str value)
{
    sc_status status = sc_string_builder_append_cstr(builder, "\"");

    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        char escaped[7] = {0};
        unsigned char ch = (unsigned char)value.ptr[i];
        if (ch == '"' || ch == '\\') {
            escaped[0] = '\\';
            escaped[1] = (char)ch;
            status = sc_string_builder_append(builder, sc_str_from_parts(escaped, 2));
        } else if (ch == '\n') {
            status = sc_string_builder_append_cstr(builder, "\\n");
        } else if (ch == '\r') {
            status = sc_string_builder_append_cstr(builder, "\\r");
        } else if (ch == '\t') {
            status = sc_string_builder_append_cstr(builder, "\\t");
        } else if (ch < 0x20u) {
            int written = snprintf(escaped, sizeof(escaped), "\\u%04x", (unsigned int)ch);
            status = written == 6 ? sc_string_builder_append(builder, sc_str_from_parts(escaped, 6))
                                  : sc_status_io("sc.memory.json_escape_failed");
        } else {
            status = sc_string_builder_append(builder, sc_str_from_parts(value.ptr + i, 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    return status;
}

sc_status sc_memory_append_entry_json(sc_string_builder *builder,
                                      const sc_memory_entry *entry,
                                      const sc_memory_export_options *options)
{
    bool raw = options != nullptr && options->include_raw_sensitive_content;
    sc_str content = {0};
    sc_str metadata = {0};
    sc_status status;
    char state[32] = {0};
    int written = 0;

    if (entry == nullptr) {
        return sc_status_invalid_argument("sc.memory.entry_json_invalid_argument");
    }
    content = raw ? sc_string_as_str(&entry->content) : sc_str_from_cstr("[REDACTED]");
    metadata = raw ? sc_string_as_str(&entry->metadata_json) : sc_str_from_cstr("{}");
    if (entry->redaction_state != SC_MEMORY_REDACTION_NONE) {
        content = sc_string_as_str(&entry->content);
        metadata = sc_string_as_str(&entry->metadata_json);
    }

    status = sc_string_builder_append_cstr(builder, "{\"id\":");
    if (sc_status_is_ok(status)) {
        status = sc_memory_append_json_string(builder, sc_string_as_str(&entry->id));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, ",\"namespace\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_append_json_string(builder, sc_string_as_str(&entry->namespace_name));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, ",\"session_id\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_append_json_string(builder, sc_string_as_str(&entry->session_id));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, ",\"category\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_append_json_string(builder, sc_string_as_str(&entry->category));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, ",\"key\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_append_json_string(builder, sc_string_as_str(&entry->key));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, ",\"content\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_append_json_string(builder, content);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, ",\"metadata\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_append_json_string(builder, metadata);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, ",\"source\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_append_json_string(builder, sc_string_as_str(&entry->source));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, ",\"content_ref\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_append_json_string(builder, sc_string_as_str(&entry->content_ref));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, ",\"superseded_by\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_append_json_string(builder, sc_string_as_str(&entry->superseded_by_id));
    }
    written = snprintf(state, sizeof(state), ",\"redaction_state\":%d}", (int)entry->redaction_state);
    if (sc_status_is_ok(status)) {
        status = written > 0 && (size_t)written < sizeof(state)
                     ? sc_string_builder_append(builder, sc_str_from_parts(state, (size_t)written))
                     : sc_status_io("sc.memory.entry_json_format_failed");
    }
    return status;
}

static bool str_contains_casefold(sc_str haystack, sc_str needle)
{
    if (str_is_empty(needle)) {
        return true;
    }
    if (haystack.ptr == nullptr || haystack.len < needle.len) {
        return false;
    }
    for (size_t i = 0; i <= haystack.len - needle.len; i += 1) {
        bool matched = true;
        for (size_t j = 0; j < needle.len; j += 1) {
            unsigned char left = (unsigned char)haystack.ptr[i + j];
            unsigned char right = (unsigned char)needle.ptr[j];
            if (tolower(left) != tolower(right)) {
                matched = false;
                break;
            }
        }
        if (matched) {
            return true;
        }
    }
    return false;
}

static void store_remove_at(sc_memory_store *store, size_t index)
{
    sc_memory_entry *entries = store->entries.ptr;

    if (index >= store->entries.len) {
        return;
    }

    sc_memory_entry_clear(&entries[index]);
    if (index + 1 < store->entries.len) {
        (void)memmove(&entries[index], &entries[index + 1], (store->entries.len - index - 1) * sizeof(entries[0]));
    }
    store->entries.len -= 1;
}

static bool namespace_key_equal(const sc_memory_entry *entry, sc_str namespace_name, sc_str key)
{
    return sc_str_equal(sc_string_as_str(&entry->namespace_name), namespace_name) &&
           sc_str_equal(sc_string_as_str(&entry->key), key);
}

static int compare_str(sc_str left, sc_str right)
{
    size_t min_len = left.len < right.len ? left.len : right.len;
    int cmp = 0;

    if (min_len > 0) {
        cmp = memcmp(left.ptr, right.ptr, min_len);
    }
    if (cmp != 0) {
        return cmp;
    }
    if (left.len < right.len) {
        return -1;
    }
    if (left.len > right.len) {
        return 1;
    }
    return 0;
}
