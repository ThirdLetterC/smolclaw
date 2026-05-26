#include "tools/tool_internal.h"
#include "sc/log.h"
#include "sc/runtime.h"

#include <stdio.h>
#include <string.h>

static sc_status add_string_property(sc_json_value *schema, sc_str name, bool required);
static sc_status append_model_spec(sc_json_value *array, sc_tool *tool, sc_allocator *alloc);
static sc_str empty_if_null(sc_str value);
static uint64_t receipt_summary_hash(sc_str value);
static sc_status receipt_summary(sc_allocator *alloc, sc_str value, sc_string *out);
static bool span_contains_nul(sc_str value);
static const char *tool_status_code_name(sc_status_code code);
static sc_status validate_schema_string_subset(const sc_json_value *schema, const sc_json_value *args);

static _Thread_local sc_str current_approval_override = {0};

void sc_tool_result_clear(sc_tool_result *result)
{
    if (result == nullptr) {
        return;
    }
    sc_string_clear(&result->output);
    sc_string_clear(&result->attachment_content_type);
    sc_string_clear(&result->attachment_filename);
    sc_bytes_clear(&result->attachment_bytes);
    *result = (sc_tool_result){0};
}

sc_status sc_tool_registry_model_specs_from_tools(sc_tool **tools,
                                                  size_t tool_count,
                                                  sc_allocator *alloc,
                                                  sc_json_value **out)
{
    sc_json_value *array = nullptr;
    sc_status status;

    if (out == nullptr || (tool_count > 0 && tools == nullptr)) {
        return sc_status_invalid_argument("sc.tool_specs.invalid_argument");
    }

    status = sc_json_array_new(alloc, &array);
    for (size_t i = 0; sc_status_is_ok(status) && i < tool_count; i += 1) {
        sc_tool_spec current = {0};
        if (tools[i] == nullptr) {
            status = sc_status_invalid_argument("sc.tool_specs.null_tool");
            break;
        }
        status = sc_tool_spec_get(tools[i], &current);
        for (size_t j = 0; sc_status_is_ok(status) && j < i; j += 1) {
            sc_tool_spec prior = {0};
            status = sc_tool_spec_get(tools[j], &prior);
            if (sc_status_is_ok(status) && sc_str_equal(prior.name, current.name)) {
                status = sc_status_invalid_argument("sc.tool_specs.duplicate_tool");
            }
        }
        if (!sc_status_is_ok(status)) {
            break;
        }
        status = append_model_spec(array, tools[i], alloc);
    }
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(array);
        return status;
    }
    *out = array;
    return sc_status_ok();
}

sc_status sc_tool_schema_string_required(sc_allocator *alloc,
                                         sc_str name,
                                         sc_str description,
                                         sc_json_value **out)
{
    (void)description;
    return sc_tool_schema_string(alloc, name, true, out);
}

sc_status sc_tool_schema_string(sc_allocator *alloc,
                                sc_str name,
                                bool required,
                                sc_json_value **out)
{
    sc_json_value *schema = nullptr;
    sc_status status = sc_json_schema_object(alloc, &schema);
    if (sc_status_is_ok(status)) {
        status = add_string_property(schema, name, required);
    }
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(schema);
        return status;
    }
    *out = schema;
    return sc_status_ok();
}

sc_status sc_tool_output_schema_text(sc_allocator *alloc, sc_json_value **out)
{
    return sc_tool_schema_string_required(alloc,
                                          sc_str_from_cstr("text"),
                                          sc_str_from_cstr("tool.output.text"),
                                          out);
}

sc_status sc_tool_schema_two_strings(sc_allocator *alloc,
                                     sc_str first,
                                     bool first_required,
                                     sc_str second,
                                     bool second_required,
                                     sc_json_value **out)
{
    sc_json_value *schema = nullptr;
    sc_status status = sc_json_schema_object(alloc, &schema);
    if (sc_status_is_ok(status)) {
        status = add_string_property(schema, first, first_required);
    }
    if (sc_status_is_ok(status)) {
        status = add_string_property(schema, second, second_required);
    }
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(schema);
        return status;
    }
    *out = schema;
    return sc_status_ok();
}

sc_status sc_tool_schema_three_strings(sc_allocator *alloc,
                                       sc_str first,
                                       bool first_required,
                                       sc_str second,
                                       bool second_required,
                                       sc_str third,
                                       bool third_required,
                                       sc_json_value **out)
{
    sc_json_value *schema = nullptr;
    sc_status status = sc_json_schema_object(alloc, &schema);
    if (sc_status_is_ok(status)) {
        status = add_string_property(schema, first, first_required);
    }
    if (sc_status_is_ok(status)) {
        status = add_string_property(schema, second, second_required);
    }
    if (sc_status_is_ok(status)) {
        status = add_string_property(schema, third, third_required);
    }
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(schema);
        return status;
    }
    *out = schema;
    return sc_status_ok();
}

sc_status sc_tool_schema_four_strings(sc_allocator *alloc,
                                      sc_str first,
                                      bool first_required,
                                      sc_str second,
                                      bool second_required,
                                      sc_str third,
                                      bool third_required,
                                      sc_str fourth,
                                      bool fourth_required,
                                      sc_json_value **out)
{
    sc_json_value *schema = nullptr;
    sc_status status = sc_json_schema_object(alloc, &schema);
    if (sc_status_is_ok(status)) {
        status = add_string_property(schema, first, first_required);
    }
    if (sc_status_is_ok(status)) {
        status = add_string_property(schema, second, second_required);
    }
    if (sc_status_is_ok(status)) {
        status = add_string_property(schema, third, third_required);
    }
    if (sc_status_is_ok(status)) {
        status = add_string_property(schema, fourth, fourth_required);
    }
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(schema);
        return status;
    }
    *out = schema;
    return sc_status_ok();
}

sc_status sc_tool_context_copy(sc_allocator *alloc,
                               const sc_tool_context *context,
                               sc_tool_impl_context *out)
{
    if (out == nullptr || context == nullptr || context->policy == nullptr) {
        return sc_status_invalid_argument("sc.tool_context.invalid_argument");
    }
    *out = (sc_tool_impl_context){
        .alloc = alloc == nullptr ? sc_allocator_heap() : alloc,
        .context = *context,
    };
    return sc_status_ok();
}

void sc_tool_impl_context_clear(sc_tool_impl_context *context)
{
    if (context == nullptr) {
        return;
    }
    sc_json_destroy(context->schema);
    sc_json_destroy(context->output_schema);
    *context = (sc_tool_impl_context){0};
}

sc_status sc_tool_get_string_arg(const sc_tool_call *call, sc_str name, sc_str *out)
{
    sc_json_value *value = nullptr;

    if (call == nullptr || call->args == nullptr || out == nullptr || name.ptr == nullptr || name.len == 0) {
        return sc_status_invalid_argument("sc.tool.args.invalid_argument");
    }
    value = sc_json_object_get(call->args, name);
    if (!sc_json_as_str(value, out) || out->ptr == nullptr || out->len == 0) {
        return sc_status_invalid_argument("sc.tool.args.missing_string");
    }
    if (span_contains_nul(*out)) {
        return sc_status_invalid_argument("sc.tool.args.embedded_nul");
    }
    if (!sc_str_is_valid_utf8(*out)) {
        return sc_status_invalid_argument("sc.tool.args.invalid_utf8");
    }
    return sc_status_ok();
}

sc_status sc_tool_get_optional_string_arg(const sc_tool_call *call, sc_str name, sc_str *out)
{
    sc_json_value *value = nullptr;

    if (call == nullptr || call->args == nullptr || out == nullptr || name.ptr == nullptr || name.len == 0) {
        return sc_status_invalid_argument("sc.tool.args.invalid_argument");
    }
    value = sc_json_object_get(call->args, name);
    if (value == nullptr || sc_json_is_null(value)) {
        *out = sc_str_from_cstr("");
        return sc_status_ok();
    }
    if (!sc_json_as_str(value, out)) {
        return sc_status_invalid_argument("sc.tool.args.wrong_type");
    }
    if (span_contains_nul(*out)) {
        return sc_status_invalid_argument("sc.tool.args.embedded_nul");
    }
    if (!sc_str_is_valid_utf8(*out)) {
        return sc_status_invalid_argument("sc.tool.args.invalid_utf8");
    }
    return sc_status_ok();
}

sc_status sc_tool_check_cancelled(const sc_tool_impl_context *context, const sc_tool_call *call)
{
    if (call != nullptr && call->cancel_token != nullptr && call->cancel_token->cancel_requested) {
        return sc_status_cancelled("sc.tool.cancelled");
    }
    if (context != nullptr && context->context.cancel_token != nullptr && context->context.cancel_token->cancel_requested) {
        return sc_status_cancelled("sc.tool.cancelled");
    }
    return sc_status_ok();
}

sc_status sc_tool_validate_call_against_schema(const sc_tool_spec *spec, const sc_tool_call *call)
{
    sc_status status;

    if (spec == nullptr || call == nullptr) {
        return sc_status_invalid_argument("sc.tool.schema.invalid_argument");
    }
    if (spec->input_schema == nullptr) {
        return sc_status_ok();
    }
    if (call->args == nullptr) {
        return sc_status_invalid_argument("sc.tool.schema.missing_args");
    }
    status = validate_schema_string_subset(spec->input_schema, call->args);
    return status;
}

sc_status sc_tool_security_check(const sc_tool_impl_context *context,
                                 sc_str tool_name,
                                 sc_tool_risk risk,
                                 sc_str path,
                                 bool path_must_exist,
                                 sc_str url,
                                 sc_str shell)
{
    return sc_tool_security_check_ex(context,
                                     tool_name,
                                     risk,
                                     path,
                                     path_must_exist,
                                     url,
                                     shell,
                                     sc_str_from_cstr(""),
                                     sc_str_from_cstr(""));
}

sc_status sc_tool_security_check_ex(const sc_tool_impl_context *context,
                                    sc_str tool_name,
                                    sc_tool_risk risk,
                                    sc_str path,
                                    bool path_must_exist,
                                    sc_str url,
                                    sc_str shell,
                                    sc_str device,
                                    sc_str otp)
{
    bool approval_required = false;
    sc_security_tool_request request = {
        .struct_size = sizeof(request),
        .tool_name = tool_name,
        .risk = risk,
        .path_arg = path,
        .path_must_exist = path_must_exist,
        .url_arg = url,
        .shell_arg = shell,
        .device_arg = device,
        .otp_code = otp,
    };
    sc_status status;
    char risk_text[32] = {0};

    if (context == nullptr) {
        return sc_status_invalid_argument("sc.tool.security.invalid_argument");
    }

    (void)snprintf(risk_text, sizeof(risk_text), "%u", (unsigned int)risk);
    sc_log_field fields[] = {
        {.key = "tool", .value = tool_name, .secret = false},
        {.key = "risk", .value = sc_str_from_cstr(risk_text), .secret = false},
    };
    sc_log_write(SC_LOG_DEBUG, "sc.tool", "tool.security_check.start", fields, SC_ARRAY_LEN(fields));
    status = sc_security_validate_request(context->context.policy,
                                          context->context.estop,
                                          &request,
                                          &approval_required);
    if (!sc_status_is_ok(status)) {
        sc_log_field denied_fields[] = {
            {.key = "tool", .value = tool_name, .secret = false},
            {.key = "subsystem", .value = sc_str_from_cstr("security"), .secret = false},
            {.key = "operation", .value = sc_str_from_cstr("tool.security_check.denied"), .secret = false},
            {.key = "outcome", .value = sc_str_from_cstr("denied"), .secret = false},
            {.key = "error_key", .value = sc_str_from_cstr(status.error_key == nullptr ? "sc.tool.security_denied" : status.error_key), .secret = false},
        };
        sc_log_write(SC_LOG_WARN, "sc.tool", "tool.security_check.denied", denied_fields, SC_ARRAY_LEN(denied_fields));
        sc_log_write(SC_LOG_WARN, "sc.audit", "audit.security.denied", denied_fields, SC_ARRAY_LEN(denied_fields));
        return status;
    }
    if (approval_required) {
        if (sc_str_equal(current_approval_override, tool_name)) {
            sc_log_write(SC_LOG_INFO, "sc.tool", "tool.approval_granted", fields, SC_ARRAY_LEN(fields));
            return sc_status_ok();
        }
        sc_log_write(SC_LOG_WARN, "sc.tool", "tool.approval_required", fields, SC_ARRAY_LEN(fields));
        return sc_status_cancelled("sc.tool.approval_required");
    }
    sc_log_write(SC_LOG_TRACE, "sc.tool", "tool.security_check.ok", fields, SC_ARRAY_LEN(fields));
    return sc_status_ok();
}

void sc_tool_approval_override_set(sc_str tool_name)
{
    current_approval_override = tool_name;
}

void sc_tool_approval_override_clear()
{
    current_approval_override = (sc_str){0};
}

void sc_tool_log_failure(sc_str tool_name, sc_status status)
{
    sc_log_field fields[] = {
        {.key = "tool", .value = tool_name, .secret = false},
        {.key = "status_code", .value = sc_str_from_cstr(tool_status_code_name(status.code)), .secret = false},
        {.key = "error_key",
         .value = sc_str_from_cstr(status.error_key == nullptr ? "sc.tool.error" : status.error_key),
         .secret = false},
        {.key = "detail", .value = sc_str_from_cstr(status.message == nullptr ? "" : status.message), .secret = false},
    };

    if (sc_status_is_ok(status)) {
        return;
    }
    sc_log_write(SC_LOG_ERROR, "sc.tool", "tool.invocation.failed", fields, SC_ARRAY_LEN(fields));
}

sc_status sc_tool_set_output(sc_allocator *alloc,
                             const sc_tool_impl_context *context,
                             sc_tool_result *out,
                             sc_str text,
                             bool success)
{
    size_t max_output = 0;
    sc_str output = text;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.tool_result.invalid_argument");
    }
    max_output = context == nullptr || context->context.max_output_bytes == 0 ? 4096 : context->context.max_output_bytes;
    if (output.len > max_output) {
        output.len = max_output;
    }
    *out = (sc_tool_result){.struct_size = sizeof(*out), .success = success};
    return sc_string_from_str(alloc, empty_if_null(output), &out->output);
}

sc_status sc_tool_record_receipt(const sc_tool_impl_context *context,
                                 sc_str tool_name,
                                 sc_str args_summary,
                                 sc_str output_summary,
                                 bool success)
{
    return sc_tool_record_receipt_status(context,
                                         tool_name,
                                         args_summary,
                                         output_summary,
                                         success,
                                         success ? sc_status_ok() : sc_status_make(SC_ERR_IO, "sc.tool.failed", nullptr));
}

sc_status sc_tool_record_receipt_status(const sc_tool_impl_context *context,
                                        sc_str tool_name,
                                        sc_str args_summary,
                                        sc_str output_summary,
                                        bool success,
                                        sc_status tool_status)
{
    sc_log_field fields[] = {
        {.key = "tool", .value = tool_name, .secret = false},
        {.key = "status", .value = success ? sc_str_from_cstr("ok") : sc_str_from_cstr("error"), .secret = false},
        {.key = "status_code", .value = sc_str_from_cstr(tool_status_code_name(tool_status.code)), .secret = false},
        {.key = "error_key",
         .value = sc_str_from_cstr(tool_status.error_key == nullptr ? "" : tool_status.error_key),
         .secret = false},
        {.key = "detail", .value = sc_str_from_cstr(tool_status.message == nullptr ? "" : tool_status.message), .secret = false},
    };
    sc_string safe_args = {0};
    sc_string safe_output = {0};
    sc_allocator *alloc = context == nullptr || context->alloc == nullptr ? sc_allocator_heap() : context->alloc;
    sc_status status;
    sc_str failure_reason = success ? sc_str_from_parts(nullptr, 0) :
                                      sc_str_from_cstr(tool_status.error_key == nullptr ? "sc.tool.failed" : tool_status.error_key);
    sc_str policy_decision = tool_status.code == SC_ERR_SECURITY_DENIED ? sc_str_from_cstr("denied") :
                                                                          sc_str_from_cstr("allowed");
    sc_str outcome = success ? sc_str_from_cstr("ok") :
                               (tool_status.code == SC_ERR_SECURITY_DENIED ? sc_str_from_cstr("denied") :
                                                                              sc_str_from_cstr("error"));

    sc_log_write(success ? SC_LOG_INFO : SC_LOG_ERROR,
                 "sc.tool",
                 success ? "tool.receipt.recorded" : "tool.receipt.failed",
                 fields,
                 SC_ARRAY_LEN(fields));
    if (context == nullptr || context->context.receipts == nullptr ||
        (context->context.policy != nullptr && !context->context.policy->receipts_enabled)) {
        return sc_status_ok();
    }
    status = receipt_summary(alloc, empty_if_null(args_summary), &safe_args);
    if (sc_status_is_ok(status)) {
        status = receipt_summary(alloc, empty_if_null(output_summary), &safe_output);
    }
    if (sc_status_is_ok(status)) {
        status = sc_receipt_chain_append_ex(context->context.receipts,
                                            tool_name,
                                            sc_string_as_str(&safe_args),
                                            sc_string_as_str(&safe_output),
                                            success,
                                            policy_decision,
                                            failure_reason,
                                            outcome);
    }
    sc_string_clear(&safe_args);
    sc_string_clear(&safe_output);
    return status;
}

static sc_status add_string_property(sc_json_value *schema, sc_str name, bool required)
{
    return sc_json_schema_add_string_property(schema, name, required);
}

static bool span_contains_nul(sc_str value)
{
    return value.ptr != nullptr && memchr(value.ptr, '\0', value.len) != nullptr;
}

static const char *tool_status_code_name(sc_status_code code)
{
    switch (code) {
    case SC_OK:
        return "ok";
    case SC_ERR_INVALID_ARGUMENT:
        return "invalid_argument";
    case SC_ERR_NO_MEMORY:
        return "no_memory";
    case SC_ERR_IO:
        return "io";
    case SC_ERR_PARSE:
        return "parse";
    case SC_ERR_HTTP:
        return "http";
    case SC_ERR_SECURITY_DENIED:
        return "security_denied";
    case SC_ERR_UNSUPPORTED:
        return "unsupported";
    case SC_ERR_TIMEOUT:
        return "timeout";
    case SC_ERR_CANCELLED:
        return "cancelled";
    }
    return "unknown";
}

static sc_status validate_schema_string_subset(const sc_json_value *schema, const sc_json_value *args)
{
    const sc_json_value *required = schema == nullptr ? nullptr : sc_json_object_get(schema, sc_str_from_cstr("required"));
    const sc_json_value *properties = schema == nullptr ? nullptr : sc_json_object_get(schema, sc_str_from_cstr("properties"));

    if (schema == nullptr) {
        return sc_status_ok();
    }
    if (sc_json_type_of(args) != SC_JSON_OBJECT) {
        return sc_status_invalid_argument("sc.tool.schema.args_not_object");
    }
    for (size_t i = 0; i < sc_json_array_len(required); i += 1) {
        sc_str name = {0};
        const sc_json_value *required_name = sc_json_array_get(required, i);
        const sc_json_value *value = nullptr;
        if (!sc_json_as_str(required_name, &name)) {
            return sc_status_invalid_argument("sc.tool.schema.invalid_required");
        }
        value = sc_json_object_get(args, name);
        if (value == nullptr) {
            return sc_status_invalid_argument("sc.tool.schema.required_missing");
        }
        if (sc_json_type_of(value) != SC_JSON_STRING) {
            return sc_status_invalid_argument("sc.tool.schema.type_mismatch");
        }
    }
    if (properties != nullptr) {
        for (size_t i = 0; i < sc_json_array_len(required); i += 1) {
            sc_str name = {0};
            if (sc_json_as_str(sc_json_array_get(required, i), &name)) {
                sc_json_value *value = sc_json_object_get(args, name);
                if (value != nullptr && sc_json_type_of(value) == SC_JSON_STRING) {
                    sc_str text = {0};
                    if (sc_json_as_str(value, &text) && (span_contains_nul(text) || !sc_str_is_valid_utf8(text))) {
                        return sc_status_invalid_argument(span_contains_nul(text) ? "sc.tool.args.embedded_nul" :
                                                                                   "sc.tool.args.invalid_utf8");
                    }
                }
            }
        }
    }
    return sc_status_ok();
}

static sc_status append_model_spec(sc_json_value *array, sc_tool *tool, sc_allocator *alloc)
{
    sc_tool_spec spec = {0};
    sc_json_value *outer = nullptr;
    sc_json_value *function = nullptr;
    sc_json_value *type = nullptr;
    sc_json_value *name = nullptr;
    sc_json_value *description = nullptr;
    sc_json_value *parameters = nullptr;
    sc_status status;

    if (tool == nullptr) {
        return sc_status_invalid_argument("sc.tool_specs.null_tool");
    }

    status = sc_tool_spec_get(tool, &spec);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &outer);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_string_new(alloc, sc_str_from_cstr("function"), &type);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(outer, sc_str_from_cstr("type"), type);
        type = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &function);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_string_new(alloc, spec.name, &name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(function, sc_str_from_cstr("name"), name);
        name = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_string_new(alloc, spec.description, &description);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(function, sc_str_from_cstr("description"), description);
        description = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_clone(spec.input_schema, alloc, &parameters);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(function, sc_str_from_cstr("parameters"), parameters);
        parameters = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(outer, sc_str_from_cstr("function"), function);
        function = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_append(array, outer);
        outer = nullptr;
    }

    sc_json_destroy(outer);
    sc_json_destroy(function);
    sc_json_destroy(type);
    sc_json_destroy(name);
    sc_json_destroy(description);
    sc_json_destroy(parameters);
    return status;
}

static sc_str empty_if_null(sc_str value)
{
    return value.ptr == nullptr ? sc_str_from_cstr("") : value;
}

static uint64_t receipt_summary_hash(sc_str value)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < value.len; i += 1) {
        hash ^= (unsigned char)value.ptr[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static sc_status receipt_summary(sc_allocator *alloc, sc_str value, sc_string *out)
{
    char text[64] = {0};
    int written = snprintf(text,
                           sizeof(text),
                           "hash=%016llx len=%zu",
                           (unsigned long long)receipt_summary_hash(value),
                           value.len);
    if (written < 0 || (size_t)written >= sizeof(text)) {
        return sc_status_no_memory();
    }
    return sc_string_from_cstr(alloc, text, out);
}
