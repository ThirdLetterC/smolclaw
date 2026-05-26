#include "tools/tool_internal.h"

#include <stdio.h>
#include <stdckdint.h>

typedef struct file_read_tool {
    sc_tool_impl_context base;
} file_read_tool;

static sc_status file_read_spec(void *impl, sc_tool_spec *out);
static sc_status file_read_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void file_read_destroy(void *impl);
static sc_status read_file_limited(sc_allocator *alloc, sc_str path, size_t max_bytes, sc_string *out);

static const sc_tool_vtab file_read_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "file_read",
    .display_name = "File read",
    .feature_flag = "SC_TOOL_FILE_READ",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = file_read_spec,
    .invoke = file_read_invoke,
    .destroy = file_read_destroy,
};

sc_status sc_tool_file_read_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    file_read_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.file_read.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(file_read_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (file_read_tool){0};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = sc_tool_schema_string_required(alloc,
                                                sc_str_from_cstr("path"),
                                                sc_str_from_cstr("tool.file_read.description"),
                                                &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, &file_read_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        file_read_destroy(tool);
    }
    return status;
}

static sc_status file_read_spec(void *impl, sc_tool_spec *out)
{
    const file_read_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.file_read.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_str_from_cstr("file_read"),
        .description = sc_str_from_cstr("tool.file_read.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = SC_TOOL_RISK_READONLY,
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_FILESYSTEM,
        .side_effect = SC_TOOL_SIDE_EFFECT_READ,
        .default_autonomy = SC_AUTONOMY_AUTONOMOUS,
        .catalog_metadata_key = sc_str_from_cstr("tool.file_read.catalog"),
    };
    return sc_status_ok();
}

static sc_status file_read_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    file_read_tool *tool = impl;
    sc_str path = {0};
    sc_string resolved = {0};
    sc_string content = {0};
    sc_status status;

    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.file_read.invalid_argument");
    }

    status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("path"), &path);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("file_read"),
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
        status = read_file_limited(alloc,
                                   sc_string_as_str(&resolved),
                                   tool->base.context.max_output_bytes == 0 ? 4096 : tool->base.context.max_output_bytes,
                                   &content);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&content), true);
    }

    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base,
                                        sc_str_from_cstr("file_read"),
                                        path,
                                        sc_string_as_str(&out->output),
                                        true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("file_read"),
                                            path,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    sc_string_clear(&content);
    sc_string_clear(&resolved);
    return status;
}

static void file_read_destroy(void *impl)
{
    file_read_tool *tool = impl;
    sc_allocator *alloc = nullptr;
    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc;
    sc_tool_impl_context_clear(&tool->base);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(file_read_tool));
}

static sc_status read_file_limited(sc_allocator *alloc, sc_str path, size_t max_bytes, sc_string *out)
{
    FILE *file = nullptr;
    sc_string text = {0};
    size_t allocation_size = 0;
    size_t read_count = 0;
    sc_status status = sc_status_ok();

    if (path.ptr == nullptr || out == nullptr || path.len == 0) {
        return sc_status_invalid_argument("sc.file_read.path_invalid");
    }
    if (ckd_add(&allocation_size, max_bytes, 1)) {
        return sc_status_invalid_argument("sc.file_read.max_bytes_overflow");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    file = fopen(path.ptr, "rb");
    if (file == nullptr) {
        status = sc_status_io("sc.file_read.open_failed");
        goto cleanup;
    }
    text.ptr = sc_alloc(alloc, allocation_size, _Alignof(char));
    if (text.ptr == nullptr) {
        status = sc_status_no_memory();
        goto cleanup;
    }
    text.alloc = alloc;
    read_count = fread(text.ptr, 1, max_bytes, file);
    if (ferror(file) != 0) {
        status = sc_status_io("sc.file_read.read_failed");
        goto cleanup;
    }
    if (fclose(file) != 0) {
        file = nullptr;
        status = sc_status_io("sc.file_read.close_failed");
        goto cleanup;
    }
    file = nullptr;
    text.ptr[read_count] = '\0';
    text.len = read_count;
    *out = text;
    text = (sc_string){0};

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    sc_string_clear(&text);
    return status;
}
