#define _XOPEN_SOURCE 700

#include "tools/tool_internal.h"

#include <dirent.h>
#include <fnmatch.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef enum search_kind {
    SEARCH_CONTENT = 0,
    SEARCH_GLOB
} search_kind;

typedef struct search_tool {
    sc_tool_impl_context base;
    search_kind kind;
} search_tool;

static sc_status content_spec(void *impl, sc_tool_spec *out);
static sc_status glob_spec(void *impl, sc_tool_spec *out);
static sc_status search_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void search_destroy(void *impl);
static sc_status search_new(sc_allocator *alloc, const sc_tool_context *context, search_kind kind, const sc_tool_vtab *vtab, sc_tool **out);
static sc_status walk_search(search_tool *tool,
                             sc_str root,
                             sc_str query,
                             sc_string_builder *builder,
                             size_t *match_count,
                             size_t depth);
static sc_status append_match(sc_string_builder *builder, sc_str path);
static sc_status file_contains(sc_allocator *alloc, sc_str path, sc_str query, bool *out);
static sc_status join_path(sc_allocator *alloc, sc_str parent, sc_str child, sc_string *out);
static sc_status glob_matches(sc_allocator *alloc, sc_str pattern, sc_str path, bool *out);
static bool span_contains_nul(sc_str value);

static const sc_tool_vtab content_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "content_search",
    .display_name = "Content search",
    .feature_flag = "SC_TOOL_CONTENT_SEARCH",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = content_spec,
    .invoke = search_invoke,
    .destroy = search_destroy,
};

static const sc_tool_vtab glob_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "glob_search",
    .display_name = "Glob search",
    .feature_flag = "SC_TOOL_GLOB_SEARCH",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = glob_spec,
    .invoke = search_invoke,
    .destroy = search_destroy,
};

sc_status sc_tool_content_search_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return search_new(alloc, context, SEARCH_CONTENT, &content_vtab, out);
}

sc_status sc_tool_glob_search_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return search_new(alloc, context, SEARCH_GLOB, &glob_vtab, out);
}

static sc_status content_spec(void *impl, sc_tool_spec *out)
{
    const search_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.content_search.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_str_from_cstr("content_search"),
        .description = sc_str_from_cstr("tool.content_search.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = SC_TOOL_RISK_READONLY,
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_FILESYSTEM,
        .side_effect = SC_TOOL_SIDE_EFFECT_READ,
        .default_autonomy = SC_AUTONOMY_AUTONOMOUS,
        .catalog_metadata_key = sc_str_from_cstr("tool.content_search.catalog"),
    };
    return sc_status_ok();
}

static sc_status glob_spec(void *impl, sc_tool_spec *out)
{
    const search_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.glob_search.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_str_from_cstr("glob_search"),
        .description = sc_str_from_cstr("tool.glob_search.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = SC_TOOL_RISK_READONLY,
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_FILESYSTEM,
        .side_effect = SC_TOOL_SIDE_EFFECT_READ,
        .default_autonomy = SC_AUTONOMY_AUTONOMOUS,
        .catalog_metadata_key = sc_str_from_cstr("tool.glob_search.catalog"),
    };
    return sc_status_ok();
}

static sc_status search_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    search_tool *tool = impl;
    sc_str path = {0};
    sc_str query = {0};
    sc_string resolved = {0};
    sc_string output = {0};
    sc_string_builder builder = {0};
    size_t match_count = 0;
    sc_status status;
    sc_str query_name = {0};
    sc_str tool_name = {0};

    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.search_tool.invalid_argument");
    }
    query_name = tool->kind == SEARCH_CONTENT ? sc_str_from_cstr("query") : sc_str_from_cstr("pattern");
    tool_name = tool->kind == SEARCH_CONTENT ? sc_str_from_cstr("content_search") : sc_str_from_cstr("glob_search");

    status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("path"), &path);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, query_name, &query);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        tool_name,
                                        SC_TOOL_RISK_READONLY,
                                        path,
                                        true,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        status = sc_workspace_resolve(tool->base.context.policy, path, true, alloc, &resolved);
    }
    if (sc_status_is_ok(status)) {
        sc_string_builder_init(&builder, alloc);
        status = walk_search(tool, sc_string_as_str(&resolved), query, &builder, &match_count, 0);
    }
    if (sc_status_is_ok(status) && match_count == 0) {
        status = sc_string_builder_append_cstr(&builder, "");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &output);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&output), true);
    }

    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, tool_name, query, sc_string_as_str(&out->output), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base, tool_name, query, sc_str_from_cstr("error"), false, status);
    }

    sc_string_clear(&output);
    sc_string_clear(&resolved);
    return status;
}

static void search_destroy(void *impl)
{
    search_tool *tool = impl;
    sc_allocator *alloc = nullptr;
    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc;
    sc_tool_impl_context_clear(&tool->base);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(search_tool));
}

static sc_status search_new(sc_allocator *alloc, const sc_tool_context *context, search_kind kind, const sc_tool_vtab *vtab, sc_tool **out)
{
    search_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.search_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(search_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (search_tool){.kind = kind};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = sc_tool_schema_two_strings(alloc,
                                            sc_str_from_cstr("path"),
                                            true,
                                            kind == SEARCH_CONTENT ? sc_str_from_cstr("query") : sc_str_from_cstr("pattern"),
                                            true,
                                            &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        search_destroy(tool);
    }
    return status;
}

static sc_status walk_search(search_tool *tool,
                             sc_str root,
                             sc_str query,
                             sc_string_builder *builder,
                             size_t *match_count,
                             size_t depth)
{
    char path_buf[4096] = {0};
    DIR *dir = nullptr;
    struct dirent *entry = nullptr;
    struct stat st = {0};

    if (depth > 32) {
        return sc_status_invalid_argument("sc.search_tool.max_depth");
    }
    if (match_count != nullptr && *match_count >= 1024) {
        return sc_status_invalid_argument("sc.search_tool.max_matches");
    }
    if (root.len >= sizeof(path_buf)) {
        return sc_status_invalid_argument("sc.search_tool.path_too_long");
    }
    (void)memcpy(path_buf, root.ptr, root.len);
    dir = opendir(path_buf);
    if (dir == nullptr) {
        if (stat(path_buf, &st) != 0) {
            return sc_status_io("sc.search_tool.stat_failed");
        }
        if (S_ISREG(st.st_mode)) {
            bool matched = false;
            sc_status status = tool->kind == SEARCH_CONTENT
                                   ? file_contains(builder->bytes.alloc, root, query, &matched)
                                   : glob_matches(builder->bytes.alloc, query, sc_str_from_cstr(path_buf), &matched);
            if (!sc_status_is_ok(status)) {
                return status;
            }
            if (matched) {
                *match_count += 1;
                return append_match(builder, root);
            }
        }
        return sc_status_ok();
    }

    while ((entry = readdir(dir)) != nullptr) {
        sc_string child = {0};
        sc_status status;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        status = join_path(builder->bytes.alloc, root, sc_str_from_cstr(entry->d_name), &child);
        if (!sc_status_is_ok(status)) {
            (void)closedir(dir);
            return status;
        }
        if (child.ptr == nullptr) {
            (void)closedir(dir);
            return sc_status_no_memory();
        }
        if (lstat(child.ptr, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                status = walk_search(tool, sc_string_as_str(&child), query, builder, match_count, depth + 1);
            } else if (S_ISREG(st.st_mode)) {
                bool matched = false;
                status = tool->kind == SEARCH_CONTENT
                             ? file_contains(builder->bytes.alloc, sc_string_as_str(&child), query, &matched)
                             : glob_matches(builder->bytes.alloc, query, sc_string_as_str(&child), &matched);
                if (sc_status_is_ok(status) && matched) {
                    *match_count += 1;
                    status = append_match(builder, sc_string_as_str(&child));
                }
            }
        }
        sc_string_clear(&child);
        if (!sc_status_is_ok(status)) {
            (void)closedir(dir);
            return status;
        }
    }
    if (closedir(dir) != 0) {
        return sc_status_io("sc.search_tool.close_failed");
    }
    return sc_status_ok();
}

static sc_status append_match(sc_string_builder *builder, sc_str path)
{
    sc_status status = sc_string_builder_append(builder, path);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\n");
    }
    return status;
}

static sc_status file_contains(sc_allocator *alloc, sc_str path, sc_str query, bool *out)
{
    FILE *file = nullptr;
    sc_string query_cstr = {0};
    char buffer[1024] = {0};
    bool found = false;
    sc_status status;

    if (query.ptr == nullptr || query.len == 0 || out == nullptr) {
        return sc_status_invalid_argument("sc.search_tool.query_invalid");
    }
    if (span_contains_nul(query)) {
        return sc_status_invalid_argument("sc.search_tool.query_invalid_nul");
    }
    *out = false;
    status = sc_string_from_str(alloc, query, &query_cstr);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    file = fopen(path.ptr, "rb");
    if (file == nullptr) {
        status = sc_status_io("sc.search_tool.open_failed");
        goto cleanup;
    }
    while (!found && fgets(buffer, sizeof(buffer), file) != nullptr) {
        if (strstr(buffer, query_cstr.ptr) != nullptr) {
            found = true;
        }
    }
    if (ferror(file) != 0) {
        status = sc_status_io("sc.search_tool.read_failed");
        goto cleanup;
    }
    (void)fclose(file);
    file = nullptr;
    *out = found;

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    sc_string_clear(&query_cstr);
    return status;
}

static sc_status glob_matches(sc_allocator *alloc, sc_str pattern, sc_str path, bool *out)
{
    sc_string pattern_cstr = {0};
    sc_status status;

    if (pattern.ptr == nullptr || pattern.len == 0 || path.ptr == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.search_tool.pattern_invalid");
    }
    if (span_contains_nul(pattern)) {
        return sc_status_invalid_argument("sc.search_tool.pattern_invalid_nul");
    }
    *out = false;
    status = sc_string_from_str(alloc, pattern, &pattern_cstr);
    if (sc_status_is_ok(status)) {
        *out = fnmatch(pattern_cstr.ptr, path.ptr, 0) == 0;
    }
    sc_string_clear(&pattern_cstr);
    return status;
}

static bool span_contains_nul(sc_str value)
{
    return value.ptr != nullptr && memchr(value.ptr, '\0', value.len) != nullptr;
}

static sc_status join_path(sc_allocator *alloc, sc_str parent, sc_str child, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, parent);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, child);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}
