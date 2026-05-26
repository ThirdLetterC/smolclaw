#include "sc/provider.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static sc_status copy_string_field(sc_allocator *alloc, sc_str value, sc_string *out);
static sc_status validate_text(sc_str value, size_t max_len, const char *error_key);
static bool model_name_valid(sc_str model);
static bool tool_specs_valid(sc_allocator *alloc, sc_str tool_specs_json);
static bool str_has_prefix(sc_str value, const char *prefix);

enum {
    SC_PROVIDER_MAX_MODEL_BYTES = 256,
    SC_PROVIDER_MAX_PROMPT_BYTES = 1048576,
    SC_PROVIDER_MAX_SYSTEM_BYTES = 262144,
    SC_PROVIDER_MAX_MEDIA_CONTEXT_BYTES = 262144,
    SC_PROVIDER_MAX_TOOL_SPEC_BYTES = 262144,
    SC_PROVIDER_MAX_TOOL_ARGUMENT_BYTES = 262144,
};

void sc_provider_tool_call_clear(sc_provider_tool_call *call)
{
    if (call == nullptr) {
        return;
    }
    sc_string_clear(&call->call_id);
    sc_string_clear(&call->name);
    sc_string_clear(&call->arguments_json);
    sc_json_destroy(call->arguments);
    *call = (sc_provider_tool_call){0};
}

sc_status sc_provider_tool_call_as_tool_call(const sc_provider_tool_call *source, sc_tool_call *out)
{
    if (source == nullptr || out == nullptr || source->arguments == nullptr) {
        return sc_status_invalid_argument("sc.provider_tool_call.invalid_argument");
    }

    *out = (sc_tool_call){
        .struct_size = sizeof(*out),
        .call_id = sc_string_as_str(&source->call_id),
        .args = source->arguments,
    };
    return sc_status_ok();
}

void sc_provider_response_init(sc_provider_response *response, sc_allocator *alloc)
{
    if (response == nullptr) {
        return;
    }
    *response = (sc_provider_response){.struct_size = sizeof(*response)};
    sc_vec_init(&response->tool_calls, alloc == nullptr ? sc_allocator_heap() : alloc, sizeof(sc_provider_tool_call));
}

sc_status sc_provider_response_add_tool_call(sc_provider_response *response, const sc_provider_tool_call *call)
{
    sc_provider_tool_call copy = {0};
    sc_status status;

    if (response == nullptr || call == nullptr || response->tool_calls.item_size != sizeof(sc_provider_tool_call)) {
        return sc_status_invalid_argument("sc.provider_response.invalid_argument");
    }
    if (call->arguments_json.len > SC_PROVIDER_MAX_TOOL_ARGUMENT_BYTES) {
        return sc_status_invalid_argument("sc.provider_tool_call.arguments_too_large");
    }

    copy.struct_size = sizeof(copy);
    status = copy_string_field(response->tool_calls.alloc, sc_string_as_str(&call->call_id), &copy.call_id);
    if (sc_status_is_ok(status)) {
        status = copy_string_field(response->tool_calls.alloc, sc_string_as_str(&call->name), &copy.name);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string_field(response->tool_calls.alloc, sc_string_as_str(&call->arguments_json), &copy.arguments_json);
    }
    if (sc_status_is_ok(status) && call->arguments != nullptr) {
        status = sc_json_clone(call->arguments, response->tool_calls.alloc, &copy.arguments);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&response->tool_calls, &copy);
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_tool_call_clear(&copy);
        return status;
    }
    return sc_status_ok();
}

void sc_provider_response_clear(sc_provider_response *response)
{
    if (response == nullptr) {
        return;
    }
    sc_string_clear(&response->text);
    sc_string_clear(&response->finish_reason);
    sc_string_clear(&response->model);
    sc_string_clear(&response->error_message);
    sc_string_clear(&response->reasoning_text);
    sc_provider_tool_call_clear(&response->pre_executed_tool_call);
    sc_string_clear(&response->pre_executed_tool_result);
    for (size_t i = 0; i < response->tool_calls.len; i += 1) {
        sc_provider_tool_call *call = sc_vec_at(&response->tool_calls, i);
        sc_provider_tool_call_clear(call);
    }
    sc_vec_clear(&response->tool_calls);
    *response = (sc_provider_response){0};
}

void sc_provider_stream_event_clear(sc_provider_stream_event *event)
{
    if (event == nullptr) {
        return;
    }
    sc_string_clear(&event->text);
    sc_provider_tool_call_clear(&event->tool_call);
    sc_string_clear(&event->error_message);
    *event = (sc_provider_stream_event){0};
}

sc_status sc_provider_parse_thinking_level(sc_str value, sc_provider_thinking_level *out)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider.thinking_level.invalid_argument");
    }
    if (value.ptr == nullptr || value.len == 0 || sc_str_equal(value, sc_str_from_cstr("default"))) {
        *out = SC_PROVIDER_THINKING_DEFAULT;
        return sc_status_ok();
    }
    if (sc_str_equal(value, sc_str_from_cstr("disabled")) || sc_str_equal(value, sc_str_from_cstr("none")) ||
        sc_str_equal(value, sc_str_from_cstr("off"))) {
        *out = SC_PROVIDER_THINKING_DISABLED;
        return sc_status_ok();
    }
    if (sc_str_equal(value, sc_str_from_cstr("low"))) {
        *out = SC_PROVIDER_THINKING_LOW;
        return sc_status_ok();
    }
    if (sc_str_equal(value, sc_str_from_cstr("medium"))) {
        *out = SC_PROVIDER_THINKING_MEDIUM;
        return sc_status_ok();
    }
    if (sc_str_equal(value, sc_str_from_cstr("high"))) {
        *out = SC_PROVIDER_THINKING_HIGH;
        return sc_status_ok();
    }
    return sc_status_invalid_argument("sc.provider.thinking_level.invalid");
}

const char *sc_provider_thinking_level_name(sc_provider_thinking_level level)
{
    switch (level) {
    case SC_PROVIDER_THINKING_DEFAULT:
        return "default";
    case SC_PROVIDER_THINKING_DISABLED:
        return "disabled";
    case SC_PROVIDER_THINKING_LOW:
        return "low";
    case SC_PROVIDER_THINKING_MEDIUM:
        return "medium";
    case SC_PROVIDER_THINKING_HIGH:
        return "high";
    }
    return "default";
}

sc_status sc_provider_redact_credential(sc_allocator *alloc, sc_str value, sc_string *out)
{
    (void)value;
    return sc_string_redacted(alloc, out);
}

sc_status sc_provider_resolve_credential(sc_allocator *alloc, const sc_provider_options *options, sc_string *out)
{
    sc_str credential_env = {0};
    sc_str generic_env = {0};

    if (options == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_credential.invalid_argument");
    }
    if (options->api_key.ptr != nullptr && options->api_key.len > 0) {
        return sc_string_from_str(alloc, options->api_key, out);
    }
    if (options->secret_value.ptr != nullptr && options->secret_value.len > 0) {
        return sc_string_from_str(alloc, options->secret_value, out);
    }
    credential_env = options->credential_env;
    if (credential_env.ptr != nullptr && credential_env.len > 0) {
        sc_string env_name = {0};
        sc_status status = sc_string_from_str(alloc, credential_env, &env_name);
        if (!sc_status_is_ok(status)) {
            return status;
        }
        const char *env_value = getenv(env_name.ptr);
        sc_string_clear(&env_name);
        if (env_value != nullptr && env_value[0] != '\0') {
            return sc_string_from_cstr(alloc, env_value, out);
        }
    }
    generic_env = options->generic_credential_env;
    if (generic_env.ptr != nullptr && generic_env.len > 0) {
        sc_string env_name = {0};
        sc_status status = sc_string_from_str(alloc, generic_env, &env_name);
        if (!sc_status_is_ok(status)) {
            return status;
        }
        const char *env_value = getenv(env_name.ptr);
        sc_string_clear(&env_name);
        if (env_value != nullptr && env_value[0] != '\0') {
            return sc_string_from_cstr(alloc, env_value, out);
        }
    }
    return sc_status_security_denied("sc.provider_credential.missing");
}

sc_status sc_provider_validate_request(const sc_provider_vtab *vtab,
                                       const sc_provider_options *options,
                                       const sc_provider_request *request)
{
    sc_str model = {0};
    sc_status status;
    uint64_t modes = vtab == nullptr ? 0 : vtab->provider_modes;

    if (request == nullptr) {
        return sc_status_invalid_argument("sc.provider_request.invalid_argument");
    }
    model = request->model.len > 0 ? request->model : (options == nullptr ? sc_str_from_cstr("") : options->default_model);
    status = validate_text(model, SC_PROVIDER_MAX_MODEL_BYTES, "sc.provider_request.model_invalid");
    if (sc_status_is_ok(status) && model.len > 0 && !model_name_valid(model)) {
        status = sc_status_invalid_argument("sc.provider_request.model_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = validate_text(request->prompt, SC_PROVIDER_MAX_PROMPT_BYTES, "sc.provider_request.prompt_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = validate_text(request->system_instruction, SC_PROVIDER_MAX_SYSTEM_BYTES, "sc.provider_request.system_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = validate_text(request->media_context, SC_PROVIDER_MAX_MEDIA_CONTEXT_BYTES, "sc.provider_request.media_context_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = validate_text(request->tool_specs_json, SC_PROVIDER_MAX_TOOL_SPEC_BYTES, "sc.provider_request.tools_invalid");
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (request->tool_specs_json.len > 0) {
        if ((modes & SC_PROVIDER_MODE_TOOL_CALLS) == 0) {
            return sc_status_unsupported("sc.provider_request.tools_unsupported");
        }
        if (!tool_specs_valid(sc_allocator_heap(), request->tool_specs_json)) {
            return sc_status_parse("sc.provider_request.tools_invalid");
        }
    }
    if (request->media_count > 0 && (modes & (SC_PROVIDER_MODE_VISION | SC_PROVIDER_MODE_AUDIO)) == 0) {
        return sc_status_unsupported("sc.provider_request.media_unsupported");
    }
    return sc_status_ok();
}

sc_provider_retry_class sc_provider_classify_retry(sc_status status, int http_status)
{
    if (status.code == SC_ERR_CANCELLED) {
        return SC_PROVIDER_RETRY_CANCELLED;
    }
    if (status.code == SC_ERR_SECURITY_DENIED) {
        return SC_PROVIDER_RETRY_SECURITY_DENIED;
    }
    if (status.code == SC_ERR_INVALID_ARGUMENT || status.code == SC_ERR_PARSE) {
        return SC_PROVIDER_RETRY_INVALID_REQUEST;
    }
    if (http_status == 401 || http_status == 403) {
        return SC_PROVIDER_RETRY_INVALID_CREDENTIALS;
    }
    if (http_status == 408 || status.code == SC_ERR_TIMEOUT) {
        return SC_PROVIDER_RETRY_TRANSIENT;
    }
    if (http_status == 429) {
        return SC_PROVIDER_RETRY_RATE_LIMIT;
    }
    if (http_status >= 500 || status.code == SC_ERR_IO || status.code == SC_ERR_HTTP || status.code == SC_ERR_TIMEOUT) {
        return SC_PROVIDER_RETRY_SERVER;
    }
    return SC_PROVIDER_RETRY_NONE;
}

bool sc_provider_should_retry(sc_status status, int http_status)
{
    sc_provider_retry_class retry_class = sc_provider_classify_retry(status, http_status);
    return retry_class == SC_PROVIDER_RETRY_TRANSIENT ||
           retry_class == SC_PROVIDER_RETRY_RATE_LIMIT ||
           retry_class == SC_PROVIDER_RETRY_SERVER;
}

bool sc_provider_url_allowed(sc_str url, bool allow_loopback)
{
    if (url.ptr == nullptr || url.len == 0) {
        return false;
    }
    if (str_has_prefix(url, "https://")) {
        return true;
    }
    if (!allow_loopback || !str_has_prefix(url, "http://")) {
        return false;
    }
    return str_has_prefix(url, "http://127.0.0.1") ||
           str_has_prefix(url, "http://localhost") ||
           str_has_prefix(url, "http://[::1]");
}

static sc_status copy_string_field(sc_allocator *alloc, sc_str value, sc_string *out)
{
    if (value.ptr == nullptr) {
        value = sc_str_from_cstr("");
    }
    return sc_string_from_str(alloc, value, out);
}

static sc_status validate_text(sc_str value, size_t max_len, const char *error_key)
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

static bool model_name_valid(sc_str model)
{
    for (size_t i = 0; i < model.len; i += 1) {
        unsigned char ch = (unsigned char)model.ptr[i];
        if (!isalnum(ch) && ch != '-' && ch != '_' && ch != '.' && ch != ':' && ch != '/') {
            return false;
        }
    }
    return true;
}

static bool tool_specs_valid(sc_allocator *alloc, sc_str tool_specs_json)
{
    sc_json_value *value = nullptr;
    sc_json_parse_error error = {0};
    sc_status status = sc_json_parse(alloc, tool_specs_json, &value, &error);
    bool valid = sc_status_is_ok(status) && sc_json_type_of(value) == SC_JSON_ARRAY;
    sc_status_clear(&status);
    sc_json_destroy(value);
    return valid;
}

static bool str_has_prefix(sc_str value, const char *prefix)
{
    size_t len = prefix == nullptr ? 0 : strlen(prefix);
    return value.ptr != nullptr && value.len >= len && memcmp(value.ptr, prefix, len) == 0;
}
