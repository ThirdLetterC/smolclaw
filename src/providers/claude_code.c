#include "sc/provider.h"

#include "tools/mcp_client_internal.h"

#include "sc/api.h"

typedef struct claude_code_provider {
    sc_allocator *alloc;
    sc_provider_options options;
    sc_string provider_name;
    sc_string default_model;
    sc_string reasoning_effort;
    sc_string mcp_server;
    sc_string mcp_tool;
    sc_string mcp_prompt_field;
    sc_string mcp_transport;
    sc_string command;
    sc_string mcp_args;
    sc_string mcp_url;
    sc_string mcp_headers;
} claude_code_provider;

static sc_status claude_code_generate(void *impl,
                                      const sc_provider_request *request,
                                      sc_allocator *alloc,
                                      sc_provider_response *out);
static sc_status claude_code_stream(void *impl,
                                    const sc_provider_request *request,
                                    sc_allocator *alloc,
                                    sc_provider_stream_callback callback,
                                    void *callback_user_data);
static void claude_code_destroy(void *impl);
static sc_status claude_code_copy_options(claude_code_provider *provider, const sc_provider_options *options);
static sc_status claude_code_build_arguments(sc_allocator *alloc,
                                             const claude_code_provider *provider,
                                             const sc_provider_request *request,
                                             sc_string *out);
static sc_status claude_code_extract_text(sc_allocator *alloc, sc_str response_json, sc_string *out);
static sc_status extract_text_from_content(sc_allocator *alloc, const sc_json_value *content, sc_string *out);
static sc_status append_json_string_prop(sc_json_value *object, sc_str key, sc_str value);
static sc_str provider_string_or_default(const sc_string *value, sc_str fallback);

static const sc_provider_vtab claude_code_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "claude-code",
    .display_name = "Claude Code MCP provider",
    .feature_flag = "SC_PROVIDER_PROCESS_ADAPTERS",
    .capabilities = SC_CONTRACT_CAP_STREAMING,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = claude_code_generate,
    .stream = claude_code_stream,
    .destroy = claude_code_destroy,
    .description_key = "sc.provider.claude_code.description",
    .config_schema_ref = "sc.schema.provider.claude_code.v1",
    .default_timeout = {.struct_size = sizeof(sc_provider_timeout_policy),
                        .connect_timeout_ms = 10000,
                        .read_timeout_ms = 30000,
                        .write_timeout_ms = 30000,
                        .total_timeout_ms = 30000,
                        .response_body_limit_bytes = 1024U * 1024U},
    .provider_modes = SC_PROVIDER_MODE_CHAT | SC_PROVIDER_MODE_STREAM,
};

sc_status sc_provider_claude_code_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    claude_code_provider *provider = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_claude_code.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    provider = sc_alloc(alloc, sizeof(*provider), _Alignof(claude_code_provider));
    if (provider == nullptr) {
        return sc_status_no_memory();
    }
    *provider = (claude_code_provider){.alloc = alloc};
    status = claude_code_copy_options(provider, options);
    if (sc_status_is_ok(status)) {
        status = sc_provider_new(alloc, &claude_code_vtab, provider, out);
    }
    if (!sc_status_is_ok(status)) {
        claude_code_destroy(provider);
    }
    return status;
}

static sc_status claude_code_generate(void *impl,
                                      const sc_provider_request *request,
                                      sc_allocator *alloc,
                                      sc_provider_response *out)
{
    claude_code_provider *provider = impl;
    sc_mcp_client_options mcp = {0};
    sc_string arguments = {0};
    sc_string raw_response = {0};
    sc_string text = {0};
    sc_status status;

    if (provider == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_claude_code.invalid_argument");
    }
    status = sc_provider_validate_request(&claude_code_vtab, &provider->options, request);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (provider->mcp_transport.len == 0) {
        return sc_status_unsupported("sc.provider_claude_code.mcp_server_missing");
    }
    if (request->cancel_requested) {
        return sc_status_cancelled("sc.provider_claude_code.cancelled");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = claude_code_build_arguments(alloc, provider, request, &arguments);
    if (sc_status_is_ok(status)) {
        mcp = (sc_mcp_client_options){
            .struct_size = sizeof(mcp),
            .transport = sc_string_as_str(&provider->mcp_transport),
            .command = sc_string_as_str(&provider->command),
            .args = sc_string_as_str(&provider->mcp_args),
            .url = sc_string_as_str(&provider->mcp_url),
            .headers = sc_string_as_str(&provider->mcp_headers),
            .max_output_bytes = claude_code_vtab.default_timeout.response_body_limit_bytes,
            .timeout_ms = provider->options.timeout_ms == 0 ? 30000 : provider->options.timeout_ms,
        };
        status = sc_mcp_client_call(alloc, &mcp, sc_string_as_str(&provider->mcp_tool), sc_string_as_str(&arguments), &raw_response);
    }
    if (sc_status_is_ok(status)) {
        status = claude_code_extract_text(alloc, sc_string_as_str(&raw_response), &text);
    }
    if (sc_status_is_ok(status) && text.len == 0) {
        status = sc_status_parse("sc.provider_claude_code.empty_response");
    }
    if (sc_status_is_ok(status)) {
        sc_provider_response_init(out, alloc);
        out->text = text;
        text = (sc_string){0};
        if (request->model.len > 0) {
            status = sc_string_from_str(alloc, request->model, &out->model);
        } else if (provider->default_model.len > 0) {
            status = sc_string_from_str(alloc, sc_string_as_str(&provider->default_model), &out->model);
        }
        if (!sc_status_is_ok(status)) {
            sc_provider_response_clear(out);
        }
    }
    sc_string_clear(&text);
    sc_string_clear(&raw_response);
    sc_string_clear(&arguments);
    return status;
}

static sc_status claude_code_stream(void *impl,
                                    const sc_provider_request *request,
                                    sc_allocator *alloc,
                                    sc_provider_stream_callback callback,
                                    void *callback_user_data)
{
    sc_provider_response response = {0};
    sc_provider_stream_event event = {0};
    sc_status status;

    if (callback == nullptr) {
        return sc_status_invalid_argument("sc.provider_claude_code.stream_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = claude_code_generate(impl, request, alloc, &response);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (response.text.len > 0) {
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_DELTA};
        status = sc_string_from_str(alloc, sc_string_as_str(&response.text), &event.text);
        if (sc_status_is_ok(status)) {
            status = callback(callback_user_data, &event);
        }
        sc_provider_stream_event_clear(&event);
    }
    if (sc_status_is_ok(status)) {
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_DONE};
        status = callback(callback_user_data, &event);
    }
    sc_provider_response_clear(&response);
    return status;
}

static void claude_code_destroy(void *impl)
{
    claude_code_provider *provider = impl;
    if (provider == nullptr) {
        return;
    }
    sc_string_clear(&provider->provider_name);
    sc_string_clear(&provider->default_model);
    sc_string_clear(&provider->reasoning_effort);
    sc_string_clear(&provider->mcp_server);
    sc_string_clear(&provider->mcp_tool);
    sc_string_clear(&provider->mcp_prompt_field);
    sc_string_clear(&provider->mcp_transport);
    sc_string_clear(&provider->command);
    sc_string_clear(&provider->mcp_args);
    sc_string_clear(&provider->mcp_url);
    sc_string_secure_clear(&provider->mcp_headers);
    sc_free(provider->alloc, provider, sizeof(*provider), _Alignof(claude_code_provider));
}

static sc_status claude_code_copy_options(claude_code_provider *provider, const sc_provider_options *options)
{
    sc_status status = sc_status_ok();
    sc_str mcp_server = options == nullptr ? sc_str_from_cstr("") : options->mcp_server;
    sc_str mcp_tool = options == nullptr ? sc_str_from_cstr("") : options->mcp_tool;
    sc_str mcp_prompt_field = options == nullptr ? sc_str_from_cstr("") : options->mcp_prompt_field;

    if (mcp_server.len == 0) {
        mcp_server = sc_str_from_cstr("claude-code");
    }
    if (mcp_tool.len == 0) {
        mcp_tool = sc_str_from_cstr("query");
    }
    if (mcp_prompt_field.len == 0) {
        mcp_prompt_field = sc_str_from_cstr("prompt");
    }
    if (options != nullptr) {
        provider->options = *options;
        status = sc_string_from_str(provider->alloc, options->provider_name, &provider->provider_name);
    }
    if (sc_status_is_ok(status) && options != nullptr) {
        status = sc_string_from_str(provider->alloc, options->default_model, &provider->default_model);
    }
    if (sc_status_is_ok(status) && options != nullptr) {
        status = sc_string_from_str(provider->alloc, options->reasoning_effort, &provider->reasoning_effort);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, mcp_server, &provider->mcp_server);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, mcp_tool, &provider->mcp_tool);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, mcp_prompt_field, &provider->mcp_prompt_field);
    }
    if (sc_status_is_ok(status) && options != nullptr) {
        status = sc_string_from_str(provider->alloc, options->mcp_transport, &provider->mcp_transport);
    }
    if (sc_status_is_ok(status) && options != nullptr) {
        status = sc_string_from_str(provider->alloc, options->command, &provider->command);
    }
    if (sc_status_is_ok(status) && options != nullptr) {
        status = sc_string_from_str(provider->alloc, options->mcp_args, &provider->mcp_args);
    }
    if (sc_status_is_ok(status) && options != nullptr) {
        status = sc_string_from_str(provider->alloc, options->mcp_url, &provider->mcp_url);
    }
    if (sc_status_is_ok(status) && options != nullptr) {
        status = sc_string_from_str(provider->alloc, options->mcp_headers, &provider->mcp_headers);
    }
    provider->options.provider_name = sc_string_as_str(&provider->provider_name);
    provider->options.default_model = sc_string_as_str(&provider->default_model);
    provider->options.reasoning_effort = sc_string_as_str(&provider->reasoning_effort);
    provider->options.mcp_server = sc_string_as_str(&provider->mcp_server);
    provider->options.mcp_tool = sc_string_as_str(&provider->mcp_tool);
    provider->options.mcp_prompt_field = sc_string_as_str(&provider->mcp_prompt_field);
    provider->options.mcp_transport = sc_string_as_str(&provider->mcp_transport);
    provider->options.command = sc_string_as_str(&provider->command);
    provider->options.mcp_args = sc_string_as_str(&provider->mcp_args);
    provider->options.mcp_url = sc_string_as_str(&provider->mcp_url);
    provider->options.mcp_headers = sc_string_as_str(&provider->mcp_headers);
    return status;
}

static sc_status claude_code_build_arguments(sc_allocator *alloc,
                                             const claude_code_provider *provider,
                                             const sc_provider_request *request,
                                             sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_status status = sc_json_object_new(alloc, &root);
    sc_str model = request->model.len > 0 ? request->model : sc_string_as_str(&provider->default_model);

    if (sc_status_is_ok(status)) {
        status = append_json_string_prop(root, provider_string_or_default(&provider->mcp_prompt_field, sc_str_from_cstr("prompt")), request->prompt);
    }
    if (sc_status_is_ok(status) && model.len > 0) {
        status = append_json_string_prop(root, sc_str_from_cstr("model"), model);
    }
    if (sc_status_is_ok(status) && request->system_instruction.len > 0) {
        status = append_json_string_prop(root, sc_str_from_cstr("system_instruction"), request->system_instruction);
    }
    if (sc_status_is_ok(status) && request->tool_specs_json.len > 0) {
        status = append_json_string_prop(root, sc_str_from_cstr("tool_specs_json"), request->tool_specs_json);
    }
    if (sc_status_is_ok(status) && request->route_hint.len > 0) {
        status = append_json_string_prop(root, sc_str_from_cstr("route_hint"), request->route_hint);
    }
    if (sc_status_is_ok(status) && provider->options.temperature > 0.0) {
        sc_json_value *number = nullptr;
        status = sc_json_number_new(alloc, provider->options.temperature, &number);
        if (sc_status_is_ok(status)) {
            status = sc_json_object_set(root, sc_str_from_cstr("temperature"), number);
            number = nullptr;
        }
        sc_json_destroy(number);
    }
    if (sc_status_is_ok(status) && provider->options.max_output_tokens > 0) {
        sc_json_value *number = nullptr;
        status = sc_json_number_new(alloc, (double)provider->options.max_output_tokens, &number);
        if (sc_status_is_ok(status)) {
            status = sc_json_object_set(root, sc_str_from_cstr("max_output_tokens"), number);
            number = nullptr;
        }
        sc_json_destroy(number);
    }
    if (sc_status_is_ok(status) && provider->reasoning_effort.len > 0) {
        status = append_json_string_prop(root, sc_str_from_cstr("reasoning_effort"), sc_string_as_str(&provider->reasoning_effort));
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_serialize(root, alloc, out);
    }
    sc_json_destroy(root);
    return status;
}

static sc_status claude_code_extract_text(sc_allocator *alloc, sc_str response_json, sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *result = nullptr;
    sc_json_parse_error error = {0};
    sc_str text = {0};
    sc_status status = sc_json_parse(alloc, response_json, &root, &error);

    if (!sc_status_is_ok(status)) {
        return status;
    }
    result = sc_json_object_get(root, sc_str_from_cstr("result"));
    if (sc_json_as_str(result, &text) || sc_json_as_str(sc_json_object_get(result, sc_str_from_cstr("text")), &text) ||
        sc_json_as_str(sc_json_object_get(result, sc_str_from_cstr("result")), &text) ||
        sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("text")), &text) ||
        sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("result")), &text)) {
        status = sc_string_from_str(alloc, text, out);
    } else {
        status = extract_text_from_content(alloc, sc_json_object_get(result, sc_str_from_cstr("content")), out);
    }
    sc_json_destroy(root);
    return status;
}

static sc_status extract_text_from_content(sc_allocator *alloc, const sc_json_value *content, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    bool found = false;

    if (sc_json_type_of(content) != SC_JSON_ARRAY) {
        return sc_status_parse("sc.provider_claude_code.empty_response");
    }
    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < sc_json_array_len(content); i += 1) {
        sc_json_value *item = sc_json_array_get(content, i);
        sc_str type = {0};
        sc_str text = {0};
        (void)sc_json_as_str(sc_json_object_get(item, sc_str_from_cstr("type")), &type);
        if (sc_str_equal(type, sc_str_from_cstr("text")) &&
            sc_json_as_str(sc_json_object_get(item, sc_str_from_cstr("text")), &text)) {
            if (found) {
                status = sc_string_builder_append_cstr(&builder, "\n");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, text);
            }
            found = true;
        }
    }
    if (sc_status_is_ok(status) && found) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
        if (sc_status_is_ok(status)) {
            status = sc_status_parse("sc.provider_claude_code.empty_response");
        }
    }
    return status;
}

static sc_status append_json_string_prop(sc_json_value *object, sc_str key, sc_str value)
{
    sc_json_value *string = nullptr;
    sc_status status = sc_json_string_new(nullptr, value, &string);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(object, key, string);
        string = nullptr;
    }
    sc_json_destroy(string);
    return status;
}

static sc_str provider_string_or_default(const sc_string *value, sc_str fallback)
{
    if (value != nullptr && value->len > 0) {
        return sc_string_as_str(value);
    }
    return fallback;
}
