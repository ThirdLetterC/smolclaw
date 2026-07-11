#include "sc/gateway.h"
#include "sc/provider.h"
#include "test_helpers.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#define REQUEST(method_value, path_value, body_value, token_value, idempotency_value, session_value) \
    (&(sc_gateway_request){                                                                        \
        .struct_size = sizeof(sc_gateway_request),                                                  \
        .method = (method_value),                                                                   \
        .path = sc_str_from_cstr(path_value),                                                       \
        .body = sc_str_from_cstr(body_value),                                                       \
        .auth_token = sc_str_from_cstr(token_value),                                                \
        .idempotency_key = sc_str_from_cstr(idempotency_value),                                     \
        .request_id = sc_str_from_cstr("req"),                                                     \
        .session_id = sc_str_from_cstr(session_value),                                              \
    })

#define REQUEST_ORIGIN(method_value, path_value, body_value, token_value, idempotency_value, session_value, origin_value) \
    (&(sc_gateway_request){                                                                                                \
        .struct_size = sizeof(sc_gateway_request),                                                                          \
        .method = (method_value),                                                                                           \
        .path = sc_str_from_cstr(path_value),                                                                               \
        .body = sc_str_from_cstr(body_value),                                                                               \
        .auth_token = sc_str_from_cstr(token_value),                                                                        \
        .idempotency_key = sc_str_from_cstr(idempotency_value),                                                             \
        .request_id = sc_str_from_cstr("req"),                                                                             \
        .session_id = sc_str_from_cstr(session_value),                                                                      \
        .origin = sc_str_from_cstr(origin_value),                                                                           \
    })

static void clear_response(sc_gateway_response *response);
static int test_status_health_and_public_bind(void);
static int test_pairing_auth_rate_and_body_limit(void);
static int test_chat_sse_session_and_config(void);
static int test_gateway_real_transport_wrappers(void);
static int test_route_descriptors_and_errors(void);
static int test_pairing_hardening(void);
static int test_static_files(void);
static sc_status fail_provider_generate(void *impl,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_response *out);
static void fail_provider_destroy(void *impl);

static const sc_provider_vtab fail_provider_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "gateway-fail-provider",
    .display_name = "Gateway Fail Provider",
    .feature_flag = "SC_TEST_GATEWAY_FAIL_PROVIDER",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = fail_provider_generate,
    .destroy = fail_provider_destroy,
};

int main(void)
{
    int failures = 0;

    failures += test_status_health_and_public_bind();
    failures += test_pairing_auth_rate_and_body_limit();
    failures += test_chat_sse_session_and_config();
    failures += test_gateway_real_transport_wrappers();
    failures += test_route_descriptors_and_errors();
    failures += test_pairing_hardening();
    failures += test_static_files();

    return failures == 0 ? 0 : 1;
}

static int test_status_health_and_public_bind(void)
{
    int failures = 0;
    sc_gateway_server *server = nullptr;
    sc_gateway_server *public_server = nullptr;
    sc_gateway_server *timeout_server = nullptr;
    sc_gateway_response response = {0};

    failures += sc_test_expect_status("gateway new",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .auth_token = sc_str_from_cstr("token"),
                                                    },
                                                    &server),
                              SC_OK);
    failures += sc_test_expect_status("status stopped",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_GET, "/status", "", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("status stopped code", response.status == 200 && strcmp(response.body.ptr, "stopped") == 0);
    clear_response(&response);
    failures += sc_test_expect_status("health stopped",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_GET, "/health", "", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("health stopped code", response.status == 503);
    clear_response(&response);
    failures += sc_test_expect_status("start", sc_gateway_server_start(server), SC_OK);
    failures += sc_test_expect_true("running", sc_gateway_server_running(server));
    failures += sc_test_expect_status("health ok",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_GET, "/health", "", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("health ok code", response.status == 200 && strcmp(response.body.ptr, "ok") == 0);
    clear_response(&response);
    failures += sc_test_expect_status("stop", sc_gateway_server_stop(server), SC_OK);
    failures += sc_test_expect_true("stopped", !sc_gateway_server_running(server));

    failures += sc_test_expect_status("public server new",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .bind = sc_str_from_cstr("0.0.0.0"),
                                                        .auth_token = sc_str_from_cstr("token"),
                                                    },
                                                    &public_server),
                              SC_OK);
    failures += sc_test_expect_status("public bind denied", sc_gateway_server_start(public_server), SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_status("timeout server new",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .bind = sc_str_from_cstr("127.0.0.1"),
                                                        .timeout_ms = -1,
                                                    },
                                                    &timeout_server),
                              SC_OK);
    failures += sc_test_expect_status("timeout start", sc_gateway_server_start(timeout_server), SC_OK);
    failures += sc_test_expect_status("timeout enforced",
                              sc_gateway_handle_request(timeout_server,
                                                        REQUEST(SC_GATEWAY_GET, "/health", "", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("timeout code", response.status == 504);
    sc_gateway_response_clear(&response);

    sc_gateway_server_destroy(timeout_server);
    sc_gateway_server_destroy(public_server);
    sc_gateway_server_destroy(server);
    return failures;
}

static int test_pairing_auth_rate_and_body_limit(void)
{
    int failures = 0;
    sc_gateway_server *server = nullptr;
    sc_gateway_server *limited = nullptr;
    sc_gateway_response response = {0};

    failures += sc_test_expect_status("gateway new",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .pairing_code = sc_str_from_cstr("123456"),
                                                        .max_body_bytes = 6,
                                                        .rate_limit = 10,
                                                    },
                                                    &server),
                              SC_OK);
    failures += sc_test_expect_status("start", sc_gateway_server_start(server), SC_OK);
    failures += sc_test_expect_status("pair body large",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_POST, "/pair", "1234567", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("body too large", response.status == 413);
    clear_response(&response);
    failures += sc_test_expect_status("pair wrong",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_POST, "/pair", "0000", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("pair wrong denied", response.status == 401);
    clear_response(&response);
    failures += sc_test_expect_status("pair ok",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_POST, "/pair", "123456", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("pair token random-looking",
                            response.status == 200 &&
                                response.body.len == 64 &&
                                strcmp(response.body.ptr, "paired-device-token") != 0);
    clear_response(&response);
    failures += sc_test_expect_status("auth fail",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_GET, "/sessions", "", "bad", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("auth fail code", response.status == 401);
    clear_response(&response);

    failures += sc_test_expect_status("limited new",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .auth_token = sc_str_from_cstr("token"),
                                                        .rate_limit = 1,
                                                    },
                                                    &limited),
                              SC_OK);
    failures += sc_test_expect_status("limited start", sc_gateway_server_start(limited), SC_OK);
    failures += sc_test_expect_status("limited first",
                              sc_gateway_handle_request(limited,
                                                        REQUEST(SC_GATEWAY_GET, "/health", "", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    clear_response(&response);
    failures += sc_test_expect_status("limited second",
                              sc_gateway_handle_request(limited,
                                                        REQUEST(SC_GATEWAY_GET, "/health", "", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("rate limited", response.status == 429);

    clear_response(&response);
    sc_gateway_server_destroy(limited);
    sc_gateway_server_destroy(server);
    return failures;
}

static int test_chat_sse_session_and_config(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_config config = {0};
    sc_gateway_server *server = nullptr;
    sc_observer *observer = nullptr;
    sc_gateway_response response = {0};
    sc_log_field fields[] = {
        {.key = "message", .value = sc_str_from_cstr("hello"), .secret = false},
        {.key = "api_key", .value = sc_str_from_cstr("secret-value"), .secret = true},
        {.key = "session_id", .value = sc_str_from_cstr("s2"), .secret = false},
    };
    sc_observer_event event = {
        .struct_size = sizeof(event),
        .target = sc_str_from_cstr("test"),
        .name = sc_str_from_cstr("gateway.test"),
        .fields = fields,
        .field_count = SC_ARRAY_LEN(fields),
    };

    failures += sc_test_expect_status("provider",
                              sc_provider_mock_new(sc_allocator_heap(),
                                                   SC_PROVIDER_MOCK_TEXT,
                                                   sc_str_from_cstr("gateway-output"),
                                                   &provider),
                              SC_OK);
    failures += sc_test_expect_status("agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){.struct_size = sizeof(sc_agent_options), .provider = provider},
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("config defaults", sc_config_init_defaults(&config, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("gateway new",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .agent = agent,
                                                        .config = &config,
                                                        .auth_token = sc_str_from_cstr("token"),
                                                        .rate_limit = 100,
                                                        .max_ws_message_bytes = 5,
                                                    },
                                                    &server),
                              SC_OK);
    failures += sc_test_expect_status("start", sc_gateway_server_start(server), SC_OK);
    failures += sc_test_expect_status("chat",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_POST, "/ws/chat", "hello", "token", "", "s1"),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("chat output", response.status == 200 && strcmp(response.body.ptr, "gateway-output") == 0);
    failures += sc_test_expect_true("session exists", sc_gateway_session_find(server, sc_str_from_cstr("s1")) != nullptr);
    clear_response(&response);

    failures += sc_test_expect_status("sessions list",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_GET, "/sessions", "", "token", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("sessions mention s1", response.status == 200 && strstr(response.body.ptr, "s1") != nullptr);
    clear_response(&response);
    failures += sc_test_expect_status("abort",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_POST, "/sessions/abort", "", "token", "abort-1", "s1"),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("abort ok", response.status == 200);
    failures += sc_test_expect_true("session aborted",
                            sc_gateway_session_find(server, sc_str_from_cstr("s1")) != nullptr &&
                                sc_gateway_session_find(server, sc_str_from_cstr("s1"))->state == SC_GATEWAY_SESSION_ABORTED);
    clear_response(&response);
    failures += sc_test_expect_status("disconnect",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_POST, "/sessions/disconnect", "", "token", "disconnect-1", "s1"),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("disconnect ok", response.status == 200);
    failures += sc_test_expect_true("session pruned", sc_gateway_session_find(server, sc_str_from_cstr("s1")) == nullptr);
    clear_response(&response);

    failures += sc_test_expect_status("observer new", sc_gateway_observer_new(sc_allocator_heap(), server, &observer), SC_OK);
    failures += sc_test_expect_status("observer emit", sc_observer_emit(observer, &event), SC_OK);
    failures += sc_test_expect_status("sse",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_GET, "/sse/events", "", "token", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("sse event", response.status == 200 && strstr(response.body.ptr, "event: gateway.test") != nullptr);
    failures += sc_test_expect_true("sse data", strstr(response.body.ptr, "message=hello") != nullptr);
    failures += sc_test_expect_true("sse redacted", strstr(response.body.ptr, "secret-value") == nullptr && strstr(response.body.ptr, "[REDACTED]") != nullptr);
    clear_response(&response);
    failures += sc_test_expect_status("sse isolated",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_GET, "/sse/events", "", "token", "", "s1"),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("sse hidden from other session", response.status == 200 && strstr(response.body.ptr, "gateway.test") == nullptr);
    clear_response(&response);

    failures += sc_test_expect_status("config invalid",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_POST, "/config/set", "gateway.port=not-a-number", "token", "cfg-1", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("config invalid rejected", response.status == 400);
    clear_response(&response);
    failures += sc_test_expect_status("config valid",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_POST, "/config/set", "gateway.port=9090", "token", "cfg-2", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("config valid accepted", response.status == 200 && config.gateway_port == 9090);
    clear_response(&response);
    failures += sc_test_expect_status("config idempotent cached",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_POST, "/config/set", "gateway.port=7070", "token", "cfg-2", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("config idempotent unchanged", response.status == 200 && config.gateway_port == 9090);
    clear_response(&response);
    failures += sc_test_expect_status("config get",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_GET, "/config/get", "gateway.port", "token", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("config get value", response.status == 200 && strcmp(response.body.ptr, "9090") == 0);

    clear_response(&response);
    sc_observer_destroy(observer);
    sc_gateway_server_destroy(server);
    sc_config_clear(&config);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    return failures;
}

static int test_gateway_real_transport_wrappers(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_config config = {0};
    sc_gateway_server *server = nullptr;
    sc_string raw_http = {0};
    sc_string ws_text = {0};
    sc_string rpc = {0};

    failures += sc_test_expect_status("provider",
                              sc_provider_mock_new(sc_allocator_heap(),
                                                   SC_PROVIDER_MOCK_TEXT,
                                                   sc_str_from_cstr("transport-output"),
                                                   &provider),
                              SC_OK);
    failures += sc_test_expect_status("agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){.struct_size = sizeof(sc_agent_options), .provider = provider},
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("config defaults", sc_config_init_defaults(&config, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("gateway new",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .agent = agent,
                                                        .config = &config,
                                                        .auth_token = sc_str_from_cstr("token"),
                                                        .rate_limit = 100,
                                                        .max_ws_message_bytes = 5,
                                                    },
                                                    &server),
                              SC_OK);
    failures += sc_test_expect_status("tls missing rejected",
                              sc_gateway_transport_configure(server,
                                                             &(sc_gateway_transport_options){
                                                                 .struct_size = sizeof(sc_gateway_transport_options),
                                                                 .kind = SC_GATEWAY_TRANSPORT_EMBEDDED,
                                                                 .tls_enabled = true,
                                                                 .websocket_enabled = true,
                                                                 .jsonrpc_enabled = true,
                                                             }),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_status("transport configure",
                              sc_gateway_transport_configure(server,
                                                             &(sc_gateway_transport_options){
                                                                 .struct_size = sizeof(sc_gateway_transport_options),
                                                                 .kind = SC_GATEWAY_TRANSPORT_EMBEDDED,
                                                                 .tls_enabled = true,
                                                                 .tls_cert_path = sc_str_from_cstr("/tmp/cert.pem"),
                                                                 .tls_key_path = sc_str_from_cstr("/tmp/key.pem"),
                                                                 .websocket_enabled = true,
                                                                 .jsonrpc_enabled = true,
                                                             }),
                              SC_OK);
    failures += sc_test_expect_status("start", sc_gateway_server_start(server), SC_OK);
    failures += sc_test_expect_status("http status",
                              sc_gateway_handle_http(server,
                                                     sc_str_from_cstr("GET /status HTTP/1.1\r\nX-Request-Id: http-1\r\n\r\n"),
                                                     sc_allocator_heap(),
                                                     &raw_http),
                              SC_OK);
    failures += sc_test_expect_true("http status body", strstr(raw_http.ptr, "HTTP/1.1 200") != nullptr && strstr(raw_http.ptr, "running") != nullptr);
    sc_string_clear(&raw_http);
    failures += sc_test_expect_status("http jsonrpc chat",
                              sc_gateway_handle_http(server,
                                                     sc_str_from_cstr("POST /jsonrpc HTTP/1.1\r\nAuthorization: Bearer token\r\nContent-Length: 119\r\n\r\n{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"gateway.chat\",\"params\":{\"body\":\"hello\",\"auth_token\":\"token\",\"session_id\":\"rpc\"}}"),
                                                     sc_allocator_heap(),
                                                     &raw_http),
                              SC_OK);
    failures += sc_test_expect_true("http jsonrpc output", strstr(raw_http.ptr, "\"result\":\"transport-output\"") != nullptr);
    sc_string_clear(&raw_http);
    failures += sc_test_expect_status("websocket text",
                              sc_gateway_websocket_receive_text(server,
                                                                sc_str_from_cstr("hello"),
                                                                sc_str_from_cstr("token"),
                                                                sc_str_from_cstr("ws"),
                                                                sc_allocator_heap(),
                                                                &ws_text),
                              SC_OK);
    failures += sc_test_expect_true("websocket output", strcmp(ws_text.ptr, "transport-output") == 0);
    failures += sc_test_expect_status("websocket oversized",
                              sc_gateway_websocket_receive_text(server,
                                                                sc_str_from_cstr("too-long"),
                                                                sc_str_from_cstr("token"),
                                                                sc_str_from_cstr("ws"),
                                                                sc_allocator_heap(),
                                                                &ws_text),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_status("websocket disconnect", sc_gateway_websocket_receive_text(server,
                                                                                        sc_str_from_cstr("[disconnect]"),
                                                                                        sc_str_from_cstr("token"),
                                                                                        sc_str_from_cstr("ws"),
                                                                                        sc_allocator_heap(),
                                                                                        &ws_text),
                              SC_OK);
    failures += sc_test_expect_true("websocket cleanup", sc_gateway_session_find(server, sc_str_from_cstr("ws")) == nullptr);
    failures += sc_test_expect_status("websocket disable configure",
                              sc_gateway_transport_configure(server,
                                                             &(sc_gateway_transport_options){
                                                                 .struct_size = sizeof(sc_gateway_transport_options),
                                                                 .kind = SC_GATEWAY_TRANSPORT_EMBEDDED,
                                                                 .websocket_enabled = false,
                                                                 .jsonrpc_enabled = true,
                                                             }),
                              SC_OK);
    failures += sc_test_expect_status("websocket disabled",
                              sc_gateway_websocket_receive_text(server,
                                                                sc_str_from_cstr("hello"),
                                                                sc_str_from_cstr("token"),
                                                                sc_str_from_cstr("ws"),
                                                                sc_allocator_heap(),
                                                                &ws_text),
                              SC_ERR_UNSUPPORTED);
    failures += sc_test_expect_status("websocket re-enable",
                              sc_gateway_transport_configure(server,
                                                             &(sc_gateway_transport_options){
                                                                 .struct_size = sizeof(sc_gateway_transport_options),
                                                                 .kind = SC_GATEWAY_TRANSPORT_EMBEDDED,
                                                                 .websocket_enabled = true,
                                                                 .jsonrpc_enabled = true,
                                                             }),
                              SC_OK);
    failures += sc_test_expect_status("jsonrpc ping",
                              sc_gateway_jsonrpc_handle(server,
                                                        sc_str_from_cstr("{\"jsonrpc\":\"2.0\",\"id\":\"p\",\"method\":\"ping\"}"),
                                                        sc_allocator_heap(),
                                                        &rpc),
                              SC_OK);
    failures += sc_test_expect_true("jsonrpc pong", strcmp(rpc.ptr, "{\"jsonrpc\":\"2.0\",\"id\":\"p\",\"result\":\"pong\"}") == 0);

    failures += sc_test_expect_status("listener stop before configure", sc_gateway_server_stop(server), SC_OK);
    failures += sc_test_expect_status("listener configure",
                              sc_gateway_transport_configure(server,
                                                             &(sc_gateway_transport_options){
                                                                 .struct_size = sizeof(sc_gateway_transport_options),
                                                                 .kind = SC_GATEWAY_TRANSPORT_EMBEDDED,
                                                                 .listener_enabled = true,
                                                                 .listen_port = 0,
                                                                 .websocket_enabled = true,
                                                                 .jsonrpc_enabled = true,
                                                             }),
                              SC_OK);
    failures += sc_test_expect_status("listener start", sc_gateway_server_start(server), SC_OK);
    failures += sc_test_expect_true("listener bound port", sc_gateway_server_bound_port(server) > 0);
    if (sc_gateway_server_bound_port(server) > 0) {
        int fd = socket(AF_INET, SOCK_STREAM, 0);
        char response_buf[512] = {0};
        struct sockaddr_in addr = {
            .sin_family = AF_INET,
            .sin_port = htons(sc_gateway_server_bound_port(server)),
        };
        (void)inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);
        failures += sc_test_expect_true("listener socket", fd >= 0);
        if (fd >= 0) {
            const char request_text[] = "GET /status HTTP/1.1\r\nX-Request-Id: listener-1\r\n\r\n";
            ssize_t received = -1;
            failures += sc_test_expect_true("listener connect", connect(fd, (const struct sockaddr *)&addr, sizeof(addr)) == 0);
            failures += sc_test_expect_true("listener send", send(fd, request_text, sizeof(request_text) - 1, 0) == (ssize_t)(sizeof(request_text) - 1));
            for (int i = 0; i < 100 && received <= 0; i += 1) {
                failures += sc_test_expect_status("listener poll", sc_gateway_server_poll(server, 0), SC_OK);
                received = recv(fd, response_buf, sizeof(response_buf) - 1, MSG_DONTWAIT);
                if (received < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                    received = 0;
                }
            }
            failures += sc_test_expect_true("listener response received", received > 0);
            failures += sc_test_expect_true("listener response body", strstr(response_buf, "HTTP/1.1 200") != nullptr && strstr(response_buf, "running") != nullptr);
            (void)close(fd);
        }
    }

    sc_string_clear(&rpc);
    sc_string_clear(&ws_text);
    sc_string_clear(&raw_http);
    sc_gateway_server_destroy(server);
    sc_config_clear(&config);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    return failures;
}

static int test_route_descriptors_and_errors(void)
{
    int failures = 0;
    sc_gateway_server *server = nullptr;
    sc_gateway_server *fail_server = nullptr;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_gateway_response response = {0};
    size_t route_count = 0;
    const sc_gateway_route_descriptor *routes = sc_gateway_route_descriptors(&route_count);

    failures += sc_test_expect_true("route descriptors present", routes != nullptr && route_count >= 10);
    failures += sc_test_expect_true("route descriptor metadata",
                            routes != nullptr &&
                                routes[0].struct_size == sizeof(sc_gateway_route_descriptor) &&
                                routes[0].path != nullptr &&
                                routes[0].handler_name != nullptr &&
                                routes[0].output_schema_ref != nullptr);

    failures += sc_test_expect_status("gateway new",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .auth_token = sc_str_from_cstr("token"),
                                                        .rate_limit = 1,
                                                    },
                                                    &server),
                              SC_OK);
    failures += sc_test_expect_status("start", sc_gateway_server_start(server), SC_OK);
    failures += sc_test_expect_status("unknown route",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_GET, "/missing", "", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("unknown route status", response.status == 404);
    clear_response(&response);
    failures += sc_test_expect_status("wrong method",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_POST, "/status", "", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("wrong method status", response.status == 405);
    clear_response(&response);
    failures += sc_test_expect_status("auth before rate",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_GET, "/sessions", "", "bad", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("auth before rate status", response.status == 401);
    clear_response(&response);
    failures += sc_test_expect_status("malformed jsonrpc",
                              sc_gateway_handle_request(server,
                                                        REQUEST(SC_GATEWAY_POST, "/jsonrpc", "{bad", "token", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("malformed jsonrpc public", response.status == 200 && strstr(response.body.ptr, "parse error") != nullptr);
    clear_response(&response);

    failures += sc_test_expect_status("fail provider", sc_provider_new(sc_allocator_heap(), &fail_provider_vtab, nullptr, &provider), SC_OK);
    failures += sc_test_expect_status("fail agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){.struct_size = sizeof(sc_agent_options), .provider = provider},
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("fail gateway",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .agent = agent,
                                                        .auth_token = sc_str_from_cstr("token"),
                                                    },
                                                    &fail_server),
                              SC_OK);
    failures += sc_test_expect_status("fail start", sc_gateway_server_start(fail_server), SC_OK);
    failures += sc_test_expect_status("runtime failure",
                              sc_gateway_handle_request(fail_server,
                                                        REQUEST(SC_GATEWAY_POST, "/ws/chat", "hello", "token", "", "fail"),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("runtime failure redacted", response.status == 500 && strcmp(response.body.ptr, "chat failed") == 0);

    clear_response(&response);
    sc_gateway_server_destroy(fail_server);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_gateway_server_destroy(server);
    return failures;
}

static int test_pairing_hardening(void)
{
    int failures = 0;
    sc_gateway_server *server = nullptr;
    sc_gateway_server *expired = nullptr;
    sc_gateway_server *limited = nullptr;
    sc_gateway_response response = {0};

    failures += sc_test_expect_status("pair hardening gateway",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .pairing_code = sc_str_from_cstr("123456"),
                                                        .pairing_origin = sc_str_from_cstr("https://local.test"),
                                                        .pairing_attempt_limit = 4,
                                                        .pairing_ttl_secs = 60,
                                                    },
                                                    &server),
                              SC_OK);
    failures += sc_test_expect_status("pair hardening start", sc_gateway_server_start(server), SC_OK);
    failures += sc_test_expect_status("pair origin denied",
                              sc_gateway_handle_request(server,
                                                        REQUEST_ORIGIN(SC_GATEWAY_POST, "/pair", "123456", "", "", "", "https://bad.test"),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("pair origin denied status", response.status == 401);
    clear_response(&response);
    failures += sc_test_expect_status("pair origin ok",
                              sc_gateway_handle_request(server,
                                                        REQUEST_ORIGIN(SC_GATEWAY_POST, "/pair", "123456", "", "", "", "https://local.test"),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("pair origin token", response.status == 200 && response.body.len == 64);
    clear_response(&response);
    failures += sc_test_expect_status("pair replay",
                              sc_gateway_handle_request(server,
                                                        REQUEST_ORIGIN(SC_GATEWAY_POST, "/pair", "123456", "", "", "", "https://local.test"),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("pair replay status", response.status == 409);
    clear_response(&response);

    failures += sc_test_expect_status("expired gateway",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .pairing_code = sc_str_from_cstr("123456"),
                                                        .pairing_ttl_secs = 1,
                                                    },
                                                    &expired),
                              SC_OK);
    failures += sc_test_expect_status("expired start", sc_gateway_server_start(expired), SC_OK);
    (void)sleep(2);
    failures += sc_test_expect_status("expired pair",
                              sc_gateway_handle_request(expired,
                                                        REQUEST(SC_GATEWAY_POST, "/pair", "123456", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("expired pair status", response.status == 401);
    clear_response(&response);

    failures += sc_test_expect_status("limited pair gateway",
                              sc_gateway_server_new(sc_allocator_heap(),
                                                    &(sc_gateway_options){
                                                        .struct_size = sizeof(sc_gateway_options),
                                                        .pairing_code = sc_str_from_cstr("123456"),
                                                        .pairing_attempt_limit = 1,
                                                    },
                                                    &limited),
                              SC_OK);
    failures += sc_test_expect_status("limited pair start", sc_gateway_server_start(limited), SC_OK);
    failures += sc_test_expect_status("limited pair first",
                              sc_gateway_handle_request(limited,
                                                        REQUEST(SC_GATEWAY_POST, "/pair", "bad", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    clear_response(&response);
    failures += sc_test_expect_status("limited pair second",
                              sc_gateway_handle_request(limited,
                                                        REQUEST(SC_GATEWAY_POST, "/pair", "123456", "", "", ""),
                                                        sc_allocator_heap(),
                                                        &response),
                              SC_OK);
    failures += sc_test_expect_true("limited pair status", response.status == 429);

    clear_response(&response);
    sc_gateway_server_destroy(limited);
    sc_gateway_server_destroy(expired);
    sc_gateway_server_destroy(server);
    return failures;
}

static int test_static_files(void)
{
    int failures = 0;
    sc_string dir = {0};
    sc_string outside_dir = {0};
    sc_gateway_server *server = nullptr;
    sc_gateway_response response = {0};
    sc_string raw_http = {0};

    failures += sc_test_expect_status("static temp dir", sc_test_make_temp_dir("gateway", &dir), SC_OK);
    failures += sc_test_expect_status("static outside temp dir", sc_test_make_temp_dir("gateway-outside", &outside_dir), SC_OK);

    {
        char index_buf[256] = {0};
        char secret_buf[256] = {0};
        char link_buf[256] = {0};
        char dir_link_buf[256] = {0};
        char outside_file_buf[256] = {0};
        (void)snprintf(index_buf, sizeof(index_buf), "%s/index.html", dir.ptr);
        (void)snprintf(secret_buf, sizeof(secret_buf), "%s/secret.txt", dir.ptr);
        (void)snprintf(link_buf, sizeof(link_buf), "%s/link.txt", dir.ptr);
        (void)snprintf(dir_link_buf, sizeof(dir_link_buf), "%s/outside", dir.ptr);
        (void)snprintf(outside_file_buf, sizeof(outside_file_buf), "%s/private.txt", outside_dir.ptr);
        failures += sc_test_expect_true("write index", sc_test_write_cstr_file(index_buf, "<h1>ok</h1>") == 0);
        failures += sc_test_expect_true("write secret", sc_test_write_cstr_file(secret_buf, "secret") == 0);
        bool symlink_created = symlink(secret_buf, link_buf) == 0;
        failures += sc_test_expect_true("write outside static file", sc_test_write_cstr_file(outside_file_buf, "private") == 0);
        bool dir_symlink_created = symlink(outside_dir.ptr, dir_link_buf) == 0;
        failures += sc_test_expect_true("create static symlink", symlink_created);
        failures += sc_test_expect_status("static gateway",
                                  sc_gateway_server_new(sc_allocator_heap(),
                                                        &(sc_gateway_options){
                                                            .struct_size = sizeof(sc_gateway_options),
                                                            .static_roots = &(sc_gateway_static_root){
                                                                .struct_size = sizeof(sc_gateway_static_root),
                                                                .url_prefix = sc_str_from_cstr("/assets"),
                                                                .filesystem_root = sc_string_as_str(&dir),
                                                            },
                                                            .static_root_count = 1,
                                                        },
                                                        &server),
                                  SC_OK);
        failures += sc_test_expect_status("static start", sc_gateway_server_start(server), SC_OK);
        failures += sc_test_expect_status("static file",
                                  sc_gateway_handle_request(server,
                                                            REQUEST(SC_GATEWAY_GET, "/assets/index.html", "", "", "", ""),
                                                            sc_allocator_heap(),
                                                            &response),
                                  SC_OK);
        failures += sc_test_expect_true("static file body", response.status == 200 && strcmp(response.body.ptr, "<h1>ok</h1>") == 0);
        failures += sc_test_expect_true("static file content type", strcmp(response.content_type.ptr, "text/html") == 0);
        clear_response(&response);
        failures += sc_test_expect_status("static traversal",
                                  sc_gateway_handle_request(server,
                                                            REQUEST(SC_GATEWAY_GET, "/assets/../secret.txt", "", "", "", ""),
                                                            sc_allocator_heap(),
                                                            &response),
                                  SC_OK);
        failures += sc_test_expect_true("static traversal denied", response.status == 403);
        clear_response(&response);
        failures += sc_test_expect_status("static encoded traversal",
                                  sc_gateway_handle_request(server,
                                                            REQUEST(SC_GATEWAY_GET, "/assets/%2e%2e/secret.txt", "", "", "", ""),
                                                            sc_allocator_heap(),
                                                            &response),
                                  SC_OK);
        failures += sc_test_expect_true("static encoded traversal denied", response.status == 403);
        clear_response(&response);
        failures += sc_test_expect_status("static missing",
                                  sc_gateway_handle_request(server,
                                                            REQUEST(SC_GATEWAY_GET, "/assets/missing.txt", "", "", "", ""),
                                                            sc_allocator_heap(),
                                                            &response),
                                  SC_OK);
        failures += sc_test_expect_true("static missing status", response.status == 404);
        clear_response(&response);
        if (symlink_created) {
            failures += sc_test_expect_status("static symlink",
                                      sc_gateway_handle_request(server,
                                                                REQUEST(SC_GATEWAY_GET, "/assets/link.txt", "", "", "", ""),
                                                                sc_allocator_heap(),
                                                                &response),
                                      SC_OK);
            failures += sc_test_expect_true("static symlink denied", response.status == 403 || response.status == 404);
            clear_response(&response);
        }
        if (dir_symlink_created) {
            failures += sc_test_expect_status("static intermediate symlink",
                                      sc_gateway_handle_request(server,
                                                                REQUEST(SC_GATEWAY_GET, "/assets/outside/private.txt", "", "", "", ""),
                                                                sc_allocator_heap(),
                                                                &response),
                                      SC_OK);
            failures += sc_test_expect_true("static intermediate symlink denied", response.status == 403);
            clear_response(&response);
        }
        failures += sc_test_expect_status("static http",
                                  sc_gateway_handle_http(server,
                                                         sc_str_from_cstr("GET /assets/index.html HTTP/1.1\r\n\r\n"),
                                                         sc_allocator_heap(),
                                                         &raw_http),
                                  SC_OK);
        failures += sc_test_expect_true("static http content type", strstr(raw_http.ptr, "Content-Type: text/html") != nullptr);
        sc_string_clear(&raw_http);
        {
            const char raw_without_nul[] = {'G','E','T',' ','/','a','s','s','e','t','s','/','i','n','d','e','x','.','h','t','m','l',' ','H','T','T','P','/','1','.','1','\r','\n','\r','\n'};
            failures += sc_test_expect_status("static non-nul http span",
                                      sc_gateway_handle_http(server,
                                                             sc_str_from_parts(raw_without_nul, sizeof(raw_without_nul)),
                                                             sc_allocator_heap(),
                                                             &raw_http),
                                      SC_OK);
            sc_string_clear(&raw_http);
        }
        (void)unlink(dir_link_buf);
        (void)unlink(outside_file_buf);
        (void)unlink(link_buf);
        (void)unlink(secret_buf);
        (void)unlink(index_buf);
    }

    clear_response(&response);
    sc_gateway_server_destroy(server);
    (void)rmdir(dir.ptr);
    (void)rmdir(outside_dir.ptr);
    sc_string_clear(&dir);
    sc_string_clear(&outside_dir);
    return failures;
}

static void clear_response(sc_gateway_response *response)
{
    sc_gateway_response_clear(response);
}

static sc_status fail_provider_generate(void *impl,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_response *out)
{
    (void)impl;
    (void)request;
    (void)alloc;
    (void)out;
    return sc_status_io("sc.test.gateway.provider_failed");
}

static void fail_provider_destroy(void *impl)
{
    (void)impl;
}
