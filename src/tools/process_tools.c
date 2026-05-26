#include "tools/tool_internal.h"
#include "tools/process_runner.h"

#include <stdio.h>
#include <unistd.h>

typedef enum process_tool_kind {
    PROCESS_TOOL_PDF = 0
} process_tool_kind;

typedef struct process_tool {
    sc_tool_impl_context base;
    process_tool_kind kind;
} process_tool;

static sc_status process_tool_spec(void *impl, sc_tool_spec *out);
static sc_status process_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void process_tool_destroy(void *impl);
static sc_status process_tool_new(sc_allocator *alloc,
                                  const sc_tool_context *context,
                                  process_tool_kind kind,
                                  sc_tool **out);
static sc_status invoke_pdf(process_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);

static const sc_tool_vtab pdf_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "pdf_extract",
    .display_name = "PDF extract",
    .feature_flag = "SC_TOOL_PDF_EXTRACT",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = process_tool_spec,
    .invoke = process_tool_invoke,
    .destroy = process_tool_destroy,
};

sc_status sc_tool_pdf_extract_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return process_tool_new(alloc, context, PROCESS_TOOL_PDF, out);
}

static sc_status process_tool_spec(void *impl, sc_tool_spec *out)
{
    const process_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.process_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_str_from_cstr("pdf_extract"),
        .description = sc_str_from_cstr("tool.pdf_extract.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = SC_TOOL_RISK_READONLY,
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_FILESYSTEM,
        .side_effect = SC_TOOL_SIDE_EFFECT_READ,
        .default_autonomy = SC_AUTONOMY_AUTONOMOUS,
        .catalog_metadata_key = sc_str_from_cstr("tool.pdf_extract.catalog"),
    };
    return sc_status_ok();
}

static sc_status process_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    process_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.process_tool.invalid_argument");
    }
    return invoke_pdf(tool, call, alloc, out);
}

static void process_tool_destroy(void *impl)
{
    process_tool *tool = impl;
    sc_allocator *alloc = nullptr;
    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc;
    sc_tool_impl_context_clear(&tool->base);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(process_tool));
}

static sc_status process_tool_new(sc_allocator *alloc,
                                  const sc_tool_context *context,
                                  process_tool_kind kind,
                                  sc_tool **out)
{
    process_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.process_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(process_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (process_tool){.kind = kind};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = sc_tool_schema_string_required(alloc,
                                                sc_str_from_cstr("path"),
                                                sc_str_from_cstr("tool.pdf_extract.description"),
                                                &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, &pdf_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        process_tool_destroy(tool);
    }
    return status;
}

static sc_status invoke_pdf(process_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str path = {0};
    sc_string resolved = {0};
    sc_string output = {0};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("path"), &path);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("pdf_extract"),
                                        SC_TOOL_RISK_READONLY,
                                        path,
                                        true,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        status = sc_workspace_resolve(tool->base.context.policy, path, true, alloc, &resolved);
    }
    if (sc_status_is_ok(status) && access("/usr/bin/pdftotext", X_OK) != 0 && access("/bin/pdftotext", X_OK) != 0) {
        status = sc_status_unsupported("sc.pdf_extract_tool.backend_unavailable");
    }
    if (sc_status_is_ok(status)) {
        sc_str args[] = {sc_string_as_str(&resolved), sc_str_from_cstr("-")};
        sc_tool_process_request request = {
            .executable = access("/usr/bin/pdftotext", X_OK) == 0 ? sc_str_from_cstr("/usr/bin/pdftotext") : sc_str_from_cstr("/bin/pdftotext"),
            .args = args,
            .arg_count = SC_ARRAY_LEN(args),
            .cwd = sc_string_as_str(&tool->base.context.policy->workspace_root),
            .policy = tool->base.context.policy,
            .timeout_ms = tool->base.context.timeout_ms == 0 ? 30000 : tool->base.context.timeout_ms,
            .max_output_bytes = tool->base.context.max_output_bytes == 0 ? 4096 : tool->base.context.max_output_bytes,
            .cancel_token = call == nullptr ? tool->base.context.cancel_token : call->cancel_token,
        };
        status = sc_tool_process_run(alloc, &request, &output);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&output), true);
    }
    (void)sc_tool_record_receipt_status(&tool->base,
                                        sc_str_from_cstr("pdf_extract"),
                                        path,
                                        sc_status_is_ok(status) ? sc_str_from_cstr("text extracted") : sc_str_from_cstr("error"),
                                        sc_status_is_ok(status),
                                        status);
    sc_string_clear(&resolved);
    sc_string_clear(&output);
    return status;
}
