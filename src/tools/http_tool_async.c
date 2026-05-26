#include "tools/http_tool_internal.h"

static void http_tool_async_complete(void *user_data, const sc_http_response *response, sc_status status);
static sc_status http_tool_translate_http_status(const sc_http_response *response, sc_status status);

sc_status http_tool_schedule_async(http_tool_async_state *state,
                                          sc_async_context *context,
                                          sc_str method,
                                          sc_str url,
                                          sc_str headers_json,
                                          sc_str body,
                                          size_t max_bytes,
                                          int64_t timeout_ms)
{
    sc_http_client *client = nullptr;
    sc_status status;

    if (state == nullptr || context == nullptr) {
        return sc_status_invalid_argument("sc.http_tool.async_invalid_argument");
    }

    status = sc_string_from_str(state->alloc, method, &state->method);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(state->alloc, url, &state->url);
    }
    if (sc_status_is_ok(status) && body.len > 0) {
        status = sc_string_from_str(state->alloc, body, &state->body);
    }
    if (sc_status_is_ok(status)) {
        status = http_headers_from_json(state->alloc, headers_json, &state->headers);
    }
    if (sc_status_is_ok(status)) {
        status = sc_async_context_http_client(context, &client);
    }
    if (sc_status_is_ok(status)) {
        const sc_cancel_token *cancel_token = state->tool->base.context.cancel_token;
        sc_http_request http_request = {
            .struct_size = sizeof(http_request),
            .method = sc_string_as_str(&state->method),
            .url = sc_string_as_str(&state->url),
            .headers = state->headers.len == 0 ? nullptr : state->headers.ptr,
            .header_count = state->headers.len,
            .body = sc_string_as_str(&state->body),
            .user_agent = sc_str_from_cstr("smolclaw-c/0.1 tool-http"),
            .max_response_bytes = max_bytes == 0 ? 4096 : max_bytes,
            .timeout_ms = timeout_ms <= 0 ? 30000 : timeout_ms,
            .connect_timeout_ms = timeout_ms <= 0 ? 10000 : timeout_ms,
            .follow_location = false,
            .cancel_token = cancel_token,
        };
        status = sc_http_client_perform(client, &http_request, state->alloc, http_tool_async_complete, state, &state->op);
    }
    return status;
}

static void http_tool_async_complete(void *user_data, const sc_http_response *response, sc_status status)
{
    http_tool_async_state *state = user_data;
    sc_tool_result result = {0};
    sc_status final_status;
    bool success = false;

    if (state == nullptr) {
        sc_status_clear(&status);
        return;
    }

    final_status = http_tool_translate_http_status(response, status);
    if (sc_status_is_ok(final_status) && response != nullptr) {
        final_status = sc_tool_set_output(state->alloc,
                                          &state->tool->base,
                                          &result,
                                          sc_str_from_parts((const char *)response->body.ptr, response->body.len),
                                          true);
    }
    success = sc_status_is_ok(final_status);
    if (success) {
        final_status = sc_tool_record_receipt(&state->tool->base,
                                              sc_string_as_str(&state->tool_name),
                                              sc_string_as_str(&state->args_summary),
                                              sc_string_as_str(&result.output),
                                              true);
        success = sc_status_is_ok(final_status);
    } else {
        (void)sc_tool_record_receipt_status(&state->tool->base,
                                            sc_string_as_str(&state->tool_name),
                                            sc_string_as_str(&state->args_summary),
                                            sc_str_from_cstr("error"),
                                            false,
                                            final_status);
    }

    state->complete(state->complete_user_data, success ? &result : nullptr, final_status);
    sc_tool_result_clear(&result);
    http_tool_async_state_destroy(state);
}

static sc_status http_tool_translate_http_status(const sc_http_response *response, sc_status status)
{
    if (sc_status_is_ok(status)) {
        return status;
    }
    if (status.code == SC_ERR_UNSUPPORTED) {
        sc_status_clear(&status);
        return sc_status_unsupported("sc.http_tool.libcurl_unavailable");
    }
    if (response != nullptr && response->too_large) {
        sc_status_clear(&status);
        return sc_status_http("sc.http_tool.response_too_large");
    }
    if (response != nullptr && response->http_status >= 400) {
        sc_status_clear(&status);
        return sc_status_http("sc.http_tool.http_status");
    }
    return status;
}

void http_tool_async_state_destroy(http_tool_async_state *state)
{
    if (state == nullptr) {
        return;
    }
    sc_http_op_destroy(state->op);
    sc_string_clear(&state->tool_name);
    sc_string_clear(&state->args_summary);
    sc_string_clear(&state->method);
    sc_string_clear(&state->url);
    sc_string_clear(&state->body);
    http_headers_clear(&state->headers);
    sc_free(state->alloc, state, sizeof(*state), _Alignof(http_tool_async_state));
}
