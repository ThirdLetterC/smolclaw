#define _POSIX_C_SOURCE 200809L

#include "sc/observer.h"
#include "sc/bootstrap.h"
#include "sc/provider.h"
#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct fake_http {
    sc_string last_request;
    sc_string last_base_url;
    sc_str response;
    int http_status;
    int calls;
    int fail_status_count;
} fake_http;

typedef struct route_observer {
    int calls;
    sc_string route;
} route_observer;

typedef struct stream_capture {
    int deltas;
    int reasoning_deltas;
    int tool_calls;
    int pre_tool_calls;
    int pre_tool_results;
    int done;
    int final_usage;
    sc_string text;
    sc_string reasoning;
    sc_string tool_name;
    sc_string pre_tool_name;
    sc_string pre_tool_result;
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t total_tokens;
} stream_capture;

typedef struct fake_bedrock_metadata {
    int calls;
    sc_string last_url;
    sc_string last_token;
    sc_str first_response;
    sc_str second_response;
    sc_str third_response;
} fake_bedrock_metadata;

static int test_mock_provider(void);
static int test_openai_compatible(void);
static int test_http_provider_adapters(void);
static int test_streaming_parser(void);
static int test_credentials(void);
static int test_provider_rules_gaps(void);
static int test_thinking_level_parser(void);
static int test_reliable_and_router(void);
static int test_provider_parity(void);
static sc_status fake_send(void *user_data,
                           const sc_provider_options *options,
                           sc_str request_json,
                           sc_allocator *alloc,
                           sc_string *response_json,
                           int *http_status);
static sc_status capture_stream(void *user_data, const sc_provider_stream_event *event);
static sc_status fake_bedrock_metadata_fetch(void *user_data,
                                             sc_str url,
                                             sc_str token,
                                             sc_allocator *alloc,
                                             sc_string *out);
static void stream_capture_clear(stream_capture *capture);
static sc_status route_emit(void *impl, const sc_observer_event *event);
static void route_destroy(void *impl);

static const sc_observer_vtab route_observer_vtab = {
    .struct_size = sizeof(sc_observer_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "route-observer",
    .display_name = "Route observer",
    .feature_flag = "SC_OBSERVER_ROUTE_TEST",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .emit = route_emit,
    .flush = nullptr,
    .destroy = route_destroy,
};

int main(void)
{
    int failures = 0;

    failures += test_mock_provider();
    failures += test_openai_compatible();
    failures += test_http_provider_adapters();
    failures += test_streaming_parser();
    failures += test_credentials();
    failures += test_provider_rules_gaps();
    failures += test_thinking_level_parser();
    failures += test_reliable_and_router();
    failures += test_provider_parity();

    return failures == 0 ? 0 : 1;
}

static int test_mock_provider(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_provider_response response = {0};
    stream_capture stream = {0};
    sc_provider_request request = {
        .struct_size = sizeof(request),
        .model = sc_str_from_cstr("mock-model"),
        .prompt = sc_str_from_cstr("hello"),
    };

    failures += sc_test_expect_status("mock text new",
                              sc_provider_mock_new(sc_allocator_heap(),
                                                   SC_PROVIDER_MOCK_TEXT,
                                                   sc_str_from_cstr("fixed"),
                                                   &provider),
                              SC_OK);
    failures += sc_test_expect_status("mock text generate",
                              sc_provider_generate(provider, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("mock fixed text", strcmp(response.text.ptr, "fixed") == 0);
    failures += sc_test_expect_true("mock no calls", response.tool_calls.len == 0);
    sc_provider_response_clear(&response);
    failures += sc_test_expect_status("mock stream",
                              sc_provider_stream(provider, &request, sc_allocator_heap(), capture_stream, &stream),
                              SC_OK);
    failures += sc_test_expect_true("mock stream deltas", stream.deltas == 2);
    failures += sc_test_expect_true("mock stream text", strcmp(stream.text.ptr, "fixed") == 0);
    failures += sc_test_expect_true("mock stream done", stream.done == 1);
    stream_capture_clear(&stream);
    sc_provider_destroy(provider);

    provider = nullptr;
    failures += sc_test_expect_status("mock multi new",
                              sc_provider_mock_new(sc_allocator_heap(),
                                                   SC_PROVIDER_MOCK_MULTI_TOOL_CALL,
                                                   sc_str_from_cstr("tools"),
                                                   &provider),
                              SC_OK);
    failures += sc_test_expect_status("mock multi generate",
                              sc_provider_generate(provider, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("mock multi calls", response.tool_calls.len == 2);
    sc_provider_response_clear(&response);
    failures += sc_test_expect_status("mock tool stream",
                              sc_provider_stream(provider, &request, sc_allocator_heap(), capture_stream, &stream),
                              SC_OK);
    failures += sc_test_expect_true("mock stream tool calls", stream.tool_calls == 2);
    failures += sc_test_expect_true("mock stream first tool", strcmp(stream.tool_name.ptr, "mock_tool") == 0);
    failures += sc_test_expect_true("mock stream tool done", stream.done == 1);
    stream_capture_clear(&stream);
    sc_provider_destroy(provider);

    provider = nullptr;
    failures += sc_test_expect_status("mock malformed new",
                              sc_provider_mock_new(sc_allocator_heap(),
                                                   SC_PROVIDER_MOCK_MALFORMED_TOOL_CALL,
                                                   sc_str_from_cstr("bad"),
                                                   &provider),
                              SC_OK);
    failures += sc_test_expect_status("mock malformed generate",
                              sc_provider_generate(provider, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("mock malformed flag", response.malformed_tool_call);
    sc_provider_response_clear(&response);
    sc_provider_destroy(provider);

    provider = nullptr;
    failures += sc_test_expect_status("mock error new",
                              sc_provider_mock_new(sc_allocator_heap(),
                                                   SC_PROVIDER_MOCK_ERROR,
                                                   sc_str_from_cstr("error"),
                                                   &provider),
                              SC_OK);
    failures += sc_test_expect_status("mock error generate",
                              sc_provider_generate(provider, &request, sc_allocator_heap(), &response),
                              SC_ERR_HTTP);
    sc_provider_destroy(provider);
    return failures;
}

static int test_openai_compatible(void)
{
    int failures = 0;
    const struct {
        const char *name;
        const char *base_url;
        const char *expected_url;
    } compatible_cases[] = {
        {"Groq", "https://api.groq.com/openai/v1", "https://api.groq.com/openai/v1/chat/completions"},
        {"Mistral", "https://api.mistral.ai", "https://api.mistral.ai/v1/chat/completions"},
        {"xAI", "https://api.x.ai", "https://api.x.ai/v1/chat/completions"},
        {"DeepSeek", "https://api.deepseek.com", "https://api.deepseek.com/v1/chat/completions"},
        {"Moonshot", "https://api.moonshot.cn/v1", "https://api.moonshot.cn/v1/chat/completions"},
        {"Z.AI/GLM", "https://open.bigmodel.cn/api/paas/v4", "https://open.bigmodel.cn/api/paas/v4/chat/completions"},
        {"MiniMax", "https://api.minimax.chat", "https://api.minimax.chat/v1/chat/completions"},
        {"Qianfan", "https://qianfan.baidubce.com/v2", "https://qianfan.baidubce.com/v2/chat/completions"},
        {"Venice", "https://api.venice.ai/api/v1", "https://api.venice.ai/api/v1/chat/completions"},
        {"Vercel AI Gateway", "https://ai-gateway.vercel.sh/v1", "https://ai-gateway.vercel.sh/v1/chat/completions"},
        {"Cloudflare Gateway", "https://gateway.ai.cloudflare.com/v1/account/gateway/chat/completions", "https://gateway.ai.cloudflare.com/v1/account/gateway/chat/completions"},
        {"OpenCode", "https://api.opencode.ai", "https://api.opencode.ai/v1/chat/completions"},
        {"Synthetic", "https://api.synthetic.ai", "https://api.synthetic.ai/v1/chat/completions"},
    };
    sc_provider_options options = {
        .struct_size = sizeof(options),
        .provider_name = sc_str_from_cstr("test-compatible"),
        .base_url = sc_str_from_cstr("https://api.example.test"),
        .api_key = sc_str_from_cstr("secret"),
        .default_model = sc_str_from_cstr("default-model"),
        .timeout_ms = 1000,
        .max_retries = 1,
        .retry_backoff_ms = 0,
        .reasoning_effort = sc_str_from_cstr("low"),
        .format_json = sc_str_from_cstr("{\"type\":\"json_object\"}"),
    };
    sc_provider_request request = {
        .struct_size = sizeof(request),
        .prompt = sc_str_from_cstr("hello"),
        .system_instruction = sc_str_from_cstr("system"),
        .tool_specs_json = sc_str_from_cstr("[{\"type\":\"function\",\"function\":{\"name\":\"localfs__mcp_call\","
                                            "\"description\":\"MCP server proxy\",\"parameters\":{\"type\":\"object\"}}}]"),
    };
    fake_http http = {
        .response = sc_str_from_cstr("{\"choices\":[{\"message\":{\"content\":\"assistant\","
                                     "\"tool_calls\":[{\"id\":\"call-a\",\"type\":\"function\","
                                     "\"function\":{\"name\":\"lookup\",\"arguments\":\"{\\\"q\\\":\\\"zero\\\"}\"}}]}}],"
                                     "\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":13,\"total_tokens\":24}}"),
        .http_status = 200,
    };
    sc_provider *provider = nullptr;
    sc_provider_response response = {0};
    stream_capture stream = {0};
    sc_string request_json = {0};

    for (size_t i = 0; i < sizeof(compatible_cases) / sizeof(compatible_cases[0]); i += 1) {
        sc_provider_options case_options = options;
        fake_http case_http = {
            .response = sc_str_from_cstr("{\"choices\":[{\"message\":{\"content\":\"assistant\","
                                         "\"tool_calls\":[{\"id\":\"call-a\",\"type\":\"function\","
                                         "\"function\":{\"name\":\"lookup\",\"arguments\":\"{\\\"q\\\":\\\"zero\\\"}\"}}]}}],"
                                         "\"usage\":{\"prompt_tokens\":11,\"completion_tokens\":13,\"total_tokens\":24}}"),
            .http_status = 200,
        };

        case_options.provider_name = sc_str_from_cstr(compatible_cases[i].name);
        case_options.base_url = sc_str_from_cstr(compatible_cases[i].base_url);
        failures += sc_test_expect_status(compatible_cases[i].name,
                                  sc_provider_openai_compatible_new(sc_allocator_heap(),
                                                                    &case_options,
                                                                    fake_send,
                                                                    &case_http,
                                                                    &provider),
                                  SC_OK);
        failures += sc_test_expect_status(compatible_cases[i].name,
                                  sc_provider_generate(provider, &request, sc_allocator_heap(), &response),
                                  SC_OK);
        failures += sc_test_expect_true(compatible_cases[i].name, strstr(case_http.last_request.ptr, "\"messages\"") != nullptr);
        failures += sc_test_expect_true(compatible_cases[i].name,
                                strcmp(case_http.last_base_url.ptr, compatible_cases[i].expected_url) == 0);
        failures += sc_test_expect_true(compatible_cases[i].name, strcmp(response.text.ptr, "assistant") == 0);
        failures += sc_test_expect_true(compatible_cases[i].name, response.tool_calls.len == 1);
        failures += sc_test_expect_true(compatible_cases[i].name,
                                response.input_tokens == 11 && response.output_tokens == 13 &&
                                    response.total_tokens == 24);
        sc_provider_response_clear(&response);
        sc_provider_destroy(provider);
        provider = nullptr;
        sc_string_clear(&case_http.last_request);
        sc_string_clear(&case_http.last_base_url);

        case_http.response = sc_str_from_cstr("data: {\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n\n"
                                              "data: {\"choices\":[{\"delta\":{\"reasoning\":\"thinking\"}}]}\n\n"
                                              "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
                                              "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":3,\"total_tokens\":5}}\n\n"
                                              "data: [DONE]\n\n");
        failures += sc_test_expect_status(compatible_cases[i].name,
                                  sc_provider_openai_compatible_new(sc_allocator_heap(),
                                                                    &case_options,
                                                                    fake_send,
                                                                    &case_http,
                                                                    &provider),
                                  SC_OK);
        failures += sc_test_expect_status(compatible_cases[i].name,
                                  sc_provider_stream(provider, &request, sc_allocator_heap(), capture_stream, &stream),
                                  SC_OK);
        failures += sc_test_expect_true(compatible_cases[i].name, strstr(case_http.last_request.ptr, "\"stream\":true") != nullptr);
        failures += sc_test_expect_true(compatible_cases[i].name, stream.deltas == 2 && strcmp(stream.text.ptr, "hello") == 0);
        failures += sc_test_expect_true(compatible_cases[i].name,
                                stream.reasoning_deltas == 1 && strcmp(stream.reasoning.ptr, "thinking") == 0);
        failures += sc_test_expect_true(compatible_cases[i].name,
                                stream.done == 1 && stream.final_usage == 1 && stream.total_tokens == 5);
        stream_capture_clear(&stream);
        sc_provider_destroy(provider);
        provider = nullptr;
        sc_string_clear(&case_http.last_request);
        sc_string_clear(&case_http.last_base_url);
    }

    failures += sc_test_expect_status("request json",
                              sc_provider_openai_build_request(sc_allocator_heap(), &options, &request, &request_json),
                              SC_OK);
    failures += sc_test_expect_true("request has default model", strstr(request_json.ptr, "\"default-model\"") != nullptr);
    failures += sc_test_expect_true("request has messages", strstr(request_json.ptr, "\"messages\"") != nullptr);
    failures += sc_test_expect_true("request has system", strstr(request_json.ptr, "\"system\"") != nullptr);
    failures += sc_test_expect_true("request has reasoning effort", strstr(request_json.ptr, "\"reasoning_effort\":\"low\"") != nullptr);
    failures += sc_test_expect_true("request has response format", strstr(request_json.ptr, "\"response_format\"") != nullptr &&
                                                   strstr(request_json.ptr, "\"json_object\"") != nullptr);
    failures += sc_test_expect_true("request has tools", strstr(request_json.ptr, "\"tools\"") != nullptr &&
                                                 strstr(request_json.ptr, "\"localfs__mcp_call\"") != nullptr);
    sc_string_clear(&request_json);

    failures += sc_test_expect_status("compatible new",
                              sc_provider_openai_compatible_new(sc_allocator_heap(), &options, fake_send, &http, &provider),
                              SC_OK);
    failures += sc_test_expect_status("compatible generate",
                              sc_provider_generate(provider, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("compatible request captured", strstr(http.last_request.ptr, "\"hello\"") != nullptr);
    failures += sc_test_expect_true("compatible normalized root url",
                            strcmp(http.last_base_url.ptr, "https://api.example.test/v1/chat/completions") == 0);
    failures += sc_test_expect_true("compatible text", strcmp(response.text.ptr, "assistant") == 0);
    failures += sc_test_expect_true("compatible usage", response.input_tokens == 11 && response.output_tokens == 13 &&
                                                    response.total_tokens == 24);
    failures += sc_test_expect_true("compatible call count", response.tool_calls.len == 1);
    if (response.tool_calls.len == 1) {
        const sc_provider_tool_call *call = sc_vec_at_const(&response.tool_calls, 0);
        sc_tool_call tool_call = {0};
        failures += sc_test_expect_true("compatible tool name", call != nullptr && strcmp(call->name.ptr, "lookup") == 0);
        failures += sc_test_expect_true("compatible tool args parsed", call != nullptr && call->arguments != nullptr);
        failures += sc_test_expect_status("compatible maps sc_tool_call",
                                  sc_provider_tool_call_as_tool_call(call, &tool_call),
                                  SC_OK);
        failures += sc_test_expect_true("tool call borrows args", tool_call.args == call->arguments);
    }

    sc_provider_response_clear(&response);
    sc_provider_destroy(provider);
    provider = nullptr;
    sc_string_clear(&http.last_request);
    sc_string_clear(&http.last_base_url);

    http.response = sc_str_from_cstr("data: {\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n\n"
                                     "data: {\"choices\":[{\"delta\":{\"reasoning_content\":\"thinking\"}}]}\n\n"
                                     "data: {\"choices\":[{\"delta\":{\"content\":\"lo\"}}]}\n\n"
                                     "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"tc\","
                                     "\"function\":{\"name\":\"lookup\",\"arguments\":\"{}\"}}]}}]}\n\n"
                                     "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":2,\"completion_tokens\":3,\"total_tokens\":5}}\n\n"
                                     "data: [DONE]\n\n");
    http.http_status = 200;
    http.calls = 0;
    failures += sc_test_expect_status("compatible stream new",
                              sc_provider_openai_compatible_new(sc_allocator_heap(), &options, fake_send, &http, &provider),
                              SC_OK);
    failures += sc_test_expect_status("compatible stream",
                              sc_provider_stream(provider, &request, sc_allocator_heap(), capture_stream, &stream),
                              SC_OK);
    failures += sc_test_expect_true("compatible stream request", strstr(http.last_request.ptr, "\"stream\":true") != nullptr);
    failures += sc_test_expect_true("compatible stream usage requested", strstr(http.last_request.ptr, "\"stream_options\"") != nullptr &&
                                                              strstr(http.last_request.ptr, "\"include_usage\":true") != nullptr);
    failures += sc_test_expect_true("compatible stream deltas", stream.deltas == 2 && strcmp(stream.text.ptr, "hello") == 0);
    failures += sc_test_expect_true("compatible reasoning delta", stream.reasoning_deltas == 1 &&
                                                       strcmp(stream.reasoning.ptr, "thinking") == 0);
    failures += sc_test_expect_true("compatible stream tool", stream.tool_calls == 1 && strcmp(stream.tool_name.ptr, "lookup") == 0);
    failures += sc_test_expect_true("compatible stream done", stream.done == 1 && stream.final_usage == 1);
    failures += sc_test_expect_true("compatible stream usage", stream.input_tokens == 2 && stream.output_tokens == 3 &&
                                                       stream.total_tokens == 5);
    stream_capture_clear(&stream);
    sc_provider_destroy(provider);
    provider = nullptr;
    sc_string_clear(&http.last_request);
    sc_string_clear(&http.last_base_url);

    http.fail_status_count = 1;
    http.response = sc_str_from_cstr("{\"choices\":[{\"message\":{\"content\":\"assistant\","
                                     "\"tool_calls\":[{\"id\":\"call-a\",\"type\":\"function\","
                                     "\"function\":{\"name\":\"lookup\",\"arguments\":\"{\\\"q\\\":\\\"zero\\\"}\"}}]}}]}");
    http.http_status = 200;
    http.calls = 0;
    failures += sc_test_expect_status("compatible retry new",
                              sc_provider_openai_compatible_new(sc_allocator_heap(), &options, fake_send, &http, &provider),
                              SC_OK);
    failures += sc_test_expect_status("compatible retry generate",
                              sc_provider_generate(provider, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("compatible retried", http.calls == 2);
    failures += sc_test_expect_true("compatible retry text", strcmp(response.text.ptr, "assistant") == 0);
    sc_provider_response_clear(&response);
    sc_provider_destroy(provider);
    sc_string_clear(&http.last_request);
    sc_string_clear(&http.last_base_url);
    provider = nullptr;

    http.response = sc_str_from_cstr("{\"message\":\"model registry is disabled on this endpoint\"}");
    http.http_status = 400;
    http.calls = 0;
    failures += sc_test_expect_status("compatible diagnostic new",
                              sc_provider_openai_compatible_new(sc_allocator_heap(), &options, fake_send, &http, &provider),
                              SC_OK);
    sc_status diagnostic = sc_provider_generate(provider, &request, sc_allocator_heap(), &response);
    failures += sc_test_expect_true("compatible nonstandard error key",
                            diagnostic.code == SC_ERR_HTTP &&
                                diagnostic.error_key != nullptr &&
                                strcmp(diagnostic.error_key, "sc.provider_compatible.nonstandard_error_response") == 0);
    failures += sc_test_expect_true("compatible nonstandard error message",
                            diagnostic.message != nullptr &&
                                strstr(diagnostic.message, "model registry is disabled") != nullptr);
    sc_status_clear(&diagnostic);
    sc_provider_response_clear(&response);
    sc_provider_destroy(provider);
    sc_string_clear(&http.last_request);
    sc_string_clear(&http.last_base_url);
    provider = nullptr;

    http.response = sc_str_from_cstr("<html><body>not found</body></html>");
    http.http_status = 404;
    http.calls = 0;
    failures += sc_test_expect_status("compatible 404 diagnostic new",
                              sc_provider_openai_compatible_new(sc_allocator_heap(), &options, fake_send, &http, &provider),
                              SC_OK);
    diagnostic = sc_provider_generate(provider, &request, sc_allocator_heap(), &response);
    failures += sc_test_expect_true("compatible 404 error key",
                            diagnostic.code == SC_ERR_HTTP &&
                                diagnostic.error_key != nullptr &&
                                strcmp(diagnostic.error_key, "sc.provider_compatible.endpoint_not_found") == 0);
    failures += sc_test_expect_true("compatible 404 mentions models",
                            diagnostic.message != nullptr && strstr(diagnostic.message, "/models") != nullptr);
    sc_status_clear(&diagnostic);
    sc_provider_response_clear(&response);
    sc_provider_destroy(provider);
    sc_string_clear(&http.last_request);
    sc_string_clear(&http.last_base_url);
    return failures;
}

static sc_status capture_stream(void *user_data, const sc_provider_stream_event *event)
{
    stream_capture *capture = user_data;
    sc_status status = sc_status_ok();

    if (capture == nullptr || event == nullptr) {
        return sc_status_invalid_argument("sc.test.stream_capture.invalid_argument");
    }
    if (event->type == SC_PROVIDER_STREAM_DELTA) {
        sc_string_builder builder = {0};
        sc_string combined = {0};
        sc_string_builder_init(&builder, sc_allocator_heap());
        if (capture->text.len > 0) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&capture->text));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&event->text));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &combined);
        } else {
            sc_string_builder_clear(&builder);
        }
        if (sc_status_is_ok(status)) {
            sc_string_clear(&capture->text);
            capture->text = combined;
        } else {
            sc_string_clear(&combined);
        }
        capture->deltas += 1;
    } else if (event->type == SC_PROVIDER_STREAM_REASONING_DELTA) {
        sc_string_builder builder = {0};
        sc_string combined = {0};
        sc_string_builder_init(&builder, sc_allocator_heap());
        if (capture->reasoning.len > 0) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&capture->reasoning));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&event->text));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &combined);
        } else {
            sc_string_builder_clear(&builder);
        }
        if (sc_status_is_ok(status)) {
            sc_string_clear(&capture->reasoning);
            capture->reasoning = combined;
        } else {
            sc_string_clear(&combined);
        }
        capture->reasoning_deltas += 1;
    } else if (event->type == SC_PROVIDER_STREAM_TOOL_CALL) {
        capture->tool_calls += 1;
        if (capture->tool_name.len == 0) {
            status = sc_string_from_str(sc_allocator_heap(), sc_string_as_str(&event->tool_call.name), &capture->tool_name);
        }
    } else if (event->type == SC_PROVIDER_STREAM_PRE_EXECUTED_TOOL_CALL) {
        capture->pre_tool_calls += 1;
        if (capture->pre_tool_name.len == 0) {
            status = sc_string_from_str(sc_allocator_heap(), sc_string_as_str(&event->tool_call.name), &capture->pre_tool_name);
        }
    } else if (event->type == SC_PROVIDER_STREAM_PRE_EXECUTED_TOOL_RESULT) {
        capture->pre_tool_results += 1;
        if (capture->pre_tool_result.len == 0) {
            status = sc_string_from_str(sc_allocator_heap(), sc_string_as_str(&event->text), &capture->pre_tool_result);
        }
    } else if (event->type == SC_PROVIDER_STREAM_FINAL_USAGE) {
        capture->final_usage += 1;
        if (event->input_tokens != 0 || event->output_tokens != 0 || event->total_tokens != 0) {
            capture->input_tokens = event->input_tokens;
            capture->output_tokens = event->output_tokens;
            capture->total_tokens = event->total_tokens;
        }
    } else if (event->type == SC_PROVIDER_STREAM_DONE) {
        capture->done += 1;
    }
    return status;
}

static void stream_capture_clear(stream_capture *capture)
{
    if (capture == nullptr) {
        return;
    }
    sc_string_clear(&capture->text);
    sc_string_clear(&capture->reasoning);
    sc_string_clear(&capture->tool_name);
    sc_string_clear(&capture->pre_tool_name);
    sc_string_clear(&capture->pre_tool_result);
    *capture = (stream_capture){0};
}

static int test_http_provider_adapters(void)
{
    int failures = 0;
    sc_provider_options anthropic_options = {
        .struct_size = sizeof(anthropic_options),
        .api_key = sc_str_from_cstr("anthropic-secret"),
        .default_model = sc_str_from_cstr("claude-test"),
        .timeout_ms = 1000,
    };
    sc_provider_options gemini_options = {
        .struct_size = sizeof(gemini_options),
        .api_key = sc_str_from_cstr("gemini-secret"),
        .default_model = sc_str_from_cstr("gemini-test"),
        .timeout_ms = 1000,
    };
    sc_provider_options ollama_options = {
        .struct_size = sizeof(ollama_options),
        .default_model = sc_str_from_cstr("llama3.2"),
        .timeout_ms = 1000,
        .temperature = 0.2,
        .max_output_tokens = 128,
    };
    sc_provider_request request = {
        .struct_size = sizeof(request),
        .prompt = sc_str_from_cstr("hello"),
        .tool_specs_json = sc_str_from_cstr("[{\"type\":\"function\",\"function\":{\"name\":\"localfs__mcp_call\","
                                            "\"description\":\"MCP server proxy\",\"parameters\":{\"type\":\"object\"}}}]"),
    };
    sc_string request_json = {0};
    sc_provider_response response = {0};
    sc_provider_stream_event event = {0};
    stream_capture stream = {0};
    sc_provider *provider = nullptr;

    failures += sc_test_expect_status("anthropic build",
                              sc_provider_anthropic_build_request(sc_allocator_heap(), &anthropic_options, &request, &request_json),
                              SC_OK);
    failures += sc_test_expect_true("anthropic model", strstr(request_json.ptr, "\"claude-test\"") != nullptr);
    failures += sc_test_expect_true("anthropic max tokens", strstr(request_json.ptr, "\"max_tokens\":1024") != nullptr);
    failures += sc_test_expect_true("anthropic prompt", strstr(request_json.ptr, "\"hello\"") != nullptr);
    failures += sc_test_expect_true("anthropic tools", strstr(request_json.ptr, "\"tools\"") != nullptr &&
                                                strstr(request_json.ptr, "\"localfs__mcp_call\"") != nullptr &&
                                                strstr(request_json.ptr, "\"input_schema\"") != nullptr);
    sc_string_clear(&request_json);

    anthropic_options.streaming = true;
    failures += sc_test_expect_status("anthropic stream build",
                              sc_provider_anthropic_build_request(sc_allocator_heap(), &anthropic_options, &request, &request_json),
                              SC_OK);
    failures += sc_test_expect_true("anthropic stream flag", strstr(request_json.ptr, "\"stream\":true") != nullptr);
    sc_string_clear(&request_json);
    anthropic_options.streaming = false;

    failures += sc_test_expect_status("anthropic sse delta",
                              sc_provider_anthropic_parse_sse_event(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("event: content_block_delta\n"
                                                   "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"text_delta\",\"text\":\"hel\"}}\n\n"),
                                  &event),
                              SC_OK);
    failures += sc_test_expect_true("anthropic sse delta type", event.type == SC_PROVIDER_STREAM_DELTA);
    failures += sc_test_expect_true("anthropic sse delta text", strcmp(event.text.ptr, "hel") == 0);
    sc_provider_stream_event_clear(&event);

    failures += sc_test_expect_status("anthropic parse",
                              sc_provider_anthropic_parse_response(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("{\"id\":\"msg\",\"content\":[{\"type\":\"text\",\"text\":\"hello \"},"
                                                   "{\"type\":\"text\",\"text\":\"claude\"}],"
                                                   "\"usage\":{\"input_tokens\":7,\"output_tokens\":9}}"),
                                  &response),
                              SC_OK);
    failures += sc_test_expect_true("anthropic text", strcmp(response.text.ptr, "hello claude") == 0);
    failures += sc_test_expect_true("anthropic usage", response.input_tokens == 7 && response.output_tokens == 9 &&
                                                   response.total_tokens == 16);
    sc_provider_response_clear(&response);

    failures += sc_test_expect_status("anthropic sse reasoning",
                              sc_provider_anthropic_parse_sse_event(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("event: content_block_delta\n"
                                                   "data: {\"type\":\"content_block_delta\",\"delta\":{\"type\":\"thinking_delta\",\"thinking\":\"trace\"}}\n\n"),
                                  &event),
                              SC_OK);
    failures += sc_test_expect_true("anthropic sse reasoning type", event.type == SC_PROVIDER_STREAM_REASONING_DELTA);
    failures += sc_test_expect_true("anthropic sse reasoning text", strcmp(event.text.ptr, "trace") == 0);
    sc_provider_stream_event_clear(&event);

    failures += sc_test_expect_status("anthropic sse usage",
                              sc_provider_anthropic_parse_sse_event(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("event: message_delta\n"
                                                   "data: {\"type\":\"message_delta\",\"usage\":{\"input_tokens\":5,\"output_tokens\":6}}\n\n"),
                                  &event),
                              SC_OK);
    failures += sc_test_expect_true("anthropic sse usage type", event.type == SC_PROVIDER_STREAM_FINAL_USAGE);
    failures += sc_test_expect_true("anthropic sse usage tokens", event.input_tokens == 5 && event.output_tokens == 6 &&
                                                        event.total_tokens == 11);
    sc_provider_stream_event_clear(&event);

    failures += sc_test_expect_status("gemini build",
                              sc_provider_gemini_build_request(sc_allocator_heap(), &gemini_options, &request, &request_json),
                              SC_OK);
    failures += sc_test_expect_true("gemini contents", strstr(request_json.ptr, "\"contents\"") != nullptr);
    failures += sc_test_expect_true("gemini prompt", strstr(request_json.ptr, "\"hello\"") != nullptr);
    failures += sc_test_expect_true("gemini tools", strstr(request_json.ptr, "\"functionDeclarations\"") != nullptr &&
                                             strstr(request_json.ptr, "\"localfs__mcp_call\"") != nullptr);
    sc_string_clear(&request_json);

    failures += sc_test_expect_status("gemini sse delta",
                              sc_provider_gemini_parse_sse_event(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"gem\"}]}}]}\n\n"),
                                  &event),
                              SC_OK);
    failures += sc_test_expect_true("gemini sse delta type", event.type == SC_PROVIDER_STREAM_DELTA);
    failures += sc_test_expect_true("gemini sse delta text", strcmp(event.text.ptr, "gem") == 0);
    sc_provider_stream_event_clear(&event);

    failures += sc_test_expect_status("gemini parse",
                              sc_provider_gemini_parse_response(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"hello \"},"
                                                   "{\"text\":\"gemini\"}]}}]}"),
                                  &response),
                              SC_OK);
    failures += sc_test_expect_true("gemini text", strcmp(response.text.ptr, "hello gemini") == 0);
    sc_provider_response_clear(&response);

    failures += sc_test_expect_status("gemini grounded parse",
                              sc_provider_gemini_parse_response(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"grounded\"}]},"
                                                   "\"groundingMetadata\":{\"webSearchQueries\":[\"smol claw\"],"
                                                   "\"groundingChunks\":[{\"web\":{\"uri\":\"https://example.test\",\"title\":\"Example\"}}]}}]}"),
                                  &response),
                              SC_OK);
    failures += sc_test_expect_true("gemini grounded tool name",
                            response.pre_executed_tool_call.name.ptr != nullptr &&
                                strcmp(response.pre_executed_tool_call.name.ptr, "google_search") == 0);
    failures += sc_test_expect_true("gemini grounded args",
                            response.pre_executed_tool_call.arguments_json.ptr != nullptr &&
                                strstr(response.pre_executed_tool_call.arguments_json.ptr, "smol claw") != nullptr);
    failures += sc_test_expect_true("gemini grounded result",
                            response.pre_executed_tool_result.ptr != nullptr &&
                                strstr(response.pre_executed_tool_result.ptr, "example.test") != nullptr);
    sc_provider_response_clear(&response);

    failures += sc_test_expect_status("gemini grounded sse",
                              sc_provider_gemini_parse_sse_event(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("data: {\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"grounded\"}]},"
                                                   "\"groundingMetadata\":{\"webSearchQueries\":[\"smol claw\"],"
                                                   "\"groundingChunks\":[{\"web\":{\"uri\":\"https://example.test\"}}]}}]}\n\n"),
                                  &event),
                              SC_OK);
    failures += sc_test_expect_true("gemini grounded sse type", event.type == SC_PROVIDER_STREAM_PRE_EXECUTED_TOOL_CALL);
    failures += sc_test_expect_true("gemini grounded sse name", event.tool_call.name.ptr != nullptr &&
                                                        strcmp(event.tool_call.name.ptr, "google_search") == 0);
    sc_provider_stream_event_clear(&event);

    failures += sc_test_expect_status("gemini parse escaped unicode",
                              sc_provider_gemini_parse_response(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("{\"candidates\":[{\"content\":{\"parts\":[{\"text\":\"Use \\u003cb\\u003etags\\u003c/b\\u003e \\ud83d\\ude80\"}]}}]}"),
                                  &response),
                              SC_OK);
    failures += sc_test_expect_true("gemini escaped unicode text",
                            strcmp(response.text.ptr, "Use <b>tags</b> " "\xF0\x9F\x9A\x80") == 0);
    sc_provider_response_clear(&response);

    failures += sc_test_expect_status("ollama build",
                              sc_provider_ollama_build_request(sc_allocator_heap(), &ollama_options, &request, &request_json),
                              SC_OK);
    failures += sc_test_expect_true("ollama model", strstr(request_json.ptr, "\"model\":\"llama3.2\"") != nullptr);
    failures += sc_test_expect_true("ollama messages", strstr(request_json.ptr, "\"messages\"") != nullptr);
    failures += sc_test_expect_true("ollama stream false", strstr(request_json.ptr, "\"stream\":false") != nullptr);
    failures += sc_test_expect_true("ollama options", strstr(request_json.ptr, "\"num_predict\":128") != nullptr);
    failures += sc_test_expect_true("ollama tools", strstr(request_json.ptr, "\"tools\"") != nullptr &&
                                             strstr(request_json.ptr, "\"localfs__mcp_call\"") != nullptr);
    sc_string_clear(&request_json);

    failures += sc_test_expect_status("ollama reasoning controls build",
                              sc_provider_ollama_build_request(sc_allocator_heap(),
                                                               &(sc_provider_options){
                                                                   .struct_size = sizeof(sc_provider_options),
                                                                   .default_model = sc_str_from_cstr("qwen3"),
                                                                   .think = false,
                                                                   .think_set = true,
                                                                   .reasoning_effort = sc_str_from_cstr("none"),
                                                                   .options_json = sc_str_from_cstr("{\"num_ctx\":2048}"),
                                                                   .format_json = sc_str_from_cstr("{\"type\":\"object\"}"),
                                                               },
                                                               &request,
                                                               &request_json),
                              SC_OK);
    failures += sc_test_expect_true("ollama think false", strstr(request_json.ptr, "\"think\":false") != nullptr);
    failures += sc_test_expect_true("ollama reasoning none", strstr(request_json.ptr, "\"reasoning_effort\":\"none\"") != nullptr);
    failures += sc_test_expect_true("ollama options json", strstr(request_json.ptr, "\"num_ctx\":2048") != nullptr);
    failures += sc_test_expect_true("ollama format json", strstr(request_json.ptr, "\"format\":{\"type\":\"object\"}") != nullptr);
    sc_string_clear(&request_json);

    ollama_options.streaming = true;
    failures += sc_test_expect_status("ollama stream build",
                              sc_provider_ollama_build_request(sc_allocator_heap(), &ollama_options, &request, &request_json),
                              SC_OK);
    failures += sc_test_expect_true("ollama stream true", strstr(request_json.ptr, "\"stream\":true") != nullptr);
    sc_string_clear(&request_json);
    ollama_options.streaming = false;

    failures += sc_test_expect_status("ollama parse",
                              sc_provider_ollama_parse_response(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("{\"model\":\"llama3.2\",\"message\":{\"role\":\"assistant\","
                                                   "\"content\":\"hello local\",\"tool_calls\":[{\"function\":"
                                                   "{\"name\":\"mock_tool\",\"arguments\":{\"value\":\"x\"}}}]},"
                                                   "\"thinking\":\"reasoned\","
                                                   "\"done\":true,\"done_reason\":\"stop\","
                                                   "\"prompt_eval_count\":3,\"eval_count\":4}"),
                                  &response),
                              SC_OK);
    failures += sc_test_expect_true("ollama text", strcmp(response.text.ptr, "hello local") == 0);
    failures += sc_test_expect_true("ollama reasoning parsed", response.reasoning_text.ptr != nullptr &&
                                                        strcmp(response.reasoning_text.ptr, "reasoned") == 0);
    failures += sc_test_expect_true("ollama model parsed", strcmp(response.model.ptr, "llama3.2") == 0);
    failures += sc_test_expect_true("ollama usage parsed", response.input_tokens == 3 && response.output_tokens == 4 && response.total_tokens == 7);
    failures += sc_test_expect_true("ollama tool call parsed", response.tool_calls.len == 1);
    sc_provider_response_clear(&response);

    failures += sc_test_expect_status("ollama parse stream",
                              sc_provider_ollama_parse_stream(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("{\"message\":{\"content\":\"hel\"},\"done\":false}\n"
                                                   "{\"message\":{\"content\":\"lo\"},\"done\":true,\"done_reason\":\"stop\"}\n"),
                                  capture_stream,
                                  &stream),
                              SC_OK);
    failures += sc_test_expect_true("ollama stream deltas", stream.deltas == 2 && strcmp(stream.text.ptr, "hello") == 0);
    failures += sc_test_expect_true("ollama stream done", stream.done == 1);
    failures += sc_test_expect_true("ollama stream final usage", stream.final_usage == 1);
    stream_capture_clear(&stream);

    failures += sc_test_expect_status("bedrock build",
                              sc_provider_bedrock_build_request(sc_allocator_heap(),
                                                                &(sc_provider_options){
                                                                    .struct_size = sizeof(sc_provider_options),
                                                                    .default_model = sc_str_from_cstr("anthropic.claude-test"),
                                                                    .max_output_tokens = 256,
                                                                    .temperature = 0.3,
                                                                },
                                                                &(sc_provider_request){
                                                                    .struct_size = sizeof(sc_provider_request),
                                                                    .prompt = sc_str_from_cstr("hello"),
                                                                    .system_instruction = sc_str_from_cstr("system"),
                                                                },
                                                                &request_json),
                              SC_OK);
    failures += sc_test_expect_true("bedrock messages", strstr(request_json.ptr, "\"messages\"") != nullptr);
    failures += sc_test_expect_true("bedrock system", strstr(request_json.ptr, "\"system\"") != nullptr);
    failures += sc_test_expect_true("bedrock inference", strstr(request_json.ptr, "\"inferenceConfig\"") != nullptr &&
                                                strstr(request_json.ptr, "\"maxTokens\":256") != nullptr);
    sc_string_clear(&request_json);

    failures += sc_test_expect_status("bedrock parse",
                              sc_provider_bedrock_parse_response(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("{\"output\":{\"message\":{\"content\":[{\"text\":\"hello \"},"
                                                   "{\"text\":\"bedrock\"}]}},\"stopReason\":\"end_turn\","
                                                   "\"usage\":{\"inputTokens\":4,\"outputTokens\":5}}"),
                                  &response),
                              SC_OK);
    failures += sc_test_expect_true("bedrock text", strcmp(response.text.ptr, "hello bedrock") == 0);
    failures += sc_test_expect_true("bedrock usage", response.input_tokens == 4 && response.output_tokens == 5 &&
                                                response.total_tokens == 9);
    sc_provider_response_clear(&response);

    failures += sc_test_expect_status("anthropic provider new",
                              sc_provider_anthropic_new(sc_allocator_heap(), &anthropic_options, &provider),
                              SC_OK);
    failures += sc_test_expect_true("anthropic vtab", strcmp(sc_provider_vtab_of(provider)->name, "anthropic") == 0);
    failures += sc_test_expect_true("anthropic stream vtab", sc_provider_vtab_of(provider)->stream != nullptr);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_status("gemini provider new",
                              sc_provider_gemini_new(sc_allocator_heap(), &gemini_options, &provider),
                              SC_OK);
    failures += sc_test_expect_true("gemini vtab", strcmp(sc_provider_vtab_of(provider)->name, "gemini") == 0);
    failures += sc_test_expect_true("gemini stream vtab", sc_provider_vtab_of(provider)->stream != nullptr);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_status("ollama provider new",
                              sc_provider_ollama_new(sc_allocator_heap(), &ollama_options, &provider),
                              SC_OK);
    failures += sc_test_expect_true("ollama vtab", strcmp(sc_provider_vtab_of(provider)->name, "ollama") == 0);
    failures += sc_test_expect_true("ollama stream vtab", sc_provider_vtab_of(provider)->stream != nullptr);
    sc_provider_destroy(provider);

#ifdef SC_HAVE_LIBCURL
    provider = nullptr;
    failures += sc_test_expect_status("openai provider new",
                              sc_provider_openai_new(sc_allocator_heap(),
                                                     &(sc_provider_options){
                                                         .struct_size = sizeof(sc_provider_options),
                                                         .api_key = sc_str_from_cstr("openai-secret"),
                                                         .default_model = sc_str_from_cstr("gpt-test"),
                                                     },
                                                     &provider),
                              SC_OK);
    failures += sc_test_expect_true("openai compatible vtab", strcmp(sc_provider_vtab_of(provider)->name, "openai-compatible") == 0);
    sc_provider_destroy(provider);
#else
    failures += sc_test_expect_status("openai provider unsupported without curl",
                              sc_provider_openai_new(sc_allocator_heap(), &anthropic_options, &provider),
                              SC_ERR_UNSUPPORTED);
#endif

    return failures;
}

static int test_streaming_parser(void)
{
    int failures = 0;
    sc_provider_stream_event event = {0};

    failures += sc_test_expect_status("sse delta",
                              sc_provider_openai_parse_sse_event(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("event: message\ndata: {\"choices\":[{\"delta\":{\"content\":\"hel\"}}]}\n\n"),
                                  &event),
                              SC_OK);
    failures += sc_test_expect_true("sse delta type", event.type == SC_PROVIDER_STREAM_DELTA);
    failures += sc_test_expect_true("sse delta text", strcmp(event.text.ptr, "hel") == 0);
    sc_provider_stream_event_clear(&event);

    failures += sc_test_expect_status("sse tool",
                              sc_provider_openai_parse_sse_event(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"tc\","
                                                   "\"function\":{\"name\":\"lookup\",\"arguments\":\"{}\"}}]}}]}\n\n"),
                                  &event),
                              SC_OK);
    failures += sc_test_expect_true("sse tool type", event.type == SC_PROVIDER_STREAM_TOOL_CALL);
    failures += sc_test_expect_true("sse tool name", strcmp(event.tool_call.name.ptr, "lookup") == 0);
    sc_provider_stream_event_clear(&event);

    failures += sc_test_expect_status("sse partial tool args",
                              sc_provider_openai_parse_sse_event(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"id\":\"tc\","
                                                   "\"function\":{\"name\":\"lookup\",\"arguments\":\"{\\\"q\\\"\"}}]}}]}\n\n"),
                                  &event),
                              SC_OK);
    failures += sc_test_expect_true("sse partial tool type", event.type == SC_PROVIDER_STREAM_TOOL_CALL);
    failures += sc_test_expect_true("sse partial tool args retained", event.tool_call.arguments_json.ptr != nullptr &&
                                                           strcmp(event.tool_call.arguments_json.ptr, "{\"q\"") == 0);
    failures += sc_test_expect_true("sse partial tool args unparsed", event.tool_call.arguments == nullptr);
    sc_provider_stream_event_clear(&event);

    failures += sc_test_expect_status("sse reasoning",
                              sc_provider_openai_parse_sse_event(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("data: {\"choices\":[{\"delta\":{\"reasoning_delta\":\"think\"}}]}\n\n"),
                                  &event),
                              SC_OK);
    failures += sc_test_expect_true("sse reasoning type", event.type == SC_PROVIDER_STREAM_REASONING_DELTA);
    failures += sc_test_expect_true("sse reasoning text", strcmp(event.text.ptr, "think") == 0);
    sc_provider_stream_event_clear(&event);

    failures += sc_test_expect_status("sse final usage",
                              sc_provider_openai_parse_sse_event(
                                  sc_allocator_heap(),
                                  sc_str_from_cstr("data: {\"choices\":[],\"usage\":{\"prompt_tokens\":8,\"completion_tokens\":9,\"total_tokens\":17}}\n\n"),
                                  &event),
                              SC_OK);
    failures += sc_test_expect_true("sse final usage type", event.type == SC_PROVIDER_STREAM_FINAL_USAGE);
    failures += sc_test_expect_true("sse final usage tokens", event.input_tokens == 8 && event.output_tokens == 9 &&
                                                     event.total_tokens == 17);
    sc_provider_stream_event_clear(&event);

    failures += sc_test_expect_status("sse done",
                              sc_provider_openai_parse_sse_event(sc_allocator_heap(),
                                                                 sc_str_from_cstr("data: [DONE]\n\n"),
                                                                 &event),
                              SC_OK);
    failures += sc_test_expect_true("sse done type", event.type == SC_PROVIDER_STREAM_DONE);
    sc_provider_stream_event_clear(&event);
    return failures;
}

static int test_credentials(void)
{
    int failures = 0;
    sc_provider_options options = {
        .struct_size = sizeof(options),
        .credential_env = sc_str_from_cstr("SC_PROVIDERS_TEST_KEY"),
    };
    sc_provider_options missing = {
        .struct_size = sizeof(missing),
        .credential_env = sc_str_from_cstr("SC_PROVIDERS_MISSING_KEY"),
    };
    sc_provider_options generic = {
        .struct_size = sizeof(generic),
        .credential_env = sc_str_from_cstr("SC_PROVIDERS_PROVIDER_MISSING_KEY"),
        .generic_credential_env = sc_str_from_cstr("SC_PROVIDERS_GENERIC_KEY"),
    };
    sc_provider_options inline_key = {
        .struct_size = sizeof(inline_key),
        .api_key = sc_str_from_cstr("inline-secret"),
        .secret_value = sc_str_from_cstr("stored-secret"),
        .credential_env = sc_str_from_cstr("SC_PROVIDERS_TEST_KEY"),
        .generic_credential_env = sc_str_from_cstr("SC_PROVIDERS_GENERIC_KEY"),
    };
    sc_provider_options secret_store = {
        .struct_size = sizeof(secret_store),
        .secret_value = sc_str_from_cstr("stored-secret"),
        .credential_env = sc_str_from_cstr("SC_PROVIDERS_TEST_KEY"),
    };
    sc_provider_options legacy_generic = {
        .struct_size = sizeof(legacy_generic),
        .credential_env = sc_str_from_cstr("SC_PROVIDERS_PROVIDER_MISSING_KEY"),
    };
    sc_string credential = {0};
    sc_string redacted = {0};
    sc_provider_bedrock_credentials aws = {0};
    fake_bedrock_metadata metadata = {0};
    char credentials_path[] = "/tmp/sc-aws-credentials-XXXXXX";
    char config_path[] = "/tmp/sc-aws-config-XXXXXX";
    int credentials_fd = -1;
    int config_fd = -1;

    failures += sc_test_expect_true("set env", setenv("SC_PROVIDERS_TEST_KEY", "env-secret", 1) == 0);
    failures += sc_test_expect_true("set ignored legacy smolclaw env", setenv("SMOLCLAW_API_KEY", "legacy-smolclaw-secret", 1) == 0);
    failures += sc_test_expect_true("set ignored legacy api env", setenv("API_KEY", "legacy-api-secret", 1) == 0);
    failures += sc_test_expect_status("resolve env credential",
                              sc_provider_resolve_credential(sc_allocator_heap(), &options, &credential),
                              SC_OK);
    failures += sc_test_expect_true("credential value", strcmp(credential.ptr, "env-secret") == 0);
    sc_string_secure_clear(&credential);
    failures += sc_test_expect_status("inline credential wins",
                              sc_provider_resolve_credential(sc_allocator_heap(), &inline_key, &credential),
                              SC_OK);
    failures += sc_test_expect_true("inline credential value", strcmp(credential.ptr, "inline-secret") == 0);
    sc_string_secure_clear(&credential);
    failures += sc_test_expect_status("redact credential",
                              sc_provider_redact_credential(sc_allocator_heap(), sc_str_from_cstr("secret"), &redacted),
                              SC_OK);
    failures += sc_test_expect_true("redacted", strcmp(redacted.ptr, "[REDACTED]") == 0);
    failures += sc_test_expect_status("missing env credential",
                              sc_provider_resolve_credential(sc_allocator_heap(), &missing, &redacted),
                              SC_ERR_SECURITY_DENIED);
    sc_string_clear(&redacted);

    failures += sc_test_expect_true("set generic env", setenv("SC_PROVIDERS_GENERIC_KEY", "generic-secret", 1) == 0);
    failures += sc_test_expect_status("resolve generic credential",
                              sc_provider_resolve_credential(sc_allocator_heap(), &generic, &redacted),
                              SC_OK);
    failures += sc_test_expect_true("generic credential value", strcmp(redacted.ptr, "generic-secret") == 0);
    sc_string_secure_clear(&redacted);

    failures += sc_test_expect_status("resolve secret store credential",
                              sc_provider_resolve_credential(sc_allocator_heap(), &secret_store, &redacted),
                              SC_OK);
    failures += sc_test_expect_true("secret store wins", strcmp(redacted.ptr, "stored-secret") == 0);
    sc_string_secure_clear(&redacted);
    failures += sc_test_expect_status("legacy generic fallback ignored",
                              sc_provider_resolve_credential(sc_allocator_heap(), &legacy_generic, &redacted),
                              SC_ERR_SECURITY_DENIED);
    sc_string_clear(&redacted);

    (void)unsetenv("AWS_ACCESS_KEY_ID");
    (void)unsetenv("AWS_SECRET_ACCESS_KEY");
    (void)unsetenv("AWS_SESSION_TOKEN");
    (void)unsetenv("AWS_PROFILE");
    (void)unsetenv("AWS_SHARED_CREDENTIALS_FILE");
    (void)unsetenv("AWS_CONFIG_FILE");
    (void)unsetenv("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");
    (void)unsetenv("AWS_CONTAINER_CREDENTIALS_FULL_URI");
    (void)unsetenv("AWS_EC2_METADATA_DISABLED");

    failures += sc_test_expect_status("bedrock inline credentials",
                              sc_provider_bedrock_resolve_credentials(
                                  sc_allocator_heap(),
                                  &(sc_provider_options){
                                      .struct_size = sizeof(sc_provider_options),
                                      .api_key = sc_str_from_cstr("inline-access"),
                                      .secret_value = sc_str_from_cstr("inline-secret"),
                                      .session_token = sc_str_from_cstr("inline-token"),
                                  },
                                  &aws),
                              SC_OK);
    failures += sc_test_expect_true("bedrock inline access", strcmp(aws.access_key_id.ptr, "inline-access") == 0);
    failures += sc_test_expect_true("bedrock inline token", strcmp(aws.session_token.ptr, "inline-token") == 0);
    sc_provider_bedrock_credentials_clear(&aws);

    failures += sc_test_expect_true("set aws env access", setenv("AWS_ACCESS_KEY_ID", "env-access", 1) == 0);
    failures += sc_test_expect_true("set aws env secret", setenv("AWS_SECRET_ACCESS_KEY", "env-secret", 1) == 0);
    failures += sc_test_expect_true("set aws env token", setenv("AWS_SESSION_TOKEN", "env-token", 1) == 0);
    failures += sc_test_expect_status("bedrock env credentials",
                              sc_provider_bedrock_resolve_credentials(sc_allocator_heap(),
                                                                      &(sc_provider_options){.struct_size = sizeof(sc_provider_options)},
                                                                      &aws),
                              SC_OK);
    failures += sc_test_expect_true("bedrock env access", strcmp(aws.access_key_id.ptr, "env-access") == 0);
    failures += sc_test_expect_true("bedrock env token", strcmp(aws.session_token.ptr, "env-token") == 0);
    sc_provider_bedrock_credentials_clear(&aws);
    (void)unsetenv("AWS_ACCESS_KEY_ID");
    (void)unsetenv("AWS_SECRET_ACCESS_KEY");
    (void)unsetenv("AWS_SESSION_TOKEN");

    credentials_fd = mkstemp(credentials_path);
    config_fd = mkstemp(config_path);
    failures += sc_test_expect_true("aws credentials temp", credentials_fd >= 0);
    failures += sc_test_expect_true("aws config temp", config_fd >= 0);
    if (credentials_fd >= 0 && config_fd >= 0) {
        FILE *credentials_file = fdopen(credentials_fd, "wb");
        FILE *config_file = fdopen(config_fd, "wb");
        failures += sc_test_expect_true("aws credentials fdopen", credentials_file != nullptr);
        failures += sc_test_expect_true("aws config fdopen", config_file != nullptr);
        if (credentials_file != nullptr) {
            (void)fputs("[default]\n"
                        "aws_access_key_id = default-access\n"
                        "aws_secret_access_key = default-secret\n"
                        "\n[named]\n"
                        "aws_access_key_id = named-access\n"
                        "aws_secret_access_key = named-secret\n"
                        "aws_session_token = named-token\n",
                        credentials_file);
            (void)fclose(credentials_file);
            credentials_fd = -1;
        }
        if (config_file != nullptr) {
            (void)fputs("[profile configonly]\n"
                        "aws_access_key_id = config-access\n"
                        "aws_secret_access_key = config-secret\n"
                        "aws_session_token = config-token\n",
                        config_file);
            (void)fclose(config_file);
            config_fd = -1;
        }
        failures += sc_test_expect_true("set shared credentials", setenv("AWS_SHARED_CREDENTIALS_FILE", credentials_path, 1) == 0);
        failures += sc_test_expect_true("set shared config", setenv("AWS_CONFIG_FILE", config_path, 1) == 0);
        failures += sc_test_expect_status("bedrock default profile",
                                  sc_provider_bedrock_resolve_credentials(sc_allocator_heap(),
                                                                          &(sc_provider_options){.struct_size = sizeof(sc_provider_options)},
                                                                          &aws),
                                  SC_OK);
        failures += sc_test_expect_true("bedrock default access", strcmp(aws.access_key_id.ptr, "default-access") == 0);
        sc_provider_bedrock_credentials_clear(&aws);
        failures += sc_test_expect_true("set aws profile named", setenv("AWS_PROFILE", "named", 1) == 0);
        failures += sc_test_expect_status("bedrock named profile",
                                  sc_provider_bedrock_resolve_credentials(sc_allocator_heap(),
                                                                          &(sc_provider_options){.struct_size = sizeof(sc_provider_options)},
                                                                          &aws),
                                  SC_OK);
        failures += sc_test_expect_true("bedrock named token", strcmp(aws.session_token.ptr, "named-token") == 0);
        sc_provider_bedrock_credentials_clear(&aws);
        failures += sc_test_expect_true("set aws profile config", setenv("AWS_PROFILE", "configonly", 1) == 0);
        failures += sc_test_expect_status("bedrock config profile",
                                  sc_provider_bedrock_resolve_credentials(sc_allocator_heap(),
                                                                          &(sc_provider_options){.struct_size = sizeof(sc_provider_options)},
                                                                          &aws),
                                  SC_OK);
        failures += sc_test_expect_true("bedrock config access", strcmp(aws.access_key_id.ptr, "config-access") == 0);
        failures += sc_test_expect_true("bedrock config token", strcmp(aws.session_token.ptr, "config-token") == 0);
        sc_provider_bedrock_credentials_clear(&aws);
    }
    if (credentials_fd >= 0) {
        (void)close(credentials_fd);
    }
    if (config_fd >= 0) {
        (void)close(config_fd);
    }
    (void)remove(credentials_path);
    (void)remove(config_path);
    (void)unsetenv("AWS_PROFILE");
    failures += sc_test_expect_true("set missing shared credentials",
                            setenv("AWS_SHARED_CREDENTIALS_FILE", "/tmp/sc-missing-aws-credentials", 1) == 0);
    failures += sc_test_expect_true("set missing shared config",
                            setenv("AWS_CONFIG_FILE", "/tmp/sc-missing-aws-config", 1) == 0);

    metadata = (fake_bedrock_metadata){
        .first_response = sc_str_from_cstr("{\"AccessKeyId\":\"ecs-access\",\"SecretAccessKey\":\"ecs-secret\","
                                           "\"Token\":\"ecs-token\"}"),
    };
    sc_provider_bedrock_set_metadata_fetcher(fake_bedrock_metadata_fetch, &metadata);
    failures += sc_test_expect_true("set ecs relative", setenv("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI", "/v2/credentials", 1) == 0);
    failures += sc_test_expect_status("bedrock ecs metadata",
                              sc_provider_bedrock_resolve_credentials(sc_allocator_heap(),
                                                                      &(sc_provider_options){.struct_size = sizeof(sc_provider_options)},
                                                                      &aws),
                              SC_OK);
    failures += sc_test_expect_true("bedrock ecs url", strcmp(metadata.last_url.ptr, "http://169.254.170.2/v2/credentials") == 0);
    failures += sc_test_expect_true("bedrock ecs token", strcmp(aws.session_token.ptr, "ecs-token") == 0);
    sc_provider_bedrock_credentials_clear(&aws);
    sc_string_clear(&metadata.last_url);
    sc_string_clear(&metadata.last_token);
    (void)unsetenv("AWS_CONTAINER_CREDENTIALS_RELATIVE_URI");

    failures += sc_test_expect_true("set denied full uri", setenv("AWS_CONTAINER_CREDENTIALS_FULL_URI", "http://example.com/creds", 1) == 0);
    failures += sc_test_expect_status_key("bedrock metadata url denied",
                                  sc_provider_bedrock_resolve_credentials(sc_allocator_heap(),
                                                                          &(sc_provider_options){.struct_size = sizeof(sc_provider_options)},
                                                                          &aws),
                                  SC_ERR_SECURITY_DENIED,
                                  "sc.provider_bedrock.metadata_url_denied");
    (void)unsetenv("AWS_CONTAINER_CREDENTIALS_FULL_URI");

    metadata = (fake_bedrock_metadata){
        .first_response = sc_str_from_cstr("imds-token"),
        .second_response = sc_str_from_cstr("role-name\n"),
        .third_response = sc_str_from_cstr("{\"AccessKeyId\":\"imds-access\",\"SecretAccessKey\":\"imds-secret\","
                                           "\"Token\":\"imds-session\"}"),
    };
    sc_provider_bedrock_set_metadata_fetcher(fake_bedrock_metadata_fetch, &metadata);
    failures += sc_test_expect_status("bedrock imds metadata",
                              sc_provider_bedrock_resolve_credentials(sc_allocator_heap(),
                                                                      &(sc_provider_options){.struct_size = sizeof(sc_provider_options)},
                                                                      &aws),
                              SC_OK);
    failures += sc_test_expect_true("bedrock imds access", strcmp(aws.access_key_id.ptr, "imds-access") == 0);
    failures += sc_test_expect_true("bedrock imds token header value", strcmp(metadata.last_token.ptr, "imds-token") == 0);
    sc_provider_bedrock_credentials_clear(&aws);
    sc_string_clear(&metadata.last_url);
    sc_string_clear(&metadata.last_token);

    metadata = (fake_bedrock_metadata){0};
    sc_provider_bedrock_set_metadata_fetcher(fake_bedrock_metadata_fetch, &metadata);
    failures += sc_test_expect_true("set imds disabled", setenv("AWS_EC2_METADATA_DISABLED", "true", 1) == 0);
    failures += sc_test_expect_status_key("bedrock imds disabled",
                                  sc_provider_bedrock_resolve_credentials(sc_allocator_heap(),
                                                                          &(sc_provider_options){.struct_size = sizeof(sc_provider_options)},
                                                                          &aws),
                                  SC_ERR_SECURITY_DENIED,
                                  "sc.provider_bedrock.imds_disabled");
    failures += sc_test_expect_true("bedrock imds disabled no fetch", metadata.calls == 0);
    sc_provider_bedrock_set_metadata_fetcher(nullptr, nullptr);

    sc_string_secure_clear(&credential);
    sc_string_secure_clear(&redacted);
    sc_provider_bedrock_credentials_clear(&aws);
    sc_string_clear(&metadata.last_url);
    sc_string_clear(&metadata.last_token);
    (void)unsetenv("SC_PROVIDERS_TEST_KEY");
    (void)unsetenv("SC_PROVIDERS_GENERIC_KEY");
    (void)unsetenv("SMOLCLAW_API_KEY");
    (void)unsetenv("API_KEY");
    (void)unsetenv("AWS_SHARED_CREDENTIALS_FILE");
    (void)unsetenv("AWS_CONFIG_FILE");
    (void)unsetenv("AWS_EC2_METADATA_DISABLED");
    return failures;
}

static int test_provider_rules_gaps(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_provider_sse_parser parser = {0};
    stream_capture stream = {0};
    fake_http http = {
        .response = sc_str_from_cstr("{\"choices\":[{\"message\":{\"content\":\"ok\"}}]}"),
        .http_status = 200,
    };
    sc_provider_options options = {
        .struct_size = sizeof(options),
        .provider_name = sc_str_from_cstr("test-compatible"),
        .base_url = sc_str_from_cstr("https://api.example.test/v1/chat/completions"),
        .api_key = sc_str_from_cstr("secret"),
        .default_model = sc_str_from_cstr("gpt-test"),
        .max_retries = 1,
    };
    sc_provider_vtab chat_only = {
        .struct_size = sizeof(chat_only),
        .abi_major = SC_ABI_VERSION_MAJOR,
        .name = "chat-only",
        .display_name = "Chat only",
        .feature_flag = "SC_TEST_CHAT_ONLY",
        .stability = SC_STABILITY_EXPERIMENTAL,
        .provider_modes = SC_PROVIDER_MODE_CHAT,
    };
    sc_provider_request valid = {
        .struct_size = sizeof(valid),
        .model = sc_str_from_cstr("gpt-test"),
        .prompt = sc_str_from_cstr("hello"),
    };
    sc_provider_request tools = {
        .struct_size = sizeof(tools),
        .model = sc_str_from_cstr("gpt-test"),
        .prompt = sc_str_from_cstr("hello"),
        .tool_specs_json = sc_str_from_cstr("[{\"type\":\"function\",\"function\":{\"name\":\"lookup\",\"parameters\":{\"type\":\"object\"}}}]"),
    };
    sc_provider_request bad_model = {
        .struct_size = sizeof(bad_model),
        .model = sc_str_from_cstr("bad model"),
        .prompt = sc_str_from_cstr("hello"),
    };
    sc_provider_request bad_tools = {
        .struct_size = sizeof(bad_tools),
        .model = sc_str_from_cstr("gpt-test"),
        .prompt = sc_str_from_cstr("hello"),
        .tool_specs_json = sc_str_from_cstr("{\"not\":\"array\"}"),
    };

    failures += sc_test_expect_status("rules compatible new",
                              sc_provider_openai_compatible_new(sc_allocator_heap(), &options, fake_send, &http, &provider),
                              SC_OK);
    const sc_provider_vtab *vtab = sc_provider_vtab_of(provider);
    failures += sc_test_expect_true("rules descriptor description", vtab != nullptr && vtab->description_key != nullptr);
    failures += sc_test_expect_true("rules descriptor schema", vtab != nullptr && vtab->config_schema_ref != nullptr);
    failures += sc_test_expect_true("rules descriptor secret", vtab != nullptr && vtab->required_secret_key_count == 1);
    failures += sc_test_expect_true("rules descriptor modes",
                            vtab != nullptr &&
                                (vtab->provider_modes & SC_PROVIDER_MODE_CHAT) != 0 &&
                                (vtab->provider_modes & SC_PROVIDER_MODE_STREAM) != 0 &&
                                (vtab->provider_modes & SC_PROVIDER_MODE_TOOL_CALLS) != 0);
    failures += sc_test_expect_true("rules descriptor stream caps",
                            vtab != nullptr &&
                                (vtab->capabilities & SC_PROVIDER_CAP_STREAMING) != 0 &&
                                (vtab->capabilities & SC_PROVIDER_CAP_STREAMING_TOOL_EVENTS) != 0 &&
                                (vtab->capabilities & SC_PROVIDER_CAP_REASONING_EVENTS) != 0);
    failures += sc_test_expect_true("rules descriptor timeout",
                            vtab != nullptr && vtab->default_timeout.total_timeout_ms > 0 &&
                                vtab->default_timeout.response_body_limit_bytes > 0);
    failures += sc_test_expect_status("rules valid request", sc_provider_validate_request(vtab, &options, &valid), SC_OK);
    failures += sc_test_expect_status("rules bad model", sc_provider_validate_request(vtab, &options, &bad_model), SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_status("rules malformed tools", sc_provider_validate_request(vtab, &options, &bad_tools), SC_ERR_PARSE);
    failures += sc_test_expect_status("rules tools unsupported", sc_provider_validate_request(&chat_only, &options, &tools), SC_ERR_UNSUPPORTED);
    failures += sc_test_expect_true("rules retry 429", sc_provider_classify_retry(sc_status_ok(), 429) == SC_PROVIDER_RETRY_RATE_LIMIT);
    failures += sc_test_expect_true("rules retry server", sc_provider_should_retry(sc_status_http("sc.test"), 500));
    failures += sc_test_expect_true("rules no retry security", !sc_provider_should_retry(sc_status_security_denied("sc.test"), 0));
    failures += sc_test_expect_true("rules no retry cancel", !sc_provider_should_retry(sc_status_cancelled("sc.test"), 0));
    failures += sc_test_expect_true("rules url https", sc_provider_url_allowed(sc_str_from_cstr("https://api.example.test/path?token=secret"), false));
    failures += sc_test_expect_true("rules url no http remote", !sc_provider_url_allowed(sc_str_from_cstr("http://api.example.test/path"), false));
    failures += sc_test_expect_true("rules url loopback", sc_provider_url_allowed(sc_str_from_cstr("http://127.0.0.1:11434/api/chat"), true));
    sc_provider_destroy(provider);
    provider = nullptr;

    sc_provider_sse_parser_init(&parser, sc_allocator_heap(), 256);
    failures += sc_test_expect_status("rules sse split one",
                              sc_provider_sse_parser_push(&parser,
                                                          sc_str_from_cstr("data: {\"choices\":[{\"delta\":{\"content\":\"he"),
                                                          sc_allocator_heap(),
                                                          capture_stream,
                                                          &stream),
                              SC_OK);
    failures += sc_test_expect_status("rules sse split two",
                              sc_provider_sse_parser_push(&parser,
                                                          sc_str_from_cstr("llo\"}}]}\n\ndata: [DONE]\n\n"),
                                                          sc_allocator_heap(),
                                                          capture_stream,
                                                          &stream),
                              SC_OK);
    failures += sc_test_expect_status("rules sse finish", sc_provider_sse_parser_finish(&parser, capture_stream, &stream), SC_OK);
    failures += sc_test_expect_true("rules sse split text", stream.deltas == 1 && strcmp(stream.text.ptr, "hello") == 0 && stream.done == 1);
    sc_provider_sse_parser_clear(&parser);
    stream_capture_clear(&stream);

    sc_provider_sse_parser_init(&parser, sc_allocator_heap(), 64);
    failures += sc_test_expect_status("rules sse malformed",
                              sc_provider_sse_parser_push(&parser,
                                                          sc_str_from_cstr("data: {\"choices\":42}\n\n"),
                                                          sc_allocator_heap(),
                                                          capture_stream,
                                                          &stream),
                              SC_ERR_PARSE);
    sc_provider_sse_parser_clear(&parser);
    sc_provider_sse_parser_init(&parser, sc_allocator_heap(), 8);
    failures += sc_test_expect_status("rules sse oversized",
                              sc_provider_sse_parser_push(&parser,
                                                          sc_str_from_cstr("data: too-long"),
                                                          sc_allocator_heap(),
                                                          capture_stream,
                                                          &stream),
                              SC_ERR_INVALID_ARGUMENT);
    sc_provider_sse_parser_clear(&parser);
    sc_provider_sse_parser_init(&parser, sc_allocator_heap(), 64);
    failures += sc_test_expect_status("rules sse truncated push",
                              sc_provider_sse_parser_push(&parser,
                                                          sc_str_from_cstr("data: {\"choices\":["),
                                                          sc_allocator_heap(),
                                                          capture_stream,
                                                          &stream),
                              SC_OK);
    failures += sc_test_expect_status("rules sse truncated finish", sc_provider_sse_parser_finish(&parser, capture_stream, &stream), SC_ERR_PARSE);
    sc_provider_sse_parser_clear(&parser);

    failures += sc_test_expect_status("rules compatible remote http denied",
                              sc_provider_openai_compatible_new(sc_allocator_heap(),
                                                                &(sc_provider_options){
                                                                    .struct_size = sizeof(sc_provider_options),
                                                                    .base_url = sc_str_from_cstr("http://api.example.test/v1/chat/completions"),
                                                                    .api_key = sc_str_from_cstr("secret"),
                                                                },
                                                                fake_send,
                                                                &http,
                                                                &provider),
                              SC_ERR_SECURITY_DENIED);
    stream_capture_clear(&stream);
    sc_string_clear(&http.last_request);
    sc_string_clear(&http.last_base_url);
    return failures;
}

static int test_thinking_level_parser(void)
{
    int failures = 0;
    sc_provider_thinking_level level = SC_PROVIDER_THINKING_DEFAULT;

    failures += sc_test_expect_status("thinking default",
                              sc_provider_parse_thinking_level(sc_str_from_cstr("default"), &level),
                              SC_OK);
    failures += sc_test_expect_true("thinking default value", level == SC_PROVIDER_THINKING_DEFAULT);
    failures += sc_test_expect_status("thinking none",
                              sc_provider_parse_thinking_level(sc_str_from_cstr("none"), &level),
                              SC_OK);
    failures += sc_test_expect_true("thinking none value", level == SC_PROVIDER_THINKING_DISABLED);
    failures += sc_test_expect_status("thinking low",
                              sc_provider_parse_thinking_level(sc_str_from_cstr("low"), &level),
                              SC_OK);
    failures += sc_test_expect_true("thinking low value", level == SC_PROVIDER_THINKING_LOW);
    failures += sc_test_expect_status("thinking medium",
                              sc_provider_parse_thinking_level(sc_str_from_cstr("medium"), &level),
                              SC_OK);
    failures += sc_test_expect_true("thinking medium value", level == SC_PROVIDER_THINKING_MEDIUM);
    failures += sc_test_expect_status("thinking high",
                              sc_provider_parse_thinking_level(sc_str_from_cstr("high"), &level),
                              SC_OK);
    failures += sc_test_expect_true("thinking high value", level == SC_PROVIDER_THINKING_HIGH);
    failures += sc_test_expect_status("thinking invalid",
                              sc_provider_parse_thinking_level(sc_str_from_cstr("maximum"), &level),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_true("thinking name", strcmp(sc_provider_thinking_level_name(SC_PROVIDER_THINKING_HIGH), "high") == 0);
    return failures;
}

static int test_reliable_and_router(void)
{
    int failures = 0;
    sc_provider *error_provider = nullptr;
    sc_provider *fallback_provider = nullptr;
    sc_provider *tool_provider = nullptr;
    sc_provider *hint_provider = nullptr;
    sc_provider *retry_provider = nullptr;
    sc_provider *reliable = nullptr;
    sc_provider *retry_reliable = nullptr;
    sc_provider *router = nullptr;
    sc_provider *hint_router = nullptr;
    sc_provider *route_to_reliable_router = nullptr;
    sc_provider *reliable_router_wrapper = nullptr;
    sc_provider *empty_reliable = nullptr;
    sc_provider *candidates[2] = {0};
    sc_provider *single_candidate[1] = {0};
    sc_provider *router_candidate[1] = {0};
    sc_provider_response response = {0};
    sc_provider_request request = {
        .struct_size = sizeof(request),
        .prompt = sc_str_from_cstr("please use a tool"),
    };
    route_observer observer_state = {0};
    sc_observer *observer = nullptr;
    fake_http retry_http = {
        .response = sc_str_from_cstr("{\"choices\":[{\"message\":{\"content\":\"retry-ok\"}}]}"),
        .http_status = 200,
        .fail_status_count = 1,
    };

    failures += sc_test_expect_status("error provider",
                              sc_provider_mock_new(sc_allocator_heap(),
                                                   SC_PROVIDER_MOCK_ERROR,
                                                   sc_str_from_cstr("error"),
                                                   &error_provider),
                              SC_OK);
    failures += sc_test_expect_status("fallback provider",
                              sc_provider_mock_new(sc_allocator_heap(),
                                                   SC_PROVIDER_MOCK_TEXT,
                                                   sc_str_from_cstr("fallback"),
                                                   &fallback_provider),
                              SC_OK);
    candidates[0] = error_provider;
    candidates[1] = fallback_provider;
    failures += sc_test_expect_status("reliable new", sc_provider_reliable_new(sc_allocator_heap(), candidates, 2, &reliable), SC_OK);
    failures += sc_test_expect_status("reliable generate",
                              sc_provider_generate(reliable, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("reliable fallback text", strcmp(response.text.ptr, "fallback") == 0);
    sc_provider_response_clear(&response);

    failures += sc_test_expect_status("retry candidate new",
                              sc_provider_openai_compatible_new(sc_allocator_heap(),
                                                                &(sc_provider_options){
                                                                    .struct_size = sizeof(sc_provider_options),
                                                                    .provider_name = sc_str_from_cstr("retry-compatible"),
                                                                    .base_url = sc_str_from_cstr("https://api.example.test"),
                                                                    .api_key = sc_str_from_cstr("secret"),
                                                                    .default_model = sc_str_from_cstr("gpt-test"),
                                                                    .max_retries = 0,
                                                                },
                                                                fake_send,
                                                                &retry_http,
                                                                &retry_provider),
                              SC_OK);
    single_candidate[0] = retry_provider;
    failures += sc_test_expect_status("retry reliable new",
                              sc_provider_reliable_new_with_options(sc_allocator_heap(), single_candidate, 1, 1, 0, &retry_reliable),
                              SC_OK);
    request.model = sc_str_from_cstr("");
    failures += sc_test_expect_status("retry reliable generate",
                              sc_provider_generate(retry_reliable, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("retry reliable attempts", retry_http.calls == 2);
    failures += sc_test_expect_true("retry reliable text", strcmp(response.text.ptr, "retry-ok") == 0);
    sc_provider_response_clear(&response);

    failures += sc_test_expect_status("tool provider",
                              sc_provider_mock_new(sc_allocator_heap(),
                                                   SC_PROVIDER_MOCK_TEXT,
                                                   sc_str_from_cstr("tool-route"),
                                                   &tool_provider),
                              SC_OK);
    failures += sc_test_expect_status("observer new",
                              sc_observer_new(sc_allocator_heap(), &route_observer_vtab, &observer_state, &observer),
                              SC_OK);
    failures += sc_test_expect_status("router new",
                              sc_provider_router_new(sc_allocator_heap(), fallback_provider, tool_provider, observer, &router),
                              SC_OK);
    failures += sc_test_expect_status("router generate",
                              sc_provider_generate(router, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("router tool text", strcmp(response.text.ptr, "tool-route") == 0);
    failures += sc_test_expect_true("router observer called", observer_state.calls == 1);
    failures += sc_test_expect_true("router observed route", strcmp(observer_state.route.ptr, "tools") == 0);

    sc_provider_response_clear(&response);
    request.prompt = sc_str_from_cstr("plain chat");
    failures += sc_test_expect_status("router default generate",
                              sc_provider_generate(router, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("router default text", strcmp(response.text.ptr, "fallback") == 0);
    sc_provider_response_clear(&response);
    request.prompt = sc_str_from_cstr("please use a tool");

    failures += sc_test_expect_status("hint provider",
                              sc_provider_mock_new(sc_allocator_heap(),
                                                   SC_PROVIDER_MOCK_TEXT,
                                                   sc_str_from_cstr("hint-route"),
                                                   &hint_provider),
                              SC_OK);
    failures += sc_test_expect_status("hint router new",
                              sc_provider_router_routes_new(sc_allocator_heap(),
                                                            fallback_provider,
                                                            &(sc_provider_route){
                                                                .struct_size = sizeof(sc_provider_route),
                                                                .hint = sc_str_from_cstr("reasoning"),
                                                                .provider = hint_provider,
                                                            },
                                                            1,
                                                            nullptr,
                                                            &hint_router),
                              SC_OK);
    request.route_hint = sc_str_from_cstr("cheap, reasoning");
    failures += sc_test_expect_status("hint router generate",
                              sc_provider_generate(hint_router, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("hint router text", strcmp(response.text.ptr, "hint-route") == 0);
    sc_provider_response_clear(&response);

    request.route_hint = sc_str_from_cstr("reasoning");
    failures += sc_test_expect_status("route reliable router new",
                              sc_provider_router_routes_new(sc_allocator_heap(),
                                                            fallback_provider,
                                                            &(sc_provider_route){
                                                                .struct_size = sizeof(sc_provider_route),
                                                                .hint = sc_str_from_cstr("reasoning"),
                                                                .provider = reliable,
                                                            },
                                                            1,
                                                            nullptr,
                                                            &route_to_reliable_router),
                              SC_OK);
    failures += sc_test_expect_status("route can point to reliable",
                              sc_provider_generate(route_to_reliable_router, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("route reliable text", strcmp(response.text.ptr, "fallback") == 0);
    sc_provider_response_clear(&response);

    router_candidate[0] = router;
    request.route_hint = sc_str_from_cstr("");
    failures += sc_test_expect_status("reliable wraps router new",
                              sc_provider_reliable_new_with_options(sc_allocator_heap(),
                                                                    router_candidate,
                                                                    1,
                                                                    0,
                                                                    0,
                                                                    &reliable_router_wrapper),
                              SC_OK);
    failures += sc_test_expect_status("reliable wraps router generate",
                              sc_provider_generate(reliable_router_wrapper, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("reliable wraps router text", strcmp(response.text.ptr, "tool-route") == 0);
    sc_provider_response_clear(&response);

    candidates[0] = error_provider;
    failures += sc_test_expect_status("empty reliable new", sc_provider_reliable_new_with_options(sc_allocator_heap(), candidates, 1, 0, 0, &empty_reliable), SC_OK);
    request.route_hint = sc_str_from_cstr("");
    failures += sc_test_expect_status("all providers fail",
                              sc_provider_generate(empty_reliable, &request, sc_allocator_heap(), &response),
                              SC_ERR_HTTP);
    sc_provider_destroy(empty_reliable);
    sc_provider_destroy(reliable_router_wrapper);
    sc_provider_destroy(route_to_reliable_router);
    sc_provider_destroy(hint_router);
    sc_provider_destroy(hint_provider);
    sc_provider_destroy(router);
    sc_observer_destroy(observer);
    sc_provider_destroy(tool_provider);
    sc_provider_destroy(retry_reliable);
    sc_provider_destroy(retry_provider);
    sc_provider_destroy(reliable);
    sc_provider_destroy(fallback_provider);
    sc_provider_destroy(error_provider);
    sc_string_clear(&retry_http.last_request);
    sc_string_clear(&retry_http.last_base_url);
    return failures;
}

static int test_provider_parity(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_provider_options options = {
        .struct_size = sizeof(options),
        .api_key = sc_str_from_cstr("secret"),
        .default_model = sc_str_from_cstr("model-test"),
    };

#ifdef SC_HAVE_LIBCURL
    failures += sc_test_expect_status("openrouter new", sc_provider_openrouter_new(sc_allocator_heap(), &options, &provider), SC_OK);
    failures += sc_test_expect_true("openrouter vtab", strcmp(sc_provider_vtab_of(provider)->name, "openai-compatible") == 0);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_status("azure missing deployment",
                              sc_provider_azure_openai_new(sc_allocator_heap(), &options, &provider),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_status("azure new",
                              sc_provider_azure_openai_new(sc_allocator_heap(),
                                                           &(sc_provider_options){
                                                               .struct_size = sizeof(sc_provider_options),
                                                               .base_url = sc_str_from_cstr("https://resource.openai.azure.com"),
                                                               .deployment = sc_str_from_cstr("gpt-4o"),
                                                               .api_version = sc_str_from_cstr("2024-10-01-preview"),
                                                               .api_key = sc_str_from_cstr("secret"),
                                                               .default_model = sc_str_from_cstr("gpt-4o"),
                                                           },
                                                           &provider),
                              SC_OK);
    failures += sc_test_expect_true("azure vtab", strcmp(sc_provider_vtab_of(provider)->name, "openai-compatible") == 0);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_status("llamacpp new", sc_provider_llamacpp_new(sc_allocator_heap(), &options, &provider), SC_OK);
    failures += sc_test_expect_true("llamacpp vtab", strcmp(sc_provider_vtab_of(provider)->name, "openai-compatible") == 0);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_status("sglang new", sc_provider_sglang_new(sc_allocator_heap(), &options, &provider), SC_OK);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_status("vllm new", sc_provider_vllm_new(sc_allocator_heap(), &options, &provider), SC_OK);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_status("groq preset new",
                              sc_provider_openai_compatible_preset_new(sc_allocator_heap(),
                                                                       sc_str_from_cstr("groq"),
                                                                       &options,
                                                                       &provider),
                              SC_OK);
    failures += sc_test_expect_true("groq preset vtab", strcmp(sc_provider_vtab_of(provider)->name, "openai-compatible") == 0);
    sc_provider_destroy(provider);
    provider = nullptr;

    failures += sc_test_expect_status("bedrock new",
                              sc_provider_bedrock_new(sc_allocator_heap(),
                                                      &(sc_provider_options){
                                                          .struct_size = sizeof(sc_provider_options),
                                                          .default_model = sc_str_from_cstr("anthropic.claude-test"),
                                                          .region = sc_str_from_cstr("us-east-1"),
                                                      },
                                                      &provider),
                              SC_OK);
    failures += sc_test_expect_true("bedrock vtab", strcmp(sc_provider_vtab_of(provider)->name, "bedrock") == 0);
    sc_provider_destroy(provider);
    provider = nullptr;
#else
    failures += sc_test_expect_status("openrouter unsupported without curl",
                              sc_provider_openrouter_new(sc_allocator_heap(), &options, &provider),
                              SC_ERR_UNSUPPORTED);
#endif

    failures += sc_test_expect_status("gemini cli provider",
                              sc_provider_gemini_cli_new(sc_allocator_heap(), &options, &provider),
                              SC_OK);
    failures += sc_test_expect_true("gemini cli vtab", strcmp(sc_provider_vtab_of(provider)->name, "gemini-cli") == 0);
    sc_provider_destroy(provider);
    provider = nullptr;
    failures += sc_test_expect_status_key("copilot unsupported",
                                  sc_provider_copilot_new(sc_allocator_heap(), &options, &provider),
                                  SC_ERR_UNSUPPORTED,
                                  "sc.provider_copilot.unsupported_oauth");
    failures += sc_test_expect_status("claude code provider",
                              sc_provider_claude_code_new(sc_allocator_heap(), &options, &provider),
                              SC_OK);
    failures += sc_test_expect_true("claude code vtab", strcmp(sc_provider_vtab_of(provider)->name, "claude-code") == 0);
    sc_provider_destroy(provider);
    provider = nullptr;
    failures += sc_test_expect_status_key("telnyx unsupported",
                                  sc_provider_telnyx_new(sc_allocator_heap(), &options, &provider),
                                  SC_ERR_UNSUPPORTED,
                                  "sc.provider_telnyx.unsupported_voice");
    failures += sc_test_expect_status_key("kilocli unsupported",
                                  sc_provider_kilocli_new(sc_allocator_heap(), &options, &provider),
                                  SC_ERR_UNSUPPORTED,
                                  "sc.provider_kilocli.unsupported_process");

    {
        char path[128] = {0};
        FILE *file = nullptr;
        sc_boot_options boot = {0};
        (void)snprintf(path, sizeof(path), "/tmp/sc-providers-provider-%ld.toml", (long)getpid());
        file = fopen(path, "wb");
        failures += sc_test_expect_true("bootstrap config open", file != nullptr);
        if (file != nullptr) {
            (void)fputs("schema_version = 2\n"
                        "[providers]\n"
                        "fallback = \"aws\"\n"
                        "[providers.models.aws]\n"
                        "kind = \"bedrock\"\n"
                        "model = \"anthropic.claude-test\"\n",
                        file);
            (void)fclose(file);
            boot = (sc_boot_options){
                .struct_size = sizeof(boot),
                .config_path = sc_str_from_cstr(path),
                .once = true,
                .max_polls = 0,
            };
            failures += sc_test_expect_status("bootstrap uses provider kind",
                                      sc_runtime_boot(sc_allocator_heap(), &boot),
                                      SC_OK);
            (void)remove(path);
        }
    }
    {
        char path[128] = {0};
        FILE *file = nullptr;
        sc_boot_options boot = {0};
        (void)snprintf(path, sizeof(path), "/tmp/sc-providers-provider-cc-%ld.toml", (long)getpid());
        file = fopen(path, "wb");
        failures += sc_test_expect_true("bootstrap claude code config open", file != nullptr);
        if (file != nullptr) {
            (void)fputs("schema_version = 2\n"
                        "[providers]\n"
                        "fallback = \"cc\"\n"
                        "[providers.models.cc]\n"
                        "kind = \"claude-code\"\n"
                        "[[mcp.servers]]\n"
                        "name = \"claude-code\"\n"
                        "transport = \"stdio\"\n"
                        "command = \"/bin/sh\"\n"
                        "args = [\"-c\", \"cat >/dev/null\"]\n",
                        file);
            (void)fclose(file);
            boot = (sc_boot_options){
                .struct_size = sizeof(boot),
                .config_path = sc_str_from_cstr(path),
                .once = true,
                .max_polls = 0,
            };
            failures += sc_test_expect_status("bootstrap claude code provider",
                                      sc_runtime_boot(sc_allocator_heap(), &boot),
                                      SC_OK);
            (void)remove(path);
        }
    }
    {
        char path[128] = {0};
        FILE *file = nullptr;
        sc_boot_options boot = {0};
        (void)snprintf(path, sizeof(path), "/tmp/sc-providers-provider-cc-missing-%ld.toml", (long)getpid());
        file = fopen(path, "wb");
        failures += sc_test_expect_true("bootstrap claude missing config open", file != nullptr);
        if (file != nullptr) {
            (void)fputs("schema_version = 2\n"
                        "[providers]\n"
                        "fallback = \"cc\"\n"
                        "[providers.models.cc]\n"
                        "kind = \"claude-code\"\n",
                        file);
            (void)fclose(file);
            boot = (sc_boot_options){
                .struct_size = sizeof(boot),
                .config_path = sc_str_from_cstr(path),
                .once = true,
                .max_polls = 0,
            };
            failures += sc_test_expect_status_key("bootstrap claude missing mcp",
                                          sc_runtime_boot(sc_allocator_heap(), &boot),
                                          SC_ERR_UNSUPPORTED,
                                          "sc.provider_claude_code.mcp_server_missing");
            (void)remove(path);
        }
    }
    {
        char path[128] = {0};
        FILE *file = nullptr;
        sc_boot_options boot = {0};
        (void)snprintf(path, sizeof(path), "/tmp/sc-providers-provider-cc-disabled-%ld.toml", (long)getpid());
        file = fopen(path, "wb");
        failures += sc_test_expect_true("bootstrap claude disabled config open", file != nullptr);
        if (file != nullptr) {
            (void)fputs("schema_version = 2\n"
                        "[providers]\n"
                        "fallback = \"cc\"\n"
                        "[providers.models.cc]\n"
                        "kind = \"claude-code\"\n"
                        "[[mcp.servers]]\n"
                        "name = \"claude-code\"\n"
                        "transport = \"stdio\"\n"
                        "command = \"/bin/sh\"\n"
                        "enabled = false\n",
                        file);
            (void)fclose(file);
            boot = (sc_boot_options){
                .struct_size = sizeof(boot),
                .config_path = sc_str_from_cstr(path),
                .once = true,
                .max_polls = 0,
            };
            failures += sc_test_expect_status_key("bootstrap claude disabled mcp",
                                          sc_runtime_boot(sc_allocator_heap(), &boot),
                                          SC_ERR_UNSUPPORTED,
                                          "sc.provider_claude_code.mcp_server_disabled");
            (void)remove(path);
        }
    }
    {
        char path[128] = {0};
        FILE *file = nullptr;
        sc_boot_options boot = {0};
        (void)snprintf(path, sizeof(path), "/tmp/sc-providers-provider-unknown-%ld.toml", (long)getpid());
        file = fopen(path, "wb");
        failures += sc_test_expect_true("bootstrap unknown config open", file != nullptr);
        if (file != nullptr) {
            (void)fputs("schema_version = 2\n"
                        "[providers]\n"
                        "fallback = \"mystery\"\n"
                        "[providers.models.mystery]\n"
                        "kind = \"not-a-provider\"\n"
                        "model = \"model-test\"\n",
                        file);
            (void)fclose(file);
            boot = (sc_boot_options){
                .struct_size = sizeof(boot),
                .config_path = sc_str_from_cstr(path),
                .once = true,
                .max_polls = 0,
            };
            failures += sc_test_expect_status_key("bootstrap unknown kind",
                                          sc_runtime_boot(sc_allocator_heap(), &boot),
                                          SC_ERR_UNSUPPORTED,
                                          "sc.bootstrap.provider_kind_unsupported");
            (void)remove(path);
        }
    }

    return failures;
}

static sc_status fake_send(void *user_data,
                           const sc_provider_options *options,
                           sc_str request_json,
                           sc_allocator *alloc,
                           sc_string *response_json,
                           int *http_status)
{
    fake_http *http = user_data;

    sc_string_clear(&http->last_request);
    sc_string_clear(&http->last_base_url);
    http->calls += 1;
    if (http_status != nullptr) {
        *http_status = http->fail_status_count > 0 ? 500 : http->http_status;
    }
    if (http->fail_status_count > 0) {
        http->fail_status_count -= 1;
    }
    sc_status status = sc_string_from_str(sc_allocator_heap(), request_json, &http->last_request);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = sc_string_from_str(sc_allocator_heap(), options == nullptr ? sc_str_from_cstr("") : options->base_url, &http->last_base_url);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_string_from_str(alloc, http->response, response_json);
}

static sc_status fake_bedrock_metadata_fetch(void *user_data,
                                             sc_str url,
                                             sc_str token,
                                             sc_allocator *alloc,
                                             sc_string *out)
{
    fake_bedrock_metadata *metadata = user_data;
    sc_str response = {0};

    metadata->calls += 1;
    sc_string_clear(&metadata->last_url);
    sc_string_clear(&metadata->last_token);
    sc_status status = sc_string_from_str(sc_allocator_heap(), url, &metadata->last_url);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(sc_allocator_heap(), token, &metadata->last_token);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (metadata->calls == 1) {
        response = metadata->first_response;
    } else if (metadata->calls == 2) {
        response = metadata->second_response;
    } else {
        response = metadata->third_response;
    }
    return sc_string_from_str(alloc, response, out);
}

static sc_status route_emit(void *impl, const sc_observer_event *event)
{
    route_observer *observer = impl;

    observer->calls += 1;
    if (event->field_count > 0) {
        sc_string_clear(&observer->route);
        return sc_string_from_str(sc_allocator_heap(), event->fields[0].value, &observer->route);
    }
    return sc_status_ok();
}

static void route_destroy(void *impl)
{
    route_observer *observer = impl;
    sc_string_clear(&observer->route);
}
