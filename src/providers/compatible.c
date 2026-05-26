#include "sc/provider.h"

#include "net/http_client.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

typedef struct compatible_provider {
    sc_allocator *alloc;
    sc_provider_options options;
    sc_string provider_name;
    sc_string base_url;
    sc_string api_key;
    sc_string credential_env;
    sc_string generic_credential_env;
    sc_string secret_value;
    sc_string default_model;
    sc_string deployment;
    sc_string api_version;
    sc_string openrouter_referer;
    sc_string openrouter_title;
    sc_string reasoning_effort;
    sc_string options_json;
    sc_string format_json;
    sc_string region;
    sc_string command;
    sc_provider_http_send_fn send;
    void *user_data;
} compatible_provider;

typedef struct compatible_async_state {
    sc_allocator *alloc;
    compatible_provider *provider;
    sc_provider_options options;
    sc_provider_generate_complete_fn generate_complete;
    sc_provider_stream_callback stream_callback;
    void *stream_callback_user_data;
    sc_provider_stream_complete_fn stream_complete;
    void *complete_user_data;
    sc_string request_json;
    sc_string credential;
    sc_string referer;
    sc_string title;
    sc_vec headers;
    sc_http_op *op;
    sc_provider_sse_parser parser;
    bool streaming;
} compatible_async_state;

static sc_status compatible_generate(void *impl,
                                     const sc_provider_request *request,
                                     sc_allocator *alloc,
                                     sc_provider_response *out);
static sc_status compatible_stream(void *impl,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_provider_stream_callback callback,
                                   void *callback_user_data);
static sc_status compatible_generate_async(void *impl,
                                           sc_async_context *context,
                                           const sc_provider_request *request,
                                           sc_allocator *alloc,
                                           sc_provider_generate_complete_fn complete,
                                           void *complete_user_data,
                                           sc_async_op **out);
static sc_status compatible_stream_async(void *impl,
                                         sc_async_context *context,
                                         const sc_provider_request *request,
                                         sc_allocator *alloc,
                                         sc_provider_stream_callback callback,
                                         void *callback_user_data,
                                         sc_provider_stream_complete_fn complete,
                                         void *complete_user_data,
                                         sc_async_op **out);
static void compatible_destroy(void *impl);
static sc_status compatible_async_state_new(compatible_provider *provider,
                                            sc_allocator *alloc,
                                            const sc_provider_options *options,
                                            compatible_async_state **out);
static sc_status compatible_async_add_header(compatible_async_state *state, sc_str name, sc_str value);
static sc_status compatible_async_build_headers(compatible_async_state *state);
static sc_status compatible_async_schedule(compatible_async_state *state,
                                           sc_async_context *context,
                                           sc_http_complete_fn complete);
static sc_status compatible_async_stream_chunk(void *user_data, sc_buf chunk);
static void compatible_async_generate_complete(void *user_data, const sc_http_response *response, sc_status status);
static void compatible_async_stream_complete(void *user_data, const sc_http_response *response, sc_status status);
static void compatible_async_state_destroy(compatible_async_state *state);
static void compatible_async_headers_clear(compatible_async_state *state);
static sc_status copy_options(compatible_provider *provider, const sc_provider_options *options);
static sc_status normalize_openai_chat_url(sc_allocator *alloc, sc_str base_url, sc_string *out);
static bool openai_base_has_version_suffix(sc_str url);
static sc_status set_string(sc_json_value *object, sc_str key, sc_str value);
static sc_status parse_tool_calls(sc_allocator *alloc, sc_json_value *message, sc_provider_response *out);
static sc_status parse_one_tool_call(sc_allocator *alloc, sc_json_value *value, sc_provider_response *out);
static sc_status parse_one_stream_tool_call(sc_allocator *alloc, sc_json_value *value, sc_provider_stream_event *out);
static bool object_string(sc_json_value *object, sc_str key, sc_str *out);
static sc_status set_raw_json(sc_allocator *alloc, sc_json_value *object, sc_str key, sc_str raw_json);
static sc_status response_error_status(sc_allocator *alloc, sc_json_value *root);
static sc_status http_error_status(sc_allocator *alloc, sc_str response_json, int http_status);
static sc_status error_status_from_json(sc_allocator *alloc,
                                        sc_json_value *root,
                                        const char *standard_key,
                                        const char *nonstandard_key);
static bool error_detail_from_object(sc_json_value *object, sc_str *out);
static sc_status diagnostic_status(sc_allocator *alloc, sc_status_code code, const char *error_key, sc_str detail);
static sc_status diagnostic_status_cstr(sc_allocator *alloc, sc_status_code code, const char *error_key, const char *detail);
static sc_status diagnostic_status_with_status(sc_allocator *alloc, const char *error_key, int http_status, sc_str detail);
static sc_str response_excerpt(sc_str response);
static sc_str options_model(const sc_provider_options *options, const sc_provider_request *request);
static sc_str reasoning_effort_option(const sc_provider_options *options);
static sc_str string_as_option(const sc_string *string);
static sc_status sse_data_payload(sc_allocator *alloc, sc_str sse_event, sc_string *out);
static sc_status set_stream_options(sc_allocator *alloc, sc_json_value *root);
static sc_status parse_usage(sc_json_value *usage, sc_provider_response *out);
static sc_status stream_event_set_usage(sc_allocator *alloc, sc_json_value *usage, sc_provider_stream_event *out);
static sc_status sse_parser_emit_event(sc_provider_sse_parser *parser,
                                       sc_str event_text,
                                       sc_allocator *event_alloc,
                                       sc_provider_stream_callback callback,
                                       void *callback_user_data);
static sc_status emit_sse_events(sc_allocator *alloc,
                                 sc_str sse_stream,
                                 sc_provider_stream_callback callback,
                                 void *callback_user_data);
static void sleep_ms(uint32_t delay_ms);

static const sc_provider_vtab compatible_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "openai-compatible",
    .display_name = "OpenAI-compatible provider",
    .feature_flag = "SC_PROVIDER_OPENAI_COMPATIBLE",
    .capabilities = SC_CONTRACT_CAP_STREAMING | SC_CONTRACT_CAP_TOOLS |
                    SC_PROVIDER_CAP_STREAMING | SC_PROVIDER_CAP_STREAMING_TOOL_EVENTS |
                    SC_PROVIDER_CAP_REASONING_EVENTS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = compatible_generate,
    .stream = compatible_stream,
    .destroy = compatible_destroy,
    .description_key = "sc.provider.openai_compatible.description",
    .config_schema_ref = "sc.schema.provider.openai_compatible.v1",
    .required_secret_keys = (const char *const[]){"api_key"},
    .required_secret_key_count = 1,
    .default_timeout = {.struct_size = sizeof(sc_provider_timeout_policy),
                        .connect_timeout_ms = 10000,
                        .read_timeout_ms = 30000,
                        .write_timeout_ms = 30000,
                        .total_timeout_ms = 30000,
                        .response_body_limit_bytes = 8U * 1024U * 1024U},
    .provider_modes = SC_PROVIDER_MODE_CHAT | SC_PROVIDER_MODE_STREAM | SC_PROVIDER_MODE_TOOL_CALLS,
    .generate_async = compatible_generate_async,
    .stream_async = compatible_stream_async,
};

sc_status sc_provider_openai_compatible_new(sc_allocator *alloc,
                                            const sc_provider_options *options,
                                            sc_provider_http_send_fn send,
                                            void *user_data,
                                            sc_provider **out)
{
    compatible_provider *impl = nullptr;
    sc_status status;

    if (out == nullptr || options == nullptr || send == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.invalid_argument");
    }
    if (options->base_url.len > 0 && !sc_provider_url_allowed(options->base_url, options->allow_loopback)) {
        return sc_status_security_denied("sc.provider_compatible.url_denied");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(compatible_provider));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (compatible_provider){.alloc = alloc, .send = send, .user_data = user_data};
    status = copy_options(impl, options);
    if (sc_status_is_ok(status)) {
        status = sc_provider_new(alloc, &compatible_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        compatible_destroy(impl);
    }
    return status;
}

sc_status sc_provider_openai_build_request(sc_allocator *alloc,
                                           const sc_provider_options *options,
                                           const sc_provider_request *request,
                                           sc_string *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *messages = nullptr;
    sc_json_value *system_message = nullptr;
    sc_json_value *message = nullptr;
    sc_json_value *stream = nullptr;
    sc_status status;

    if (request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_request.invalid_argument");
    }

    status = sc_provider_validate_request(&compatible_vtab, options, request);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &root);
    }
    if (sc_status_is_ok(status)) {
        status = set_string(root, sc_str_from_cstr("model"), options_model(options, request));
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_new(alloc, &messages);
    }
    if (sc_status_is_ok(status) && request->system_instruction.len > 0) {
        status = sc_json_object_new(alloc, &system_message);
    }
    if (sc_status_is_ok(status) && system_message != nullptr) {
        status = set_string(system_message, sc_str_from_cstr("role"), sc_str_from_cstr("system"));
    }
    if (sc_status_is_ok(status) && system_message != nullptr) {
        status = set_string(system_message, sc_str_from_cstr("content"), request->system_instruction);
    }
    if (sc_status_is_ok(status) && system_message != nullptr) {
        status = sc_json_array_append(messages, system_message);
        system_message = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_new(alloc, &message);
    }
    if (sc_status_is_ok(status)) {
        status = set_string(message, sc_str_from_cstr("role"), sc_str_from_cstr("user"));
    }
    if (sc_status_is_ok(status)) {
        status = set_string(message, sc_str_from_cstr("content"), request->prompt);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_array_append(messages, message);
        message = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("messages"), messages);
        messages = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_bool_new(alloc, options != nullptr && options->streaming, &stream);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("stream"), stream);
        stream = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = set_raw_json(alloc, root, sc_str_from_cstr("tools"), request->tool_specs_json);
    }
    if (sc_status_is_ok(status) && reasoning_effort_option(options).len > 0) {
        status = set_string(root, sc_str_from_cstr("reasoning_effort"), reasoning_effort_option(options));
    }
    if (sc_status_is_ok(status) && options != nullptr && options->format_json.len > 0) {
        status = set_raw_json(alloc, root, sc_str_from_cstr("response_format"), options->format_json);
    }
    if (sc_status_is_ok(status) && options != nullptr && options->streaming) {
        status = set_stream_options(alloc, root);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_serialize(root, alloc, out);
    }

    sc_json_destroy(root);
    sc_json_destroy(messages);
    sc_json_destroy(system_message);
    sc_json_destroy(message);
    sc_json_destroy(stream);
    return status;
}

sc_status sc_provider_openai_parse_response(sc_allocator *alloc,
                                            sc_str response_json,
                                            sc_provider_response *out)
{
    sc_json_value *root = nullptr;
    sc_json_value *choices = nullptr;
    sc_json_value *choice = nullptr;
    sc_json_value *message = nullptr;
    sc_json_value *usage = nullptr;
    sc_str content = {0};
    sc_json_parse_error error = {0};
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_response.invalid_argument");
    }
    status = sc_json_parse(alloc, response_json, &root, &error);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = response_error_status(alloc, root);
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(root);
        return status;
    }

    sc_provider_response_init(out, alloc);
    choices = sc_json_object_get(root, sc_str_from_cstr("choices"));
    choice = sc_json_array_get(choices, 0);
    message = sc_json_object_get(choice, sc_str_from_cstr("message"));
    if (object_string(message, sc_str_from_cstr("content"), &content)) {
        status = sc_string_from_str(alloc, content, &out->text);
    } else {
        status = sc_string_from_cstr(alloc, "", &out->text);
    }
    if (sc_status_is_ok(status)) {
        status = parse_tool_calls(alloc, message, out);
    }
    usage = sc_json_object_get(root, sc_str_from_cstr("usage"));
    if (sc_status_is_ok(status) && usage != nullptr) {
        status = parse_usage(usage, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_response_clear(out);
    }
    sc_json_destroy(root);
    return status;
}

sc_status sc_provider_openai_parse_sse_event(sc_allocator *alloc,
                                             sc_str sse_event,
                                             sc_provider_stream_event *out)
{
    sc_string payload = {0};
    sc_json_value *root = nullptr;
    sc_json_value *choices = nullptr;
    sc_json_value *choice = nullptr;
    sc_json_value *delta = nullptr;
    sc_json_value *usage = nullptr;
    sc_json_parse_error error = {0};
    sc_str text = {0};
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_stream.invalid_argument");
    }
    *out = (sc_provider_stream_event){.struct_size = sizeof(*out)};
    status = sse_data_payload(alloc, sse_event, &payload);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (sc_str_equal(sc_string_as_str(&payload), sc_str_from_cstr("[DONE]"))) {
        out->type = SC_PROVIDER_STREAM_DONE;
        sc_string_clear(&payload);
        return sc_status_ok();
    }

    status = sc_json_parse(alloc, sc_string_as_str(&payload), &root, &error);
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&payload);
        return status;
    }
    status = response_error_status(alloc, root);
    if (!sc_status_is_ok(status)) {
        sc_status error_status = status;

        out->type = SC_PROVIDER_STREAM_ERROR;
        status = sc_string_from_cstr(alloc, error_status.error_key, &out->error_message);
        sc_status_clear(&error_status);
        sc_json_destroy(root);
        sc_string_clear(&payload);
        return status;
    }

    usage = sc_json_object_get(root, sc_str_from_cstr("usage"));
    if (usage != nullptr && sc_json_object_get(root, sc_str_from_cstr("choices")) == nullptr) {
        status = stream_event_set_usage(alloc, usage, out);
        sc_json_destroy(root);
        sc_string_clear(&payload);
        return status;
    }

    choices = sc_json_object_get(root, sc_str_from_cstr("choices"));
    choice = sc_json_array_get(choices, 0);
    delta = sc_json_object_get(choice, sc_str_from_cstr("delta"));
    usage = sc_json_object_get(root, sc_str_from_cstr("usage"));
    if (usage != nullptr && sc_json_array_len(choices) == 0) {
        status = stream_event_set_usage(alloc, usage, out);
    } else if (choices == nullptr || choice == nullptr || delta == nullptr) {
        status = sc_status_parse("sc.provider_stream.invalid_event");
    }
    if (sc_status_is_ok(status) && object_string(delta, sc_str_from_cstr("content"), &text)) {
        out->type = SC_PROVIDER_STREAM_DELTA;
        status = sc_string_from_str(alloc, text, &out->text);
    } else if (sc_status_is_ok(status) &&
               (object_string(delta, sc_str_from_cstr("reasoning_content"), &text) ||
                object_string(delta, sc_str_from_cstr("reasoning"), &text) ||
                object_string(delta, sc_str_from_cstr("reasoning_delta"), &text))) {
        out->type = SC_PROVIDER_STREAM_REASONING_DELTA;
        status = sc_string_from_str(alloc, text, &out->text);
    } else if (sc_status_is_ok(status) && sc_json_object_get(delta, sc_str_from_cstr("tool_calls")) != nullptr) {
        sc_json_value *calls = sc_json_object_get(delta, sc_str_from_cstr("tool_calls"));
        if (sc_json_type_of(calls) != SC_JSON_ARRAY || sc_json_array_len(calls) == 0) {
            status = sc_status_parse("sc.provider_tool_call.expected_array");
        } else {
            out->type = SC_PROVIDER_STREAM_TOOL_CALL;
            status = parse_one_stream_tool_call(alloc, sc_json_array_get(calls, 0), out);
        }
    } else if (sc_status_is_ok(status) && out->type != SC_PROVIDER_STREAM_FINAL_USAGE) {
        out->type = SC_PROVIDER_STREAM_DELTA;
        status = sc_string_from_cstr(alloc, "", &out->text);
    }

    sc_json_destroy(root);
    sc_string_clear(&payload);
    return status;
}

static sc_status compatible_generate(void *impl,
                                     const sc_provider_request *request,
                                     sc_allocator *alloc,
                                     sc_provider_response *out)
{
    compatible_provider *provider = impl;
    sc_string request_json = {0};
    sc_string response_json = {0};
    int http_status = 0;
    sc_status status;

    if (provider == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.invalid_argument");
    }

    status = sc_provider_openai_build_request(alloc, &provider->options, request, &request_json);
    if (sc_status_is_ok(status)) {
        uint32_t max_attempts = provider->options.max_retries + 1U;
        for (uint32_t attempt = 1; attempt <= max_attempts; attempt += 1) {
            sc_string_clear(&response_json);
            http_status = 0;
            status = provider->send(provider->user_data,
                                    &provider->options,
                                    sc_string_as_str(&request_json),
                                    alloc,
                                    &response_json,
                                    &http_status);
            if (!sc_provider_should_retry(status, http_status) || attempt >= max_attempts) {
                break;
            }
            sc_status_clear(&status);
            sleep_ms(provider->options.retry_backoff_ms * attempt);
        }
    }
    if (sc_status_is_ok(status) && (http_status < 200 || http_status >= 300)) {
        status = http_error_status(alloc, sc_string_as_str(&response_json), http_status);
    }
    if (sc_status_is_ok(status)) {
        status = sc_provider_openai_parse_response(alloc, sc_string_as_str(&response_json), out);
    }

    sc_string_clear(&response_json);
    sc_string_clear(&request_json);
    return status;
}

static sc_status compatible_stream(void *impl,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_provider_stream_callback callback,
                                   void *callback_user_data)
{
    compatible_provider *provider = impl;
    sc_provider_options options = {0};
    sc_string request_json = {0};
    sc_string response_json = {0};
    int http_status = 0;
    sc_status status;

    if (provider == nullptr || request == nullptr || callback == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.stream_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    options = provider->options;
    options.streaming = true;
    status = sc_provider_openai_build_request(alloc, &options, request, &request_json);
    if (sc_status_is_ok(status)) {
        uint32_t max_attempts = options.max_retries + 1U;
        for (uint32_t attempt = 1; attempt <= max_attempts; attempt += 1) {
            sc_string_clear(&response_json);
            http_status = 0;
            status = provider->send(provider->user_data,
                                    &options,
                                    sc_string_as_str(&request_json),
                                    alloc,
                                    &response_json,
                                    &http_status);
            if (!sc_provider_should_retry(status, http_status) || attempt >= max_attempts) {
                break;
            }
            sleep_ms(options.retry_backoff_ms * attempt);
        }
    }
    if (sc_status_is_ok(status) && (http_status < 200 || http_status >= 300)) {
        status = http_error_status(alloc, sc_string_as_str(&response_json), http_status);
    }
    if (sc_status_is_ok(status)) {
        status = emit_sse_events(alloc, sc_string_as_str(&response_json), callback, callback_user_data);
    }

    sc_string_clear(&response_json);
    sc_string_clear(&request_json);
    return status;
}

static sc_status compatible_generate_async(void *impl,
                                           sc_async_context *context,
                                           const sc_provider_request *request,
                                           sc_allocator *alloc,
                                           sc_provider_generate_complete_fn complete,
                                           void *complete_user_data,
                                           sc_async_op **out)
{
    compatible_provider *provider = impl;
    compatible_async_state *state = nullptr;
    sc_status status;

    if (provider == nullptr || context == nullptr || request == nullptr || complete == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.async_invalid_argument");
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = compatible_async_state_new(provider, alloc, &provider->options, &state);
    if (sc_status_is_ok(status)) {
        state->generate_complete = complete;
        state->complete_user_data = complete_user_data;
        status = sc_provider_openai_build_request(alloc, &state->options, request, &state->request_json);
    }
    if (sc_status_is_ok(status)) {
        status = compatible_async_build_headers(state);
    }
    if (sc_status_is_ok(status)) {
        status = compatible_async_schedule(state, context, compatible_async_generate_complete);
    }
    if (!sc_status_is_ok(status)) {
        complete(complete_user_data, nullptr, status);
        compatible_async_state_destroy(state);
    }
    return sc_status_ok();
}

static sc_status compatible_stream_async(void *impl,
                                         sc_async_context *context,
                                         const sc_provider_request *request,
                                         sc_allocator *alloc,
                                         sc_provider_stream_callback callback,
                                         void *callback_user_data,
                                         sc_provider_stream_complete_fn complete,
                                         void *complete_user_data,
                                         sc_async_op **out)
{
    compatible_provider *provider = impl;
    compatible_async_state *state = nullptr;
    sc_provider_options options = {0};
    sc_status status;

    if (provider == nullptr || context == nullptr || request == nullptr || callback == nullptr || complete == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.async_stream_invalid_argument");
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    options = provider->options;
    options.streaming = true;
    status = compatible_async_state_new(provider, alloc, &options, &state);
    if (sc_status_is_ok(status)) {
        state->streaming = true;
        state->stream_callback = callback;
        state->stream_callback_user_data = callback_user_data;
        state->stream_complete = complete;
        state->complete_user_data = complete_user_data;
        sc_provider_sse_parser_init(&state->parser, alloc, 64U * 1024U);
        status = sc_provider_openai_build_request(alloc, &state->options, request, &state->request_json);
    }
    if (sc_status_is_ok(status)) {
        status = compatible_async_build_headers(state);
    }
    if (sc_status_is_ok(status)) {
        status = compatible_async_schedule(state, context, compatible_async_stream_complete);
    }
    if (!sc_status_is_ok(status)) {
        complete(complete_user_data, status);
        compatible_async_state_destroy(state);
    }
    return sc_status_ok();
}

static sc_status compatible_async_state_new(compatible_provider *provider,
                                            sc_allocator *alloc,
                                            const sc_provider_options *options,
                                            compatible_async_state **out)
{
    compatible_async_state *state = nullptr;

    if (provider == nullptr || options == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.async_invalid_argument");
    }
    state = sc_alloc(alloc, sizeof(*state), _Alignof(compatible_async_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (compatible_async_state){
        .alloc = alloc,
        .provider = provider,
        .options = *options,
    };
    sc_vec_init(&state->headers, alloc, sizeof(sc_http_header));
    *out = state;
    return sc_status_ok();
}

static sc_status compatible_async_add_header(compatible_async_state *state, sc_str name, sc_str value)
{
    sc_string name_copy = {0};
    sc_string value_copy = {0};
    sc_http_header header;
    sc_status status;

    if (state == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.async_header_invalid_argument");
    }
    status = sc_string_from_str(state->alloc, name, &name_copy);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(state->alloc, value, &value_copy);
    }
    if (sc_status_is_ok(status)) {
        header = (sc_http_header){
            .name = sc_string_as_str(&name_copy),
            .value = sc_string_as_str(&value_copy),
        };
        status = sc_vec_push(&state->headers, &header);
    }
    if (sc_status_is_ok(status)) {
        name_copy = (sc_string){0};
        value_copy = (sc_string){0};
    }
    sc_string_clear(&name_copy);
    sc_string_clear(&value_copy);
    return status;
}

static sc_status compatible_async_build_headers(compatible_async_state *state)
{
    sc_status status;

    if (state == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.async_header_invalid_argument");
    }
    status = compatible_async_add_header(state, sc_str_from_cstr("Content-Type"), sc_str_from_cstr("application/json"));
    if (sc_status_is_ok(status)) {
        status = sc_provider_resolve_credential(state->alloc, &state->options, &state->credential);
        if (status.code == SC_ERR_UNSUPPORTED || status.code == SC_ERR_INVALID_ARGUMENT) {
            sc_status_clear(&status);
            status = sc_status_ok();
        }
    }
    if (sc_status_is_ok(status) && state->credential.len > 0) {
        sc_string_builder builder = {0};
        sc_string bearer = {0};
        sc_string_builder_init(&builder, state->alloc);
        status = sc_string_builder_append_cstr(&builder, "Bearer ");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&state->credential));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &bearer);
        } else {
            sc_string_builder_clear(&builder);
        }
        if (sc_status_is_ok(status)) {
            status = compatible_async_add_header(state, sc_str_from_cstr("Authorization"), sc_string_as_str(&bearer));
        }
        sc_string_clear(&bearer);
    }
    if (sc_status_is_ok(status) && sc_str_equal(state->options.provider_name, sc_str_from_cstr("openrouter"))) {
        sc_str referer = state->options.openrouter_referer.len == 0 ? sc_str_from_cstr("https://smolclaw.local") :
                                                                      state->options.openrouter_referer;
        sc_str title = state->options.openrouter_title.len == 0 ? sc_str_from_cstr("SmolClaw") :
                                                                  state->options.openrouter_title;
        status = compatible_async_add_header(state, sc_str_from_cstr("HTTP-Referer"), referer);
        if (sc_status_is_ok(status)) {
            status = compatible_async_add_header(state, sc_str_from_cstr("X-Title"), title);
        }
    }
    return status;
}

static sc_status compatible_async_schedule(compatible_async_state *state,
                                           sc_async_context *context,
                                           sc_http_complete_fn complete)
{
    sc_http_client *client = nullptr;
    sc_status status;
    sc_string url = {0};

    if (state == nullptr || context == nullptr || complete == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.async_invalid_argument");
    }
    status = normalize_openai_chat_url(state->alloc, state->options.base_url, &url);
    if (sc_status_is_ok(status)) {
        status = sc_async_context_http_client(context, &client);
    }
    if (sc_status_is_ok(status)) {
        sc_http_request http_request = {
            .struct_size = sizeof(http_request),
            .method = sc_str_from_cstr("POST"),
            .url = sc_string_as_str(&url),
            .headers = state->headers.len == 0 ? nullptr : state->headers.ptr,
            .header_count = state->headers.len,
            .body = sc_string_as_str(&state->request_json),
            .user_agent = sc_str_from_cstr("smolclaw-c/0.1 provider-http"),
            .max_response_bytes = 8U * 1024U * 1024U,
            .timeout_ms = state->options.timeout_ms == 0 ? 30000 : state->options.timeout_ms,
            .connect_timeout_ms = state->options.timeout_ms == 0 || state->options.timeout_ms > 10000 ?
                10000 :
                state->options.timeout_ms,
            .follow_location = false,
            .on_chunk = state->streaming ? compatible_async_stream_chunk : nullptr,
            .chunk_user_data = state,
        };
        status = sc_http_client_perform(client, &http_request, state->alloc, complete, state, &state->op);
    }
    sc_string_clear(&url);
    return status;
}

static sc_status compatible_async_stream_chunk(void *user_data, sc_buf chunk)
{
    compatible_async_state *state = user_data;
    if (state == nullptr || chunk.ptr == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.async_stream_invalid_argument");
    }
    return sc_provider_sse_parser_push(&state->parser,
                                       sc_str_from_parts((const char *)chunk.ptr, chunk.len),
                                       state->alloc,
                                       state->stream_callback,
                                       state->stream_callback_user_data);
}

static void compatible_async_generate_complete(void *user_data, const sc_http_response *response, sc_status status)
{
    compatible_async_state *state = user_data;
    sc_provider_response provider_response = {0};
    sc_status final_status = status;

    if (state == nullptr) {
        sc_status_clear(&status);
        return;
    }
    if (sc_status_is_ok(final_status) && response != nullptr && (response->http_status < 200 || response->http_status >= 300)) {
        final_status = http_error_status(state->alloc,
                                         sc_str_from_parts((const char *)response->body.ptr, response->body.len),
                                         (int)response->http_status);
    }
    if (sc_status_is_ok(final_status) && response != nullptr) {
        final_status = sc_provider_openai_parse_response(state->alloc,
                                                         sc_str_from_parts((const char *)response->body.ptr, response->body.len),
                                                         &provider_response);
    }
    state->generate_complete(state->complete_user_data, sc_status_is_ok(final_status) ? &provider_response : nullptr, final_status);
    sc_provider_response_clear(&provider_response);
    compatible_async_state_destroy(state);
}

static void compatible_async_stream_complete(void *user_data, const sc_http_response *response, sc_status status)
{
    compatible_async_state *state = user_data;
    sc_status final_status = status;

    if (state == nullptr) {
        sc_status_clear(&status);
        return;
    }
    if (sc_status_is_ok(final_status) && response != nullptr && (response->http_status < 200 || response->http_status >= 300)) {
        final_status = http_error_status(state->alloc,
                                         sc_str_from_parts((const char *)response->body.ptr, response->body.len),
                                         (int)response->http_status);
    }
    if (sc_status_is_ok(final_status)) {
        final_status = sc_provider_sse_parser_finish(&state->parser, state->stream_callback, state->stream_callback_user_data);
    }
    state->stream_complete(state->complete_user_data, final_status);
    compatible_async_state_destroy(state);
}

static void compatible_async_state_destroy(compatible_async_state *state)
{
    if (state == nullptr) {
        return;
    }
    sc_http_op_destroy(state->op);
    sc_provider_sse_parser_clear(&state->parser);
    compatible_async_headers_clear(state);
    sc_string_clear(&state->request_json);
    sc_string_secure_clear(&state->credential);
    sc_free(state->alloc, state, sizeof(*state), _Alignof(compatible_async_state));
}

static void compatible_async_headers_clear(compatible_async_state *state)
{
    if (state == nullptr) {
        return;
    }
    for (size_t i = 0; i < state->headers.len; i += 1) {
        const sc_http_header *header = sc_vec_at(&state->headers, i);
        sc_string name = {.ptr = header == nullptr ? nullptr : (char *)header->name.ptr,
                          .len = header == nullptr ? 0 : header->name.len,
                          .alloc = state->alloc};
        sc_string value = {.ptr = header == nullptr ? nullptr : (char *)header->value.ptr,
                           .len = header == nullptr ? 0 : header->value.len,
                           .alloc = state->alloc};
        sc_string_clear(&name);
        sc_string_clear(&value);
    }
    sc_vec_clear(&state->headers);
}

static sc_status emit_sse_events(sc_allocator *alloc,
                                 sc_str sse_stream,
                                 sc_provider_stream_callback callback,
                                 void *callback_user_data)
{
    sc_provider_sse_parser parser = {0};
    sc_status status;

    sc_provider_sse_parser_init(&parser, alloc, 64U * 1024U);
    status = sc_provider_sse_parser_push(&parser, sse_stream, alloc, callback, callback_user_data);
    if (sc_status_is_ok(status)) {
        status = sc_provider_sse_parser_finish(&parser, callback, callback_user_data);
    }
    sc_provider_sse_parser_clear(&parser);
    return status;
}

void sc_provider_sse_parser_init(sc_provider_sse_parser *parser, sc_allocator *alloc, size_t max_partial_bytes)
{
    if (parser == nullptr) {
        return;
    }
    *parser = (sc_provider_sse_parser){
        .struct_size = sizeof(*parser),
        .alloc = alloc == nullptr ? sc_allocator_heap() : alloc,
        .max_partial_bytes = max_partial_bytes == 0 ? 64U * 1024U : max_partial_bytes,
    };
}

sc_status sc_provider_sse_parser_push(sc_provider_sse_parser *parser,
                                      sc_str chunk,
                                      sc_allocator *event_alloc,
                                      sc_provider_stream_callback callback,
                                      void *callback_user_data)
{
    sc_string_builder builder = {0};
    sc_string combined = {0};
    size_t offset = 0;
    sc_status status;

    if (parser == nullptr || callback == nullptr || parser->failed) {
        return sc_status_invalid_argument("sc.provider_sse.invalid_argument");
    }
    if (chunk.ptr == nullptr || chunk.len == 0) {
        return sc_status_ok();
    }
    if (parser->partial.len + chunk.len > parser->max_partial_bytes) {
        parser->failed = true;
        return sc_status_invalid_argument("sc.provider_sse.partial_too_large");
    }

    sc_string_builder_init(&builder, parser->alloc);
    status = sc_string_builder_append(&builder, sc_string_as_str(&parser->partial));
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, chunk);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &combined);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (!sc_status_is_ok(status)) {
        parser->failed = true;
        return status;
    }
    sc_string_clear(&parser->partial);

    while (sc_status_is_ok(status) && offset < combined.len) {
        size_t event_len = 0;
        bool found = false;
        while (offset + event_len + 1 < combined.len) {
            if (combined.ptr[offset + event_len] == '\n' && combined.ptr[offset + event_len + 1] == '\n') {
                found = true;
                break;
            }
            event_len += 1;
        }
        if (!found) {
            status = sc_string_from_str(parser->alloc,
                                        sc_str_from_parts(combined.ptr + offset, combined.len - offset),
                                        &parser->partial);
            break;
        }
        if (event_len > parser->max_partial_bytes) {
            status = sc_status_invalid_argument("sc.provider_sse.event_too_large");
            break;
        }
        if (event_len > 0) {
            status = sse_parser_emit_event(parser,
                                           sc_str_from_parts(combined.ptr + offset, event_len),
                                           event_alloc,
                                           callback,
                                           callback_user_data);
        }
        offset += event_len + 2;
        while (offset < combined.len && (combined.ptr[offset] == '\n' || combined.ptr[offset] == '\r')) {
            offset += 1;
        }
    }
    sc_string_clear(&combined);
    if (!sc_status_is_ok(status)) {
        parser->failed = true;
    }
    return status;
}

sc_status sc_provider_sse_parser_finish(sc_provider_sse_parser *parser,
                                        sc_provider_stream_callback callback,
                                        void *callback_user_data)
{
    sc_status status = sc_status_ok();

    if (parser == nullptr || callback == nullptr || parser->failed) {
        return sc_status_invalid_argument("sc.provider_sse.finish_invalid_argument");
    }
    if (parser->partial.len > 0) {
        parser->failed = true;
        return sc_status_parse("sc.provider_sse.truncated_event");
    }
    if (!parser->done) {
        sc_provider_stream_event event = {
            .struct_size = sizeof(event),
            .type = SC_PROVIDER_STREAM_DONE,
        };
        status = callback(callback_user_data, &event);
        if (sc_status_is_ok(status)) {
            parser->done = true;
        }
    }
    return status;
}

void sc_provider_sse_parser_clear(sc_provider_sse_parser *parser)
{
    if (parser == nullptr) {
        return;
    }
    sc_string_clear(&parser->partial);
    *parser = (sc_provider_sse_parser){0};
}

static sc_status sse_parser_emit_event(sc_provider_sse_parser *parser,
                                       sc_str event_text,
                                       sc_allocator *event_alloc,
                                       sc_provider_stream_callback callback,
                                       void *callback_user_data)
{
    sc_provider_stream_event event = {0};
    sc_status status = sc_provider_openai_parse_sse_event(event_alloc, event_text, &event);

    if (sc_status_is_ok(status)) {
        parser->done = event.type == SC_PROVIDER_STREAM_DONE;
        status = callback(callback_user_data, &event);
    }
    sc_provider_stream_event_clear(&event);
    return status;
}

static void sleep_ms(uint32_t delay_ms)
{
    struct timespec requested = {
        .tv_sec = (time_t)(delay_ms / 1000U),
        .tv_nsec = (long)(delay_ms % 1000U) * 1000000L,
    };
    if (delay_ms == 0) {
        return;
    }
    (void)nanosleep(&requested, nullptr);
}

static void compatible_destroy(void *impl)
{
    compatible_provider *provider = impl;
    if (provider == nullptr) {
        return;
    }
    sc_string_clear(&provider->provider_name);
    sc_string_clear(&provider->base_url);
    sc_string_secure_clear(&provider->api_key);
    sc_string_clear(&provider->credential_env);
    sc_string_clear(&provider->generic_credential_env);
    sc_string_secure_clear(&provider->secret_value);
    sc_string_clear(&provider->default_model);
    sc_string_clear(&provider->deployment);
    sc_string_clear(&provider->api_version);
    sc_string_clear(&provider->openrouter_referer);
    sc_string_clear(&provider->openrouter_title);
    sc_string_clear(&provider->reasoning_effort);
    sc_string_clear(&provider->options_json);
    sc_string_clear(&provider->format_json);
    sc_string_clear(&provider->region);
    sc_string_clear(&provider->command);
    sc_free(provider->alloc, provider, sizeof(*provider), _Alignof(compatible_provider));
}

static sc_status copy_options(compatible_provider *provider, const sc_provider_options *options)
{
    sc_string normalized_url = {0};
    sc_status status = normalize_openai_chat_url(provider->alloc, options->base_url, &normalized_url);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->provider_name, &provider->provider_name);
    }
    if (sc_status_is_ok(status)) {
        provider->base_url = normalized_url;
        normalized_url = (sc_string){0};
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->api_key, &provider->api_key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->credential_env, &provider->credential_env);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->generic_credential_env, &provider->generic_credential_env);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->secret_value, &provider->secret_value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->default_model, &provider->default_model);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->deployment, &provider->deployment);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->api_version, &provider->api_version);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->openrouter_referer, &provider->openrouter_referer);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->openrouter_title, &provider->openrouter_title);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->reasoning_effort, &provider->reasoning_effort);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->options_json, &provider->options_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->format_json, &provider->format_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->region, &provider->region);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(provider->alloc, options->command, &provider->command);
    }
    if (sc_status_is_ok(status)) {
        provider->options = *options;
        provider->options.provider_name = string_as_option(&provider->provider_name);
        provider->options.base_url = string_as_option(&provider->base_url);
        provider->options.api_key = string_as_option(&provider->api_key);
        provider->options.credential_env = string_as_option(&provider->credential_env);
        provider->options.generic_credential_env = string_as_option(&provider->generic_credential_env);
        provider->options.secret_value = string_as_option(&provider->secret_value);
        provider->options.default_model = string_as_option(&provider->default_model);
        provider->options.deployment = string_as_option(&provider->deployment);
        provider->options.api_version = string_as_option(&provider->api_version);
        provider->options.openrouter_referer = string_as_option(&provider->openrouter_referer);
        provider->options.openrouter_title = string_as_option(&provider->openrouter_title);
        provider->options.reasoning_effort = string_as_option(&provider->reasoning_effort);
        provider->options.options_json = string_as_option(&provider->options_json);
        provider->options.format_json = string_as_option(&provider->format_json);
        provider->options.region = string_as_option(&provider->region);
        provider->options.command = string_as_option(&provider->command);
    }
    sc_string_clear(&normalized_url);
    return status;
}

static sc_status normalize_openai_chat_url(sc_allocator *alloc, sc_str base_url, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;
    sc_str url = base_url;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_compatible.url_invalid_argument");
    }
    if (url.ptr == nullptr || url.len == 0) {
        return sc_string_from_cstr(alloc, "", out);
    }
    while (url.len > 1 && url.ptr[url.len - 1] == '/') {
        url.len -= 1;
    }
    if ((url.len >= 17 && memcmp(url.ptr + url.len - 17, "/chat/completions", 17) == 0) ||
        (url.len >= 18 && memcmp(url.ptr + url.len - 18, "/chat/completions?", 18) == 0)) {
        return sc_string_from_str(alloc, url, out);
    }
    for (size_t i = 0; i + 18 <= url.len; i += 1) {
        if (memcmp(url.ptr + i, "/chat/completions?", 18) == 0) {
            return sc_string_from_str(alloc, url, out);
        }
    }

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, url);
    if (sc_status_is_ok(status) && !openai_base_has_version_suffix(url)) {
        status = sc_string_builder_append_cstr(&builder, "/v1");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/chat/completions");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static bool openai_base_has_version_suffix(sc_str url)
{
    size_t slash = url.len;

    while (slash > 0 && url.ptr[slash - 1] != '/') {
        slash -= 1;
    }
    if (slash + 2 > url.len || url.ptr[slash] != 'v') {
        return false;
    }
    for (size_t i = slash + 1; i < url.len; i += 1) {
        if (url.ptr[i] < '0' || url.ptr[i] > '9') {
            return false;
        }
    }
    return true;
}

static sc_status set_string(sc_json_value *object, sc_str key, sc_str value)
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

static sc_status parse_tool_calls(sc_allocator *alloc, sc_json_value *message, sc_provider_response *out)
{
    sc_json_value *calls = sc_json_object_get(message, sc_str_from_cstr("tool_calls"));
    sc_status status = sc_status_ok();

    if (calls != nullptr && sc_json_type_of(calls) != SC_JSON_ARRAY) {
        return sc_status_parse("sc.provider_tool_call.expected_array");
    }
    for (size_t i = 0; calls != nullptr && sc_status_is_ok(status) && i < sc_json_array_len(calls); i += 1) {
        status = parse_one_tool_call(alloc, sc_json_array_get(calls, i), out);
    }
    return status;
}

static sc_status parse_one_tool_call(sc_allocator *alloc, sc_json_value *value, sc_provider_response *out)
{
    sc_provider_tool_call call = {.struct_size = sizeof(call)};
    sc_json_value *function = sc_json_object_get(value, sc_str_from_cstr("function"));
    sc_str id = {0};
    sc_str name = {0};
    sc_str arguments = {0};
    sc_json_parse_error error = {0};
    sc_status status;

    if (!object_string(value, sc_str_from_cstr("id"), &id)) {
        id = sc_str_from_cstr("");
    }
    if (value == nullptr || function == nullptr ||
        !object_string(function, sc_str_from_cstr("name"), &name) ||
        name.len == 0) {
        out->malformed_tool_call = true;
        return sc_status_parse("sc.provider_tool_call.malformed");
    }
    if (!object_string(function, sc_str_from_cstr("arguments"), &arguments)) {
        arguments = sc_str_from_cstr("{}");
    }

    status = sc_string_from_str(alloc, id, &call.call_id);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, name, &call.name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, arguments, &call.arguments_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_parse(alloc, arguments, &call.arguments, &error);
        if (!sc_status_is_ok(status)) {
            out->malformed_tool_call = true;
            status = sc_status_ok();
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_provider_response_add_tool_call(out, &call);
    }
    sc_provider_tool_call_clear(&call);
    return status;
}

static bool object_string(sc_json_value *object, sc_str key, sc_str *out)
{
    sc_json_value *value = sc_json_object_get(object, key);
    return sc_json_as_str(value, out);
}

static sc_status set_raw_json(sc_allocator *alloc, sc_json_value *object, sc_str key, sc_str raw_json)
{
    sc_json_value *value = nullptr;
    sc_json_parse_error error = {0};
    sc_status status;

    if (raw_json.ptr == nullptr || raw_json.len == 0) {
        return sc_status_ok();
    }
    status = sc_json_parse(alloc, raw_json, &value, &error);
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(object, key, value);
        value = nullptr;
    }
    sc_json_destroy(value);
    return status;
}

static sc_status parse_one_stream_tool_call(sc_allocator *alloc, sc_json_value *value, sc_provider_stream_event *out)
{
    sc_json_value *function = sc_json_object_get(value, sc_str_from_cstr("function"));
    sc_str id = {0};
    sc_str name = {0};
    sc_str arguments = {0};
    sc_json_parse_error error = {0};
    sc_status status;

    if (out == nullptr || value == nullptr || function == nullptr) {
        return sc_status_parse("sc.provider_tool_call.malformed");
    }
    if (!object_string(value, sc_str_from_cstr("id"), &id)) {
        id = sc_str_from_cstr("");
    }
    if (!object_string(function, sc_str_from_cstr("name"), &name)) {
        name = sc_str_from_cstr("");
    }
    if (!object_string(function, sc_str_from_cstr("arguments"), &arguments)) {
        arguments = sc_str_from_cstr("");
    }
    status = sc_string_from_str(alloc, id, &out->tool_call.call_id);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, name, &out->tool_call.name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, arguments, &out->tool_call.arguments_json);
    }
    if (sc_status_is_ok(status) && arguments.len > 0) {
        status = sc_json_parse(alloc, arguments, &out->tool_call.arguments, &error);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            status = sc_status_ok();
        }
    }
    return status;
}

static sc_status response_error_status(sc_allocator *alloc, sc_json_value *root)
{
    return error_status_from_json(alloc,
                                  root,
                                  "sc.provider_compatible.error_response",
                                  "sc.provider_compatible.nonstandard_error_response");
}

static sc_status http_error_status(sc_allocator *alloc, sc_str response_json, int http_status)
{
    sc_json_value *root = nullptr;
    sc_json_parse_error parse_error = {0};
    sc_status status = sc_json_parse(alloc, response_json, &root, &parse_error);

    if (sc_status_is_ok(status)) {
        status = error_status_from_json(alloc,
                                        root,
                                        http_status == 404 ? "sc.provider_compatible.endpoint_not_found"
                                                           : "sc.provider_compatible.http_error",
                                        "sc.provider_compatible.nonstandard_error_response");
        sc_json_destroy(root);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
    }
    if (http_status == 404) {
        return diagnostic_status_cstr(alloc,
                                      SC_ERR_HTTP,
                                      "sc.provider_compatible.endpoint_not_found",
                                      "OpenAI-compatible endpoint returned 404. Check base_url; expected a server root, /v1, or /v1/chat/completions. If this endpoint is used for model discovery, it may not expose /models.");
    }
    return diagnostic_status_with_status(alloc,
                                         "sc.provider_compatible.non_json_error_response",
                                         http_status,
                                         response_excerpt(response_json));
}

static sc_status error_status_from_json(sc_allocator *alloc,
                                        sc_json_value *root,
                                        const char *standard_key,
                                        const char *nonstandard_key)
{
    sc_json_value *error = sc_json_object_get(root, sc_str_from_cstr("error"));
    sc_str detail = {0};

    if (error != nullptr) {
        if (sc_json_type_of(error) == SC_JSON_OBJECT) {
            if (object_string(error, sc_str_from_cstr("message"), &detail)) {
                return diagnostic_status(alloc, SC_ERR_HTTP, standard_key, detail);
            }
            if (error_detail_from_object(error, &detail)) {
                return diagnostic_status(alloc, SC_ERR_HTTP, nonstandard_key, detail);
            }
        } else if (sc_json_as_str(error, &detail) && detail.len > 0) {
            return diagnostic_status(alloc, SC_ERR_HTTP, nonstandard_key, detail);
        }
        return sc_status_http(nonstandard_key);
    }
    if (error_detail_from_object(root, &detail)) {
        return diagnostic_status(alloc, SC_ERR_HTTP, nonstandard_key, detail);
    }
    return sc_status_ok();
}

static bool error_detail_from_object(sc_json_value *object, sc_str *out)
{
    static const char *const keys[] = {
        "message",
        "error_description",
        "detail",
        "reason",
        "title",
    };

    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i += 1) {
        sc_str value = {0};
        if (object_string(object, sc_str_from_cstr(keys[i]), &value) && value.len > 0) {
            *out = value;
            return true;
        }
    }
    return false;
}

static sc_status diagnostic_status(sc_allocator *alloc, sc_status_code code, const char *error_key, sc_str detail)
{
    sc_string message = {0};
    sc_status status;

    if (detail.ptr == nullptr || detail.len == 0) {
        return sc_status_make(code, error_key, nullptr);
    }
    status = sc_string_from_str(alloc, detail, &message);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = sc_status_make_owned(alloc, code, error_key, message.ptr);
    sc_string_clear(&message);
    return status;
}

static sc_status diagnostic_status_cstr(sc_allocator *alloc, sc_status_code code, const char *error_key, const char *detail)
{
    return sc_status_make_owned(alloc, code, error_key, detail);
}

static sc_status diagnostic_status_with_status(sc_allocator *alloc, const char *error_key, int http_status, sc_str detail)
{
    sc_string_builder builder = {0};
    sc_string message = {0};
    sc_status status;
    char status_text[32] = {0};

    (void)snprintf(status_text, sizeof(status_text), "%d", http_status);
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "OpenAI-compatible endpoint returned HTTP ");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, status_text);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, " with a non-JSON or nonstandard error response");
    }
    if (sc_status_is_ok(status) && detail.len > 0) {
        status = sc_string_builder_append_cstr(&builder, ": ");
    }
    if (sc_status_is_ok(status) && detail.len > 0) {
        status = sc_string_builder_append(&builder, detail);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &message);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = sc_status_make_owned(alloc, SC_ERR_HTTP, error_key, message.ptr);
    sc_string_clear(&message);
    return status;
}

static sc_str response_excerpt(sc_str response)
{
    size_t len = response.len > 160U ? 160U : response.len;

    if (response.ptr == nullptr) {
        return sc_str_from_cstr("");
    }
    while (len > 0 && (response.ptr[len - 1] == '\n' || response.ptr[len - 1] == '\r')) {
        len -= 1;
    }
    return sc_str_from_parts(response.ptr, len);
}

static sc_str options_model(const sc_provider_options *options, const sc_provider_request *request)
{
    if (request->model.ptr != nullptr && request->model.len > 0) {
        return request->model;
    }
    if (options != nullptr && options->default_model.ptr != nullptr && options->default_model.len > 0) {
        return options->default_model;
    }
    return sc_str_from_cstr("");
}

static sc_str reasoning_effort_option(const sc_provider_options *options)
{
    if (options == nullptr) {
        return sc_str_from_cstr("");
    }
    if (options->reasoning_effort.len > 0) {
        return options->reasoning_effort;
    }
    switch (options->thinking_level) {
    case SC_PROVIDER_THINKING_DISABLED:
        return sc_str_from_cstr("none");
    case SC_PROVIDER_THINKING_LOW:
        return sc_str_from_cstr("low");
    case SC_PROVIDER_THINKING_MEDIUM:
        return sc_str_from_cstr("medium");
    case SC_PROVIDER_THINKING_HIGH:
        return sc_str_from_cstr("high");
    case SC_PROVIDER_THINKING_DEFAULT:
        break;
    }
    return sc_str_from_cstr("");
}

static sc_str string_as_option(const sc_string *string)
{
    if (string == nullptr || string->ptr == nullptr) {
        return sc_str_from_cstr("");
    }
    return sc_string_as_str(string);
}

static sc_status set_stream_options(sc_allocator *alloc, sc_json_value *root)
{
    sc_json_value *options = nullptr;
    sc_json_value *include_usage = nullptr;
    sc_status status = sc_json_object_new(alloc, &options);

    if (sc_status_is_ok(status)) {
        status = sc_json_bool_new(alloc, true, &include_usage);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(options, sc_str_from_cstr("include_usage"), include_usage);
        include_usage = nullptr;
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_object_set(root, sc_str_from_cstr("stream_options"), options);
        options = nullptr;
    }
    sc_json_destroy(options);
    sc_json_destroy(include_usage);
    return status;
}

static sc_status parse_usage(sc_json_value *usage, sc_provider_response *out)
{
    double number = 0.0;

    if (usage == nullptr || out == nullptr) {
        return sc_status_ok();
    }
    if (sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("prompt_tokens")), &number) ||
        sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("input_tokens")), &number)) {
        out->input_tokens = (int64_t)number;
    }
    if (sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("completion_tokens")), &number) ||
        sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("output_tokens")), &number)) {
        out->output_tokens = (int64_t)number;
    }
    if (sc_json_as_number(sc_json_object_get(usage, sc_str_from_cstr("total_tokens")), &number)) {
        out->total_tokens = (int64_t)number;
    } else {
        out->total_tokens = out->input_tokens + out->output_tokens;
    }
    return sc_status_ok();
}

static sc_status stream_event_set_usage(sc_allocator *alloc, sc_json_value *usage, sc_provider_stream_event *out)
{
    sc_provider_response response = {0};
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_stream.usage_invalid_argument");
    }
    out->type = SC_PROVIDER_STREAM_FINAL_USAGE;
    sc_provider_response_init(&response, alloc);
    status = parse_usage(usage, &response);
    if (sc_status_is_ok(status)) {
        out->input_tokens = response.input_tokens;
        out->output_tokens = response.output_tokens;
        out->total_tokens = response.total_tokens;
    }
    sc_provider_response_clear(&response);
    return status;
}

static sc_status sse_data_payload(sc_allocator *alloc, sc_str sse_event, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;
    size_t pos = 0;

    sc_string_builder_init(&builder, alloc);
    status = sc_status_ok();
    while (sc_status_is_ok(status) && pos < sse_event.len) {
        size_t start = pos;
        size_t len = 0;
        while (pos < sse_event.len && sse_event.ptr[pos] != '\n') {
            pos += 1;
        }
        len = pos - start;
        if (len > 0 && sse_event.ptr[start + len - 1] == '\r') {
            len -= 1;
        }
        if (len >= 6 && memcmp(sse_event.ptr + start, "data:", 5) == 0) {
            size_t data_start = start + 5;
            if (data_start < start + len && sse_event.ptr[data_start] == ' ') {
                data_start += 1;
            }
            if (builder.bytes.len > 0) {
                status = sc_string_builder_append_cstr(&builder, "\n");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder,
                                                  sc_str_from_parts(sse_event.ptr + data_start, start + len - data_start));
            }
        }
        if (pos < sse_event.len && sse_event.ptr[pos] == '\n') {
            pos += 1;
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    sc_string_builder_clear(&builder);
    return status;
}
