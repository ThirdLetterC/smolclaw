#include "sc/provider.h"

typedef struct mock_provider {
    sc_allocator *alloc;
    sc_provider_mock_mode mode;
    sc_string text;
} mock_provider;

static sc_status mock_generate(void *impl,
                               const sc_provider_request *request,
                               sc_allocator *alloc,
                               sc_provider_response *out);
static sc_status mock_stream(void *impl,
                             const sc_provider_request *request,
                             sc_allocator *alloc,
                             sc_provider_stream_callback callback,
                             void *callback_user_data);
static void mock_destroy(void *impl);
static sc_status add_mock_tool_call(sc_provider_response *response,
                                    sc_str call_id,
                                    sc_str name,
                                    sc_str arguments_json,
                                    bool parse_arguments);

static const sc_provider_vtab mock_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock",
    .display_name = "Mock provider",
    .feature_flag = "SC_PROVIDER_MOCK",
    .capabilities = SC_CONTRACT_CAP_TOOLS | SC_CONTRACT_CAP_STREAMING,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = mock_generate,
    .stream = mock_stream,
    .destroy = mock_destroy,
    .description_key = "sc.provider.mock.description",
    .config_schema_ref = "sc.schema.provider.mock.v1",
    .default_timeout = {.struct_size = sizeof(sc_provider_timeout_policy),
                        .connect_timeout_ms = 0,
                        .read_timeout_ms = 0,
                        .write_timeout_ms = 0,
                        .total_timeout_ms = 0,
                        .response_body_limit_bytes = 0},
    .provider_modes = SC_PROVIDER_MODE_CHAT | SC_PROVIDER_MODE_STREAM | SC_PROVIDER_MODE_TOOL_CALLS,
};

sc_status sc_provider_mock_new(sc_allocator *alloc,
                               sc_provider_mock_mode mode,
                               sc_str text,
                               sc_provider **out)
{
    mock_provider *impl = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_mock.invalid_argument");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(mock_provider));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (mock_provider){.alloc = alloc, .mode = mode};
    if (text.ptr == nullptr) {
        text = sc_str_from_cstr("mock assistant");
    }
    status = sc_string_from_str(alloc, text, &impl->text);
    if (sc_status_is_ok(status)) {
        status = sc_provider_new(alloc, &mock_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        mock_destroy(impl);
    }
    return status;
}

static sc_status mock_generate(void *impl,
                               const sc_provider_request *request,
                               sc_allocator *alloc,
                               sc_provider_response *out)
{
    mock_provider *provider = impl;
    sc_status status;

    if (provider == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_mock.invalid_argument");
    }
    status = sc_provider_validate_request(&mock_vtab, nullptr, request);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (provider->mode == SC_PROVIDER_MOCK_ERROR) {
        return sc_status_http("sc.provider_mock.error");
    }

    sc_provider_response_init(out, alloc);
    status = sc_string_from_str(alloc, sc_string_as_str(&provider->text), &out->text);
    if (!sc_status_is_ok(status)) {
        sc_provider_response_clear(out);
        return status;
    }

    if (provider->mode == SC_PROVIDER_MOCK_TOOL_CALL || provider->mode == SC_PROVIDER_MOCK_MULTI_TOOL_CALL) {
        status = add_mock_tool_call(out,
                                    sc_str_from_cstr("call-1"),
                                    sc_str_from_cstr("mock_tool"),
                                    sc_str_from_cstr("{\"value\":\"one\"}"),
                                    true);
    }
    if (sc_status_is_ok(status) && provider->mode == SC_PROVIDER_MOCK_MULTI_TOOL_CALL) {
        status = add_mock_tool_call(out,
                                    sc_str_from_cstr("call-2"),
                                    sc_str_from_cstr("mock_tool"),
                                    sc_str_from_cstr("{\"value\":\"two\"}"),
                                    true);
    }
    if (provider->mode == SC_PROVIDER_MOCK_MALFORMED_TOOL_CALL) {
        status = add_mock_tool_call(out,
                                    sc_str_from_cstr("bad-call"),
                                    sc_str_from_cstr("mock_tool"),
                                    sc_str_from_cstr("{"),
                                    false);
        out->malformed_tool_call = true;
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_response_clear(out);
        return status;
    }
    return sc_status_ok();
}

static sc_status mock_stream(void *impl,
                             const sc_provider_request *request,
                             sc_allocator *alloc,
                             sc_provider_stream_callback callback,
                             void *callback_user_data)
{
    mock_provider *provider = impl;
    sc_provider_response response = {0};
    sc_provider_stream_event event = {0};
    sc_status status;
    size_t split = 0;

    if (provider == nullptr || request == nullptr || callback == nullptr) {
        return sc_status_invalid_argument("sc.provider_mock.stream_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = mock_generate(impl, request, alloc, &response);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    split = response.text.len / 2U;
    if (split == 0 && response.text.len > 0) {
        split = response.text.len;
    }
    for (size_t offset = 0; sc_status_is_ok(status) && offset < response.text.len;) {
        size_t len = offset == 0 ? split : response.text.len - offset;
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_DELTA};
        status = sc_string_from_str(alloc, sc_str_from_parts(response.text.ptr + offset, len), &event.text);
        if (sc_status_is_ok(status)) {
            status = callback(callback_user_data, &event);
        }
        sc_provider_stream_event_clear(&event);
        offset += len;
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < response.tool_calls.len; i += 1) {
        const sc_provider_tool_call *call = sc_vec_at_const(&response.tool_calls, i);
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_TOOL_CALL};
        if (call != nullptr) {
            status = sc_string_from_str(alloc, sc_string_as_str(&call->call_id), &event.tool_call.call_id);
            if (sc_status_is_ok(status)) {
                status = sc_string_from_str(alloc, sc_string_as_str(&call->name), &event.tool_call.name);
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_from_str(alloc, sc_string_as_str(&call->arguments_json), &event.tool_call.arguments_json);
            }
            if (sc_status_is_ok(status) && call->arguments != nullptr) {
                status = sc_json_clone(call->arguments, alloc, &event.tool_call.arguments);
            }
        }
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

static void mock_destroy(void *impl)
{
    mock_provider *provider = impl;
    if (provider == nullptr) {
        return;
    }
    sc_string_clear(&provider->text);
    sc_free(provider->alloc, provider, sizeof(*provider), _Alignof(mock_provider));
}

static sc_status add_mock_tool_call(sc_provider_response *response,
                                    sc_str call_id,
                                    sc_str name,
                                    sc_str arguments_json,
                                    bool parse_arguments)
{
    sc_provider_tool_call call = {.struct_size = sizeof(call)};
    sc_json_parse_error error = {0};
    sc_status status = sc_string_from_str(response->tool_calls.alloc, call_id, &call.call_id);

    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(response->tool_calls.alloc, name, &call.name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(response->tool_calls.alloc, arguments_json, &call.arguments_json);
    }
    if (sc_status_is_ok(status) && parse_arguments) {
        status = sc_json_parse(response->tool_calls.alloc, arguments_json, &call.arguments, &error);
    }
    if (sc_status_is_ok(status)) {
        status = sc_provider_response_add_tool_call(response, &call);
    }
    sc_provider_tool_call_clear(&call);
    return status;
}
