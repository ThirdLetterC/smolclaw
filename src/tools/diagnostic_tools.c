#include "tools/tool_internal.h"

#include "net/http_client.h"

#include <dirent.h>
#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>

typedef enum diagnostic_tool_kind {
    DIAG_TOOL_DIAGNOSTICS = 0,
    DIAG_POLICY_EXPLAIN,
    DIAG_TOOL_REGISTRY_LIST,
    DIAG_DEPENDENCY_STATUS,
    DIAG_CAPABILITY_MATRIX,
    DIAG_RESOURCE_USAGE,
    DIAG_APPROVAL_TEST
} diagnostic_tool_kind;

typedef struct diagnostic_tool {
    sc_tool_impl_context base;
    diagnostic_tool_kind kind;
} diagnostic_tool;

typedef struct diagnostic_tool_descriptor {
    const char *name;
    sc_tool_risk risk;
    sc_tool_side_effect side_effect;
    sc_tool_capability_category capability;
} diagnostic_tool_descriptor;

static sc_status diagnostic_spec(void *impl, sc_tool_spec *out);
static sc_status diagnostic_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void diagnostic_destroy(void *impl);
static sc_status diagnostic_new(sc_allocator *alloc,
                                const sc_tool_context *context,
                                diagnostic_tool_kind kind,
                                const sc_tool_vtab *vtab,
                                sc_tool **out);
static sc_status build_schema(sc_allocator *alloc, diagnostic_tool_kind kind, sc_json_value **out);
static sc_str diagnostic_name(diagnostic_tool_kind kind);
static sc_str diagnostic_description(diagnostic_tool_kind kind);
static sc_status invoke_tool_diagnostics(diagnostic_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_policy_explain(diagnostic_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_tool_registry_list(diagnostic_tool *tool, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_dependency_status(diagnostic_tool *tool, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_capability_matrix(diagnostic_tool *tool, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_resource_usage(diagnostic_tool *tool, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_approval_test(diagnostic_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status append_descriptor(sc_string_builder *builder, const diagnostic_tool_descriptor *descriptor);
static sc_status append_live_tool(sc_string_builder *builder, sc_tool *tool);
static sc_status append_tool_status(sc_string_builder *builder, const diagnostic_tool *tool, const diagnostic_tool_descriptor *descriptor);
static sc_status append_browser_backend_status(sc_string_builder *builder, const diagnostic_tool *tool);
static sc_status append_policy_decision(sc_string_builder *builder,
                                        const diagnostic_tool *tool,
                                        sc_str name,
                                        sc_tool_risk risk,
                                        sc_str path,
                                        sc_str url,
                                        sc_str shell,
                                        sc_str device,
                                        bool *allowed);
static sc_status append_format(sc_string_builder *builder, const char *format, const char *value);
static sc_status append_size_line(sc_string_builder *builder, const char *key, size_t value);
static sc_status append_i64_line(sc_string_builder *builder, const char *key, int64_t value);
static sc_status append_u64_line(sc_string_builder *builder, const char *key, uint64_t value);
static const diagnostic_tool_descriptor *find_descriptor(sc_str name);
static const char *risk_name(sc_tool_risk risk);
static const char *side_effect_name(sc_tool_side_effect side_effect);
static const char *capability_name(sc_tool_capability_category capability);
static const char *status_code_name(sc_status_code code);
static bool command_available(const char *path);
static sc_status diagnostic_get_config_string(const diagnostic_tool *tool, sc_str path, sc_allocator *alloc, sc_string *out);
static sc_status diagnostic_http_probe(sc_str url, sc_allocator *alloc, bool *out);
static bool browser_enabled(const diagnostic_tool *tool);
static bool cron_enabled(const diagnostic_tool *tool);
static bool memory_enabled(const diagnostic_tool *tool);
static const char *memory_backend_name(const diagnostic_tool *tool);
static sc_tool_risk risk_for_name(sc_str name);
static int64_t timeval_to_ms(struct timeval value);
static size_t live_tool_count(const diagnostic_tool *tool);
static bool read_current_rss_kb(size_t *out);
static bool count_open_fds(size_t *out);

static const sc_tool_vtab tool_diagnostics_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "tool_diagnostics",
    .display_name = "Tool diagnostics",
    .feature_flag = "SC_TOOL_DIAGNOSTICS",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = diagnostic_spec,
    .invoke = diagnostic_invoke,
    .destroy = diagnostic_destroy,
};

static const sc_tool_vtab policy_explain_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "policy_explain",
    .display_name = "Policy explain",
    .feature_flag = "SC_TOOL_POLICY_EXPLAIN",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = diagnostic_spec,
    .invoke = diagnostic_invoke,
    .destroy = diagnostic_destroy,
};

static const sc_tool_vtab tool_registry_list_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "tool_registry_list",
    .display_name = "Tool registry list",
    .feature_flag = "SC_TOOL_REGISTRY_LIST",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = diagnostic_spec,
    .invoke = diagnostic_invoke,
    .destroy = diagnostic_destroy,
};

static const sc_tool_vtab dependency_status_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "dependency_status",
    .display_name = "Dependency status",
    .feature_flag = "SC_TOOL_DEPENDENCY_STATUS",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = diagnostic_spec,
    .invoke = diagnostic_invoke,
    .destroy = diagnostic_destroy,
};

static const sc_tool_vtab capability_matrix_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "capability_matrix",
    .display_name = "Capability matrix",
    .feature_flag = "SC_TOOL_CAPABILITY_MATRIX",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = diagnostic_spec,
    .invoke = diagnostic_invoke,
    .destroy = diagnostic_destroy,
};

static const sc_tool_vtab resource_usage_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "resource_usage",
    .display_name = "Resource usage",
    .feature_flag = "SC_TOOL_RESOURCE_USAGE",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = diagnostic_spec,
    .invoke = diagnostic_invoke,
    .destroy = diagnostic_destroy,
};

static const sc_tool_vtab approval_test_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "approval_test",
    .display_name = "Approval test",
    .feature_flag = "SC_TOOL_APPROVAL_TEST",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = diagnostic_spec,
    .invoke = diagnostic_invoke,
    .destroy = diagnostic_destroy,
};

static const diagnostic_tool_descriptor tool_descriptors[] = {
    {"tool_diagnostics", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_NONE},
    {"policy_explain", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_NONE},
    {"tool_registry_list", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_NONE},
    {"dependency_status", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_NONE},
    {"capability_matrix", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_NONE},
    {"resource_usage", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_NONE},
    {"approval_test", SC_TOOL_RISK_SIDE_EFFECT, SC_TOOL_SIDE_EFFECT_WRITE, SC_TOOL_CAPABILITY_NONE},
    {"file_read", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_FILESYSTEM},
    {"file_write", SC_TOOL_RISK_SIDE_EFFECT, SC_TOOL_SIDE_EFFECT_WRITE, SC_TOOL_CAPABILITY_FILESYSTEM},
    {"file_list", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_FILESYSTEM},
    {"content_search", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_FILESYSTEM},
    {"glob_search", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_FILESYSTEM},
    {"shell", SC_TOOL_RISK_SHELL, SC_TOOL_SIDE_EFFECT_PROCESS, SC_TOOL_CAPABILITY_PROCESS},
    {"http", SC_TOOL_RISK_NETWORK, SC_TOOL_SIDE_EFFECT_NETWORK, SC_TOOL_CAPABILITY_NETWORK},
    {"web_search", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_NETWORK, SC_TOOL_CAPABILITY_NETWORK},
    {"browser", SC_TOOL_RISK_NETWORK, SC_TOOL_SIDE_EFFECT_PROCESS, SC_TOOL_CAPABILITY_BROWSER},
    {"browser_screenshot", SC_TOOL_RISK_NETWORK, SC_TOOL_SIDE_EFFECT_PROCESS, SC_TOOL_CAPABILITY_BROWSER},
    {"pdf_extract", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_FILESYSTEM},
    {"time", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_NONE},
    {"memory_store", SC_TOOL_RISK_SIDE_EFFECT, SC_TOOL_SIDE_EFFECT_WRITE, SC_TOOL_CAPABILITY_MEMORY},
    {"memory_recall", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_MEMORY},
    {"memory_search", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_MEMORY},
    {"memory_pin", SC_TOOL_RISK_SIDE_EFFECT, SC_TOOL_SIDE_EFFECT_WRITE, SC_TOOL_CAPABILITY_MEMORY},
    {"memory_forget", SC_TOOL_RISK_SIDE_EFFECT, SC_TOOL_SIDE_EFFECT_WRITE, SC_TOOL_CAPABILITY_MEMORY},
    {"memory_export", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_MEMORY},
    {"memory_purge", SC_TOOL_RISK_SIDE_EFFECT, SC_TOOL_SIDE_EFFECT_DESTRUCTIVE, SC_TOOL_CAPABILITY_MEMORY},
    {"sop_inspect", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_NONE},
    {"sop_advance", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_NONE},
    {"cron_list", SC_TOOL_RISK_READONLY, SC_TOOL_SIDE_EFFECT_READ, SC_TOOL_CAPABILITY_NONE},
    {"cron_upsert", SC_TOOL_RISK_SIDE_EFFECT, SC_TOOL_SIDE_EFFECT_WRITE, SC_TOOL_CAPABILITY_NONE},
    {"cron_remove", SC_TOOL_RISK_SIDE_EFFECT, SC_TOOL_SIDE_EFFECT_WRITE, SC_TOOL_CAPABILITY_NONE},
};

sc_status sc_tool_tool_diagnostics_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return diagnostic_new(alloc, context, DIAG_TOOL_DIAGNOSTICS, &tool_diagnostics_vtab, out);
}

sc_status sc_tool_policy_explain_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return diagnostic_new(alloc, context, DIAG_POLICY_EXPLAIN, &policy_explain_vtab, out);
}

sc_status sc_tool_tool_registry_list_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return diagnostic_new(alloc, context, DIAG_TOOL_REGISTRY_LIST, &tool_registry_list_vtab, out);
}

sc_status sc_tool_dependency_status_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return diagnostic_new(alloc, context, DIAG_DEPENDENCY_STATUS, &dependency_status_vtab, out);
}

sc_status sc_tool_capability_matrix_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return diagnostic_new(alloc, context, DIAG_CAPABILITY_MATRIX, &capability_matrix_vtab, out);
}

sc_status sc_tool_resource_usage_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return diagnostic_new(alloc, context, DIAG_RESOURCE_USAGE, &resource_usage_vtab, out);
}

sc_status sc_tool_approval_test_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return diagnostic_new(alloc, context, DIAG_APPROVAL_TEST, &approval_test_vtab, out);
}

static sc_status diagnostic_new(sc_allocator *alloc,
                                const sc_tool_context *context,
                                diagnostic_tool_kind kind,
                                const sc_tool_vtab *vtab,
                                sc_tool **out)
{
    diagnostic_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.diagnostic_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(diagnostic_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (diagnostic_tool){.kind = kind};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = build_schema(alloc, kind, &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        diagnostic_destroy(tool);
    }
    return status;
}

static sc_status build_schema(sc_allocator *alloc, diagnostic_tool_kind kind, sc_json_value **out)
{
    if (kind == DIAG_TOOL_DIAGNOSTICS) {
        return sc_tool_schema_string(alloc, sc_str_from_cstr("tool"), false, out);
    }
    if (kind == DIAG_APPROVAL_TEST) {
        return sc_tool_schema_string(alloc, sc_str_from_cstr("message"), false, out);
    }
    if (kind == DIAG_POLICY_EXPLAIN) {
        return sc_tool_schema_four_strings(alloc,
                                           sc_str_from_cstr("tool"),
                                           true,
                                           sc_str_from_cstr("path"),
                                           false,
                                           sc_str_from_cstr("url"),
                                           false,
                                           sc_str_from_cstr("shell"),
                                           false,
                                           out);
    }
    return sc_json_schema_object(alloc, out);
}

static sc_status diagnostic_spec(void *impl, sc_tool_spec *out)
{
    const diagnostic_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.diagnostic_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = diagnostic_name(tool->kind),
        .description = diagnostic_description(tool->kind),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = tool->kind == DIAG_APPROVAL_TEST ? SC_TOOL_RISK_SIDE_EFFECT : SC_TOOL_RISK_READONLY,
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_NONE,
        .side_effect = tool->kind == DIAG_APPROVAL_TEST ? SC_TOOL_SIDE_EFFECT_WRITE : SC_TOOL_SIDE_EFFECT_READ,
        .default_autonomy = tool->kind == DIAG_APPROVAL_TEST ? SC_AUTONOMY_SUPERVISED : SC_AUTONOMY_AUTONOMOUS,
        .catalog_metadata_key = sc_str_from_cstr("tool.diagnostics.catalog"),
    };
    return sc_status_ok();
}

static sc_status diagnostic_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    diagnostic_tool *tool = impl;
    sc_status status;

    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.diagnostic_tool.invalid_argument");
    }
    status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        diagnostic_name(tool->kind),
                                        SC_TOOL_RISK_READONLY,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        switch (tool->kind) {
        case DIAG_TOOL_DIAGNOSTICS:
            status = invoke_tool_diagnostics(tool, call, alloc, out);
            break;
        case DIAG_POLICY_EXPLAIN:
            status = invoke_policy_explain(tool, call, alloc, out);
            break;
        case DIAG_TOOL_REGISTRY_LIST:
            status = invoke_tool_registry_list(tool, alloc, out);
            break;
        case DIAG_DEPENDENCY_STATUS:
            status = invoke_dependency_status(tool, alloc, out);
            break;
        case DIAG_CAPABILITY_MATRIX:
            status = invoke_capability_matrix(tool, alloc, out);
            break;
        case DIAG_RESOURCE_USAGE:
            status = invoke_resource_usage(tool, alloc, out);
            break;
        case DIAG_APPROVAL_TEST:
            status = invoke_approval_test(tool, call, alloc, out);
            break;
        }
    }
    (void)sc_tool_record_receipt_status(&tool->base,
                                        diagnostic_name(tool->kind),
                                        sc_str_from_cstr("diagnostic"),
                                        sc_status_is_ok(status) ? sc_str_from_cstr("reported") : sc_str_from_cstr("error"),
                                        sc_status_is_ok(status),
                                        status);
    return status;
}

static sc_status invoke_tool_diagnostics(diagnostic_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_string_builder builder = {0};
    sc_string text = {0};
    sc_str requested = {0};
    sc_status status;

    (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("tool"), &requested);
    sc_string_builder_init(&builder, alloc);
    if (requested.len > 0) {
        const diagnostic_tool_descriptor *descriptor = find_descriptor(requested);
        status = descriptor == nullptr ? sc_string_builder_append_cstr(&builder, "known=false\nerror_key=sc.diagnostic_tool.unknown_tool\n") :
                                         append_tool_status(&builder, tool, descriptor);
    } else {
        status = sc_string_builder_append_cstr(&builder, "known_tools=\n");
        for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(tool_descriptors); i += 1) {
            status = append_tool_status(&builder, tool, &tool_descriptors[i]);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &text);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&text), true);
    }
    sc_string_clear(&text);
    return status;
}

static sc_status invoke_policy_explain(diagnostic_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str name = {0};
    sc_str path = {0};
    sc_str url = {0};
    sc_str shell = {0};
    sc_string_builder builder = {0};
    sc_string text = {0};
    bool allowed = false;
    sc_status status = sc_tool_get_string_arg(call, sc_str_from_cstr("tool"), &name);

    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("path"), &path);
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("url"), &url);
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("shell"), &shell);
        sc_string_builder_init(&builder, alloc);
        status = append_policy_decision(&builder, tool, name, risk_for_name(name), path, url, shell, sc_str_from_cstr(""), &allowed);
    }
    if (sc_status_is_ok(status)) {
        status = append_format(&builder, "decision=%s\n", allowed ? "allowed" : "denied");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &text);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&text), true);
    }
    sc_string_clear(&text);
    return status;
}

static sc_status invoke_tool_registry_list(diagnostic_tool *tool, sc_allocator *alloc, sc_tool_result *out)
{
    sc_string_builder builder = {0};
    sc_string text = {0};
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    if (tool->base.context.tools != nullptr && tool->base.context.tool_capacity > 0) {
        status = sc_string_builder_append_cstr(&builder, "tool registry live\n");
        for (size_t i = 0; sc_status_is_ok(status) && i < tool->base.context.tool_capacity; i += 1) {
            if (tool->base.context.tools[i] != nullptr) {
                status = append_live_tool(&builder, tool->base.context.tools[i]);
            }
        }
    } else {
        status = sc_string_builder_append_cstr(&builder, "tool registry descriptors\n");
        for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(tool_descriptors); i += 1) {
            status = append_descriptor(&builder, &tool_descriptors[i]);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &text);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &((diagnostic_tool *)tool)->base, out, sc_string_as_str(&text), true);
    }
    sc_string_clear(&text);
    return status;
}

static sc_status invoke_dependency_status(diagnostic_tool *tool, sc_allocator *alloc, sc_tool_result *out)
{
    sc_string_builder builder = {0};
    sc_string text = {0};
    sc_status status;
    (void)tool;

    sc_string_builder_init(&builder, alloc);
    status = append_format(&builder,
                           "lightpanda=%s\n",
                           (command_available("/usr/bin/lightpanda") || command_available("/usr/local/bin/lightpanda")) ? "available" : "unavailable");
    if (sc_status_is_ok(status)) {
        status = append_format(&builder,
                               "pdftotext=%s\n",
                               (command_available("/usr/bin/pdftotext") || command_available("/bin/pdftotext")) ? "available" : "unavailable");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "http_backend=compiled\nsqlite_backend=compiled\n");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &text);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&text), true);
    }
    sc_string_clear(&text);
    return status;
}

static sc_status invoke_capability_matrix(diagnostic_tool *tool, sc_allocator *alloc, sc_tool_result *out)
{
    sc_string_builder builder = {0};
    sc_string text = {0};
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "capability status detail\n");
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(tool_descriptors); i += 1) {
        status = append_tool_status(&builder, tool, &tool_descriptors[i]);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &text);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&text), true);
    }
    sc_string_clear(&text);
    return status;
}

static sc_status invoke_resource_usage(diagnostic_tool *tool, sc_allocator *alloc, sc_tool_result *out)
{
    struct rusage usage = {0};
    sc_string_builder builder = {0};
    sc_string text = {0};
    size_t current_rss_kb = 0;
    size_t open_fd_count = 0;
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "resource_usage\n");
    if (sc_status_is_ok(status)) {
        if (getrusage(RUSAGE_SELF, &usage) != 0) {
            status = sc_status_io("sc.resource_usage.rusage_failed");
        }
    }
    if (sc_status_is_ok(status)) {
        status = append_i64_line(&builder, "process_user_cpu_ms", timeval_to_ms(usage.ru_utime));
    }
    if (sc_status_is_ok(status)) {
        status = append_i64_line(&builder, "process_system_cpu_ms", timeval_to_ms(usage.ru_stime));
    }
    if (sc_status_is_ok(status)) {
        status = append_i64_line(&builder, "process_max_rss_kb", (int64_t)usage.ru_maxrss);
    }
    if (sc_status_is_ok(status)) {
        if (read_current_rss_kb(&current_rss_kb)) {
            status = append_size_line(&builder, "process_current_rss_kb", current_rss_kb);
        } else {
            status = sc_string_builder_append_cstr(&builder, "process_current_rss_kb=unknown\n");
        }
    }
    if (sc_status_is_ok(status)) {
        if (count_open_fds(&open_fd_count)) {
            status = append_size_line(&builder, "process_open_fd_count", open_fd_count);
        } else {
            status = sc_string_builder_append_cstr(&builder, "process_open_fd_count=unknown\n");
        }
    }
    if (sc_status_is_ok(status)) {
        status = append_size_line(&builder, "tool_registry_live", live_tool_count(tool));
    }
    if (sc_status_is_ok(status)) {
        status = append_size_line(&builder, "tool_registry_capacity", tool->base.context.tool_capacity);
    }
    if (sc_status_is_ok(status)) {
        status = append_size_line(&builder, "tool_max_output_bytes", tool->base.context.max_output_bytes);
    }
    if (sc_status_is_ok(status)) {
        status = append_size_line(&builder, "tool_max_arg_bytes", tool->base.context.max_arg_bytes);
    }
    if (sc_status_is_ok(status)) {
        status = append_i64_line(&builder, "tool_timeout_ms", tool->base.context.timeout_ms);
    }
    if (sc_status_is_ok(status)) {
        status = append_size_line(&builder,
                                  "receipt_count",
                                  tool->base.context.receipts == nullptr ? 0 : tool->base.context.receipts->receipts.len);
    }
    if (sc_status_is_ok(status)) {
        status = append_format(&builder, "memory_backend=%s\n", memory_backend_name(tool));
    }
    if (sc_status_is_ok(status)) {
        status = append_u64_line(&builder, "resource_schema_version", UINT64_C(1));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &text);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&text), true);
    }
    sc_string_clear(&text);
    return status;
}

static sc_status invoke_approval_test(diagnostic_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str message = {0};
    sc_string_builder builder = {0};
    sc_string text = {0};
    sc_status status;

    (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("message"), &message);
    status = sc_tool_security_check(&tool->base,
                                    sc_str_from_cstr("approval_test"),
                                    SC_TOOL_RISK_SIDE_EFFECT,
                                    sc_str_from_cstr(""),
                                    false,
                                    sc_str_from_cstr(""),
                                    sc_str_from_cstr(""));
    if (sc_status_is_ok(status)) {
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append_cstr(&builder, "approved=true\nmessage=");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, message.len == 0 ? sc_str_from_cstr("ok") : message);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\n");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &text);
        } else {
            sc_string_builder_clear(&builder);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&text), true);
    }
    sc_string_clear(&text);
    return status;
}

static sc_status append_descriptor(sc_string_builder *builder, const diagnostic_tool_descriptor *descriptor)
{
    sc_status status = append_format(builder, "name=%s ", descriptor->name);
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "risk=%s ", risk_name(descriptor->risk));
    }
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "side_effect=%s ", side_effect_name(descriptor->side_effect));
    }
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "capability=%s\n", capability_name(descriptor->capability));
    }
    return status;
}

static sc_status append_live_tool(sc_string_builder *builder, sc_tool *tool)
{
    sc_tool_spec spec = {0};
    sc_status status = sc_tool_spec_get(tool, &spec);
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "name=%s ", spec.name.ptr == nullptr ? "" : spec.name.ptr);
    }
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "risk=%s ", risk_name(spec.risk));
    }
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "side_effect=%s ", side_effect_name(spec.side_effect));
    }
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "capability=%s\n", capability_name(spec.capability_category));
    }
    return status;
}

static sc_status append_tool_status(sc_string_builder *builder, const diagnostic_tool *tool, const diagnostic_tool_descriptor *descriptor)
{
    bool allowed = false;
    sc_status status = append_descriptor(builder, descriptor);

    if (sc_status_is_ok(status)) {
        status = append_policy_decision(builder,
                                        tool,
                                        sc_str_from_cstr(descriptor->name),
                                        descriptor->risk,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""),
                                        &allowed);
    }
    if (sc_status_is_ok(status)) {
        const char *backend = "ok";
        if (strcmp(descriptor->name, "browser") == 0 || strcmp(descriptor->name, "browser_screenshot") == 0) {
            status = append_format(builder, "policy=%s ", allowed ? "allowed" : "denied");
            if (sc_status_is_ok(status)) {
                status = append_browser_backend_status(builder, tool);
            }
            return status;
        } else if (strcmp(descriptor->name, "pdf_extract") == 0 &&
                   !(command_available("/usr/bin/pdftotext") || command_available("/bin/pdftotext"))) {
            backend = "unavailable";
        } else if (strncmp(descriptor->name, "memory_", strlen("memory_")) == 0 && !memory_enabled(tool)) {
            backend = "disabled";
        } else if (strncmp(descriptor->name, "cron_", strlen("cron_")) == 0 && !cron_enabled(tool)) {
            backend = "disabled";
        }
        status = append_format(builder, "policy=%s ", allowed ? "allowed" : "denied");
        if (sc_status_is_ok(status)) {
            status = append_format(builder, "backend=%s\n", backend);
        }
    }
    return status;
}

static sc_status append_browser_backend_status(sc_string_builder *builder, const diagnostic_tool *tool)
{
    sc_string version_url = {0};
    bool version_reachable = false;
    bool enabled = browser_enabled(tool);
    bool cdp_unreachable = false;
    bool websocket_client_unavailable = false;
    const char *backend = "ok";
    sc_status status;

    status = diagnostic_get_config_string(tool,
                                          sc_str_from_cstr("browser.lightpanda.version_url"),
                                          sc_allocator_heap(),
                                          &version_url);
    if (sc_status_is_ok(status)) {
        sc_status probe_status = diagnostic_http_probe(sc_string_as_str(&version_url), sc_allocator_heap(), &version_reachable);
        if (!sc_status_is_ok(probe_status)) {
            sc_status_clear(&probe_status);
            version_reachable = false;
        }
    }
    if (!enabled) {
        backend = "disabled";
#ifndef SC_HAVE_TP_WEBSOCKET_CLIENT
    } else {
        backend = "websocket_client_unavailable";
        websocket_client_unavailable = true;
#else
    } else if (!version_reachable) {
        backend = "cdp_unreachable";
        cdp_unreachable = true;
#endif
    }

    if (sc_status_is_ok(status)) {
        status = append_format(builder, "backend=%s ", backend);
    }
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "browser_enabled=%s ", enabled ? "true" : "false");
    }
    if (sc_status_is_ok(status)) {
#ifdef SC_HAVE_TP_WEBSOCKET_CLIENT
        status = append_format(builder, "websocket_client=%s ", "available");
#else
        status = append_format(builder, "websocket_client=%s ", "missing");
#endif
    }
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "lightpanda=%s ", command_available("lightpanda") ? "available" : "missing");
    }
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "version_endpoint=%s ", version_reachable ? "reachable" : "unreachable");
    }
    if (sc_status_is_ok(status) && cdp_unreachable) {
        status = sc_string_builder_append_cstr(builder,
                                               "hint=start_lightpanda_or_chromium_remote_debugging_or_set_browser.lightpanda.cdp_url ");
    }
    if (sc_status_is_ok(status) && websocket_client_unavailable) {
        status = sc_string_builder_append_cstr(builder,
                                               "hint=rebuild_with_SC_ENABLE_THIRD_PARTY_WEBSOCKET_CLIENT ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\n");
    }
    sc_string_clear(&version_url);
    return status;
}

static sc_status append_policy_decision(sc_string_builder *builder,
                                        const diagnostic_tool *tool,
                                        sc_str name,
                                        sc_tool_risk risk,
                                        sc_str path,
                                        sc_str url,
                                        sc_str shell,
                                        sc_str device,
                                        bool *allowed)
{
    bool approval_required = false;
    sc_security_tool_request request = {
        .struct_size = sizeof(request),
        .tool_name = name,
        .risk = risk,
        .path_arg = path,
        .path_must_exist = false,
        .url_arg = url,
        .shell_arg = shell,
        .device_arg = device,
    };
    sc_status decision = sc_security_validate_request(tool->base.context.policy,
                                                      tool->base.context.estop,
                                                      &request,
                                                      &approval_required);
    sc_status status = append_format(builder, "check=%s ", name.ptr == nullptr ? "" : name.ptr);
    if (allowed != nullptr) {
        *allowed = sc_status_is_ok(decision);
    }
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "status_code=%s ", status_code_name(decision.code));
    }
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "error_key=%s ", decision.error_key == nullptr ? "" : decision.error_key);
    }
    if (sc_status_is_ok(status)) {
        status = append_format(builder, "approval_required=%s\n", approval_required ? "true" : "false");
    }
    sc_status_clear(&decision);
    return status;
}

static sc_status append_format(sc_string_builder *builder, const char *format, const char *value)
{
    char text[256] = {0};
    int written = snprintf(text, sizeof(text), format, value);
    if (written < 0 || (size_t)written >= sizeof(text)) {
        return sc_status_io("sc.diagnostic_tool.format_failed");
    }
    return sc_string_builder_append(builder, sc_str_from_parts(text, (size_t)written));
}

static sc_status append_size_line(sc_string_builder *builder, const char *key, size_t value)
{
    char text[128] = {0};
    int written = snprintf(text, sizeof(text), "%s=%zu\n", key, value);
    if (written < 0 || (size_t)written >= sizeof(text)) {
        return sc_status_io("sc.diagnostic_tool.format_failed");
    }
    return sc_string_builder_append(builder, sc_str_from_parts(text, (size_t)written));
}

static sc_status append_i64_line(sc_string_builder *builder, const char *key, int64_t value)
{
    char text[128] = {0};
    int written = snprintf(text, sizeof(text), "%s=%" PRId64 "\n", key, value);
    if (written < 0 || (size_t)written >= sizeof(text)) {
        return sc_status_io("sc.diagnostic_tool.format_failed");
    }
    return sc_string_builder_append(builder, sc_str_from_parts(text, (size_t)written));
}

static sc_status append_u64_line(sc_string_builder *builder, const char *key, uint64_t value)
{
    char text[128] = {0};
    int written = snprintf(text, sizeof(text), "%s=%" PRIu64 "\n", key, value);
    if (written < 0 || (size_t)written >= sizeof(text)) {
        return sc_status_io("sc.diagnostic_tool.format_failed");
    }
    return sc_string_builder_append(builder, sc_str_from_parts(text, (size_t)written));
}

static const diagnostic_tool_descriptor *find_descriptor(sc_str name)
{
    for (size_t i = 0; i < SC_ARRAY_LEN(tool_descriptors); i += 1) {
        if (sc_str_equal(name, sc_str_from_cstr(tool_descriptors[i].name))) {
            return &tool_descriptors[i];
        }
    }
    return nullptr;
}

static sc_str diagnostic_name(diagnostic_tool_kind kind)
{
    switch (kind) {
    case DIAG_TOOL_DIAGNOSTICS:
        return sc_str_from_cstr("tool_diagnostics");
    case DIAG_POLICY_EXPLAIN:
        return sc_str_from_cstr("policy_explain");
    case DIAG_TOOL_REGISTRY_LIST:
        return sc_str_from_cstr("tool_registry_list");
    case DIAG_DEPENDENCY_STATUS:
        return sc_str_from_cstr("dependency_status");
    case DIAG_CAPABILITY_MATRIX:
        return sc_str_from_cstr("capability_matrix");
    case DIAG_RESOURCE_USAGE:
        return sc_str_from_cstr("resource_usage");
    case DIAG_APPROVAL_TEST:
        return sc_str_from_cstr("approval_test");
    }
    return sc_str_from_cstr("tool_diagnostics");
}

static sc_str diagnostic_description(diagnostic_tool_kind kind)
{
    switch (kind) {
    case DIAG_TOOL_DIAGNOSTICS:
        return sc_str_from_cstr("tool.tool_diagnostics.description");
    case DIAG_POLICY_EXPLAIN:
        return sc_str_from_cstr("tool.policy_explain.description");
    case DIAG_TOOL_REGISTRY_LIST:
        return sc_str_from_cstr("tool.tool_registry_list.description");
    case DIAG_DEPENDENCY_STATUS:
        return sc_str_from_cstr("tool.dependency_status.description");
    case DIAG_CAPABILITY_MATRIX:
        return sc_str_from_cstr("tool.capability_matrix.description");
    case DIAG_RESOURCE_USAGE:
        return sc_str_from_cstr("tool.resource_usage.description");
    case DIAG_APPROVAL_TEST:
        return sc_str_from_cstr("tool.approval_test.description");
    }
    return sc_str_from_cstr("tool.tool_diagnostics.description");
}

static const char *risk_name(sc_tool_risk risk)
{
    switch (risk) {
    case SC_TOOL_RISK_READONLY:
        return "readonly";
    case SC_TOOL_RISK_SIDE_EFFECT:
        return "side_effect";
    case SC_TOOL_RISK_NETWORK:
        return "network";
    case SC_TOOL_RISK_SHELL:
        return "shell";
    case SC_TOOL_RISK_DESTRUCTIVE:
        return "destructive";
    }
    return "unknown";
}

static const char *side_effect_name(sc_tool_side_effect side_effect)
{
    switch (side_effect) {
    case SC_TOOL_SIDE_EFFECT_NONE:
        return "none";
    case SC_TOOL_SIDE_EFFECT_READ:
        return "read";
    case SC_TOOL_SIDE_EFFECT_WRITE:
        return "write";
    case SC_TOOL_SIDE_EFFECT_NETWORK:
        return "network";
    case SC_TOOL_SIDE_EFFECT_PROCESS:
        return "process";
    case SC_TOOL_SIDE_EFFECT_DESTRUCTIVE:
        return "destructive";
    }
    return "unknown";
}

static const char *capability_name(sc_tool_capability_category capability)
{
    switch (capability) {
    case SC_TOOL_CAPABILITY_NONE:
        return "none";
    case SC_TOOL_CAPABILITY_FILESYSTEM:
        return "filesystem";
    case SC_TOOL_CAPABILITY_MEMORY:
        return "memory";
    case SC_TOOL_CAPABILITY_NETWORK:
        return "network";
    case SC_TOOL_CAPABILITY_PROCESS:
        return "process";
    case SC_TOOL_CAPABILITY_MCP:
        return "mcp";
    case SC_TOOL_CAPABILITY_BROWSER:
        return "browser";
    case SC_TOOL_CAPABILITY_HARDWARE:
        return "hardware";
    case SC_TOOL_CAPABILITY_SAAS:
        return "saas";
    }
    return "mixed";
}

static const char *status_code_name(sc_status_code code)
{
    switch (code) {
    case SC_OK:
        return "SC_OK";
    case SC_ERR_INVALID_ARGUMENT:
        return "SC_ERR_INVALID_ARGUMENT";
    case SC_ERR_NO_MEMORY:
        return "SC_ERR_NO_MEMORY";
    case SC_ERR_IO:
        return "SC_ERR_IO";
    case SC_ERR_PARSE:
        return "SC_ERR_PARSE";
    case SC_ERR_HTTP:
        return "SC_ERR_HTTP";
    case SC_ERR_SECURITY_DENIED:
        return "SC_ERR_SECURITY_DENIED";
    case SC_ERR_UNSUPPORTED:
        return "SC_ERR_UNSUPPORTED";
    case SC_ERR_TIMEOUT:
        return "SC_ERR_TIMEOUT";
    case SC_ERR_CANCELLED:
        return "SC_ERR_CANCELLED";
    }
    return "SC_ERR_UNKNOWN";
}

static bool command_available(const char *path)
{
    const char *env_path = nullptr;
    size_t name_len = path == nullptr ? 0 : strlen(path);

    if (name_len == 0) {
        return false;
    }
    if (strchr(path, '/') != nullptr) {
        return access(path, X_OK) == 0;
    }

    env_path = getenv("PATH");
    while (env_path != nullptr && *env_path != '\0') {
        const char *end = strchr(env_path, ':');
        size_t dir_len = end == nullptr ? strlen(env_path) : (size_t)(end - env_path);
        char candidate[4096] = {0};
        int written = 0;
        if (dir_len == 0) {
            if (name_len + 2 >= sizeof(candidate)) {
                if (end == nullptr) {
                    break;
                }
                env_path = end + 1;
                continue;
            }
            written = snprintf(candidate, sizeof(candidate), "./%s", path);
        } else {
            if (dir_len >= sizeof(candidate) || name_len >= sizeof(candidate) ||
                dir_len + 1 >= sizeof(candidate) - name_len) {
                if (end == nullptr) {
                    break;
                }
                env_path = end + 1;
                continue;
            }
            written = snprintf(candidate, sizeof(candidate), "%.*s/%s", (int)dir_len, env_path, path);
        }
        if (written > 0 && (size_t)written < sizeof(candidate) && access(candidate, X_OK) == 0) {
            return true;
        }
        if (end == nullptr) {
            break;
        }
        env_path = end + 1;
    }
    return false;
}

static sc_status diagnostic_get_config_string(const diagnostic_tool *tool, sc_str path, sc_allocator *alloc, sc_string *out)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.diagnostic_tool.invalid_argument");
    }
    if (tool != nullptr && tool->base.context.config != nullptr && sc_config_has_prop(tool->base.context.config, path)) {
        return sc_config_get_prop(tool->base.context.config, path, alloc, out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("browser.lightpanda.version_url"))) {
        return sc_string_from_cstr(alloc, "http://127.0.0.1:9222/json/version", out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("browser.lightpanda.cdp_url"))) {
        return sc_string_from_cstr(alloc, "ws://127.0.0.1:9222", out);
    }
    return sc_string_from_cstr(alloc, "", out);
}

static sc_status diagnostic_http_probe(sc_str url, sc_allocator *alloc, bool *out)
{
    sc_http_response response = {0};
    sc_http_request request = {
        .struct_size = sizeof(request),
        .method = sc_str_from_cstr("GET"),
        .url = url,
        .max_response_bytes = 4096,
        .timeout_ms = 250,
        .connect_timeout_ms = 250,
        .follow_location = false,
    };
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.diagnostic_tool.invalid_argument");
    }
    status = sc_http_client_perform_sync(alloc, &request, &response);
    *out = sc_status_is_ok(status) && response.http_status >= 200 && response.http_status < 300;
    sc_http_response_clear(&response);
    return status;
}

static bool browser_enabled(const diagnostic_tool *tool)
{
    return tool->base.context.config == nullptr ||
           sc_config_get_bool(tool->base.context.config, sc_str_from_cstr("browser.enabled"), true);
}

static bool cron_enabled(const diagnostic_tool *tool)
{
    return tool->base.context.cron_jobs != nullptr;
}

static bool memory_enabled(const diagnostic_tool *tool)
{
    return strcmp(memory_backend_name(tool), "none") != 0;
}

static const char *memory_backend_name(const diagnostic_tool *tool)
{
    const sc_memory_vtab *vtab = tool == nullptr ? nullptr : sc_memory_vtab_of(tool->base.context.memory);
    return vtab == nullptr || vtab->name == nullptr ? "none" : vtab->name;
}

static sc_tool_risk risk_for_name(sc_str name)
{
    const diagnostic_tool_descriptor *descriptor = find_descriptor(name);
    return descriptor == nullptr ? SC_TOOL_RISK_READONLY : descriptor->risk;
}

static int64_t timeval_to_ms(struct timeval value)
{
    return (int64_t)value.tv_sec * INT64_C(1000) + (int64_t)value.tv_usec / INT64_C(1000);
}

static size_t live_tool_count(const diagnostic_tool *tool)
{
    size_t count = 0;

    if (tool == nullptr || tool->base.context.tools == nullptr) {
        return 0;
    }
    for (size_t i = 0; i < tool->base.context.tool_capacity; i += 1) {
        if (tool->base.context.tools[i] != nullptr) {
            count += 1;
        }
    }
    return count;
}

static bool read_current_rss_kb(size_t *out)
{
    FILE *file = nullptr;
    char line[128] = {0};
    char *cursor = nullptr;
    char *end = nullptr;
    unsigned long long resident_pages = 0;
    uint64_t page_size_bytes = 0;
    uint64_t rss_bytes = 0;
    uint64_t rss_kb = 0;
    long page_size = 0;

    if (out == nullptr) {
        return false;
    }
    file = fopen("/proc/self/statm", "r");
    if (file == nullptr) {
        return false;
    }
    if (fgets(line, sizeof(line), file) == nullptr) {
        (void)fclose(file);
        return false;
    }
    (void)fclose(file);

    cursor = line;
    errno = 0;
    (void)strtoull(cursor, &end, 10);
    if (cursor == end || errno != 0) {
        return false;
    }
    cursor = end;
    errno = 0;
    resident_pages = strtoull(cursor, &end, 10);
    if (cursor == end || errno != 0) {
        return false;
    }

    page_size = sysconf(_SC_PAGESIZE);
    if (page_size <= 0) {
        return false;
    }
    page_size_bytes = (uint64_t)page_size;
    if (resident_pages > UINT64_MAX / page_size_bytes) {
        return false;
    }
    rss_bytes = (uint64_t)resident_pages * page_size_bytes;
    rss_kb = rss_bytes / UINT64_C(1024);
    if (rss_kb > (uint64_t)SIZE_MAX) {
        return false;
    }
    *out = (size_t)rss_kb;
    return true;
}

static bool count_open_fds(size_t *out)
{
    DIR *dir = nullptr;
    const struct dirent *entry = nullptr;
    size_t count = 0;

    if (out == nullptr) {
        return false;
    }
    dir = opendir("/proc/self/fd");
    if (dir == nullptr) {
        return false;
    }
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") != 0 && strcmp(entry->d_name, "..") != 0) {
            count += 1;
        }
    }
    (void)closedir(dir);
    *out = count;
    return true;
}

static void diagnostic_destroy(void *impl)
{
    diagnostic_tool *tool = impl;
    sc_allocator *alloc;

    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc == nullptr ? sc_allocator_heap() : tool->base.alloc;
    sc_tool_impl_context_clear(&tool->base);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(diagnostic_tool));
}
