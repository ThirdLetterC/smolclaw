// cppcheck-suppress-file redundantInitialization
#include "sc/gateway.h"

#include "sc/acp.h"
#include "sc/contract.h"
#include "sc/json.h"
#include "sc/log.h"
#include "sc/time.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <uv.h>

typedef struct gateway_event {
    sc_string name;
    sc_string data;
    sc_string session_id;
} gateway_event;

typedef struct idempotency_entry {
    sc_string key;
    sc_gateway_response response;
} idempotency_entry;

typedef struct gateway_static_root {
    sc_string url_prefix;
    sc_string filesystem_root;
} gateway_static_root;

typedef struct gateway_route {
    sc_gateway_route_descriptor descriptor;
    sc_status (*handler)(sc_gateway_server *server,
                         const sc_gateway_request *request,
                         sc_allocator *alloc,
                         sc_gateway_response *out);
} gateway_route;

typedef struct gateway_client gateway_client;

typedef struct gateway_write {
    uv_write_t req;
    uv_buf_t buf;
    sc_string response;
    gateway_client *client;
} gateway_write;

struct gateway_client {
    sc_gateway_server *server;
    uv_tcp_t handle;
    sc_bytes request;
};

struct sc_gateway_server {
    sc_allocator *alloc;
    sc_agent *agent;
    sc_config *config;
    sc_acp_server *acp;
    sc_string bind;
    sc_string pairing_code;
    sc_string auth_token;
    sc_string pairing_origin;
    uint16_t port;
    bool public_bind_enabled;
    bool running;
    size_t max_body_bytes;
    size_t rate_limit;
    size_t request_count;
    time_t request_window_started;
    size_t pairing_attempt_count;
    time_t pairing_window_started;
    size_t pairing_attempt_limit;
    uint64_t pairing_ttl_secs;
    time_t pairing_created_at;
    bool pairing_used;
    int64_t timeout_ms;
    size_t max_sse_events;
    size_t max_ws_message_bytes;
    size_t max_ws_queued_messages;
    size_t ws_queued_messages;
    sc_vec sessions;
    sc_vec events;
    sc_vec idempotency;
    sc_vec static_roots;
    sc_gateway_transport_kind transport_kind;
    bool listener_enabled;
    bool listener_started;
    uint16_t bound_port;
    bool tls_enabled;
    sc_string tls_cert_path;
    sc_string tls_key_path;
    bool websocket_enabled;
    bool jsonrpc_enabled;
    uv_loop_t loop;
    uv_tcp_t listener;
    bool loop_initialized;
    bool listener_initialized;
};

typedef struct gateway_observer_impl {
    sc_allocator *alloc;
    sc_gateway_server *server;
} gateway_observer_impl;

static sc_status copy_string(sc_allocator *alloc, sc_str value, sc_string *out);
static sc_status response_set(sc_gateway_response *response, sc_allocator *alloc, int status, sc_str body);
static sc_status response_set_typed(sc_gateway_response *response, sc_allocator *alloc, int status, sc_str body, sc_str content_type);
static bool route_is_mutation(const sc_gateway_request *request);
static const gateway_route *route_find(const sc_gateway_server *server, sc_str path, sc_gateway_method method, bool *path_matched);
static bool route_path_matches_static(const sc_gateway_server *server, sc_str path);
static sc_status enforce_gates(sc_gateway_server *server,
                               const gateway_route *route,
                               const sc_gateway_request *request,
                               sc_allocator *alloc,
                               sc_gateway_response *out);
static bool token_matches(sc_gateway_server *server, sc_str token);
static bool pairing_expired(const sc_gateway_server *server);
static sc_status handle_pair(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_status(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_health(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_chat(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_jsonrpc_route(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_acp_route(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_sse(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_sessions(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_abort(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_disconnect(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_config_get(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_config_set(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_status handle_static(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out);
static sc_gateway_session *session_find_mut(sc_gateway_server *server, sc_str session_id);
static sc_status session_remove(sc_gateway_server *server, sc_str session_id);
static sc_status session_get_or_create(sc_gateway_server *server, sc_str session_id, sc_gateway_session **out);
static void session_clear(sc_gateway_session *session);
static void event_clear(gateway_event *event);
static void idempotency_entry_clear(idempotency_entry *entry);
static void static_root_clear(gateway_static_root *root);
static const idempotency_entry *idempotency_find(const sc_gateway_server *server, sc_str key);
static sc_status idempotency_store(sc_gateway_server *server, sc_str key, const sc_gateway_response *response);
static sc_status response_copy(sc_allocator *alloc, const sc_gateway_response *src, sc_gateway_response *out);
static sc_status parse_assignment(sc_str body, sc_str *path, sc_str *value);
static sc_status parse_http_request(sc_str raw, sc_gateway_request *out);
static sc_status write_http_response(const sc_gateway_response *response, sc_allocator *alloc, sc_string *out);
static sc_status append_jsonrpc_result(sc_allocator *alloc, const sc_json_value *id, sc_str result_text, sc_string *out);
static sc_status append_jsonrpc_error(sc_allocator *alloc, const sc_json_value *id, int code, sc_str message, sc_string *out);
static sc_status jsonrpc_id_to_string(sc_string_builder *builder, const sc_json_value *id);
static sc_status jsonrpc_handle_method(sc_gateway_server *server,
                                       sc_str method,
                                       const sc_json_value *params,
                                       const sc_json_value *id,
                                       sc_allocator *alloc,
                                       sc_string *out);
static sc_status request_from_jsonrpc_params(sc_str path,
                                             sc_gateway_method method,
                                             const sc_json_value *params,
                                             sc_gateway_request *out);
static sc_str json_param_str(const sc_json_value *params, const char *name);
static sc_status static_find_root(sc_gateway_server *server,
                                  sc_str path,
                                  const gateway_static_root **root_out,
                                  sc_str *relative_out);
static bool static_relative_path_safe(sc_str relative);
static bool static_path_has_encoded_traversal(sc_str value);
static sc_status static_open_file(const gateway_static_root *root, sc_str relative, int *out_fd);
static sc_str static_content_type(sc_str path);
static sc_status static_read_fd(sc_allocator *alloc, int fd, size_t max_bytes, sc_string *out);
static bool event_visible_to_session(const gateway_event *event, sc_str session_id);
static sc_str event_field_session_id(const sc_observer_event *event);
static sc_status gateway_events_push(sc_gateway_server *server, gateway_event *event);
static sc_status gateway_observer_emit(void *impl, const sc_observer_event *event);
static void gateway_observer_destroy(void *impl);
static sc_status gateway_listener_start(sc_gateway_server *server);
static void gateway_listener_stop(sc_gateway_server *server);
static bool str_equal_cstr(sc_str value, const char *cstr);
static bool str_has_prefix(sc_str value, const char *prefix);
static sc_str trim_header_value(sc_str value);
static sc_status gateway_generate_auth_token(sc_allocator *alloc, sc_string *out);

static void gateway_on_connection(uv_stream_t *listener, int status);
static void gateway_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf);
static void gateway_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf);
static void gateway_write_cb(uv_write_t *req, int status);
static void gateway_client_close_cb(uv_handle_t *handle);
static void gateway_read_buf_free(gateway_client *client, const uv_buf_t *buf);
static bool gateway_http_request_complete(sc_str raw, bool *invalid);
static bool gateway_http_content_length(sc_str raw, size_t *out);
static const char *gateway_find_header_end(sc_str raw);
static const char *gateway_find_bytes(sc_str raw, size_t start, const char *needle, size_t needle_len);

static const sc_observer_vtab gateway_observer_vtab = {
    .struct_size = sizeof(sc_observer_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "gateway-observer",
    .display_name = "Gateway observer buffer",
    .feature_flag = "SC_OBSERVER_GATEWAY",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .emit = gateway_observer_emit,
    .flush = nullptr,
    .destroy = gateway_observer_destroy,
};

static const gateway_route gateway_routes[] = {
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_GET,
                       .path = "/status",
                       .auth_mode = SC_GATEWAY_AUTH_NONE,
                       .required_capability = SC_GATEWAY_CAP_STATUS,
                       .request_size_limit = 0,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "public",
                       .input_schema_ref = "sc.schema.gateway.empty",
                       .output_schema_ref = "sc.schema.gateway.status",
                       .handler_name = "handle_status"},
        .handler = handle_status,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_GET,
                       .path = "/health",
                       .auth_mode = SC_GATEWAY_AUTH_NONE,
                       .required_capability = SC_GATEWAY_CAP_STATUS,
                       .request_size_limit = 0,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "public",
                       .input_schema_ref = "sc.schema.gateway.empty",
                       .output_schema_ref = "sc.schema.gateway.health",
                       .handler_name = "handle_health"},
        .handler = handle_health,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_POST,
                       .path = "/pair",
                       .auth_mode = SC_GATEWAY_AUTH_PAIRING,
                       .required_capability = SC_GATEWAY_CAP_PAIR,
                       .request_size_limit = 128,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "pairing",
                       .input_schema_ref = "sc.schema.gateway.pair.request",
                       .output_schema_ref = "sc.schema.gateway.pair.response",
                       .handler_name = "handle_pair"},
        .handler = handle_pair,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_POST,
                       .path = "/ws/chat",
                       .auth_mode = SC_GATEWAY_AUTH_BEARER,
                       .required_capability = SC_GATEWAY_CAP_CHAT,
                       .request_size_limit = 0,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "authenticated",
                       .input_schema_ref = "sc.schema.gateway.chat.request",
                       .output_schema_ref = "sc.schema.gateway.chat.response",
                       .handler_name = "handle_chat"},
        .handler = handle_chat,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_POST,
                       .path = "/jsonrpc",
                       .auth_mode = SC_GATEWAY_AUTH_BEARER,
                       .required_capability = SC_GATEWAY_CAP_CHAT | SC_GATEWAY_CAP_CONFIG | SC_GATEWAY_CAP_SESSIONS,
                       .request_size_limit = 0,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "authenticated",
                       .input_schema_ref = "sc.schema.gateway.jsonrpc.request",
                       .output_schema_ref = "sc.schema.gateway.jsonrpc.response",
                       .handler_name = "handle_jsonrpc_route"},
        .handler = handle_jsonrpc_route,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_POST,
                       .path = "/acp",
                       .auth_mode = SC_GATEWAY_AUTH_BEARER,
                       .required_capability = SC_GATEWAY_CAP_ACP,
                       .request_size_limit = 0,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "authenticated",
                       .input_schema_ref = "sc.schema.gateway.acp.request",
                       .output_schema_ref = "sc.schema.gateway.acp.response",
                       .handler_name = "handle_acp_route"},
        .handler = handle_acp_route,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_GET,
                       .path = "/sse/events",
                       .auth_mode = SC_GATEWAY_AUTH_BEARER,
                       .required_capability = SC_GATEWAY_CAP_EVENTS,
                       .request_size_limit = 0,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "authenticated",
                       .input_schema_ref = "sc.schema.gateway.events.request",
                       .output_schema_ref = "sc.schema.gateway.events.response",
                       .handler_name = "handle_sse"},
        .handler = handle_sse,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_GET,
                       .path = "/sessions",
                       .auth_mode = SC_GATEWAY_AUTH_BEARER,
                       .required_capability = SC_GATEWAY_CAP_SESSIONS,
                       .request_size_limit = 0,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "authenticated",
                       .input_schema_ref = "sc.schema.gateway.sessions.request",
                       .output_schema_ref = "sc.schema.gateway.sessions.response",
                       .handler_name = "handle_sessions"},
        .handler = handle_sessions,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_POST,
                       .path = "/sessions/abort",
                       .auth_mode = SC_GATEWAY_AUTH_BEARER,
                       .required_capability = SC_GATEWAY_CAP_SESSIONS,
                       .request_size_limit = 256,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "authenticated",
                       .input_schema_ref = "sc.schema.gateway.session_abort.request",
                       .output_schema_ref = "sc.schema.gateway.session_abort.response",
                       .handler_name = "handle_abort"},
        .handler = handle_abort,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_POST,
                       .path = "/sessions/disconnect",
                       .auth_mode = SC_GATEWAY_AUTH_BEARER,
                       .required_capability = SC_GATEWAY_CAP_SESSIONS,
                       .request_size_limit = 256,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "authenticated",
                       .input_schema_ref = "sc.schema.gateway.session_disconnect.request",
                       .output_schema_ref = "sc.schema.gateway.session_disconnect.response",
                       .handler_name = "handle_disconnect"},
        .handler = handle_disconnect,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_GET,
                       .path = "/config/get",
                       .auth_mode = SC_GATEWAY_AUTH_BEARER,
                       .required_capability = SC_GATEWAY_CAP_CONFIG,
                       .request_size_limit = 512,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "authenticated",
                       .input_schema_ref = "sc.schema.gateway.config_get.request",
                       .output_schema_ref = "sc.schema.gateway.config_get.response",
                       .handler_name = "handle_config_get"},
        .handler = handle_config_get,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_POST,
                       .path = "/config/set",
                       .auth_mode = SC_GATEWAY_AUTH_BEARER,
                       .required_capability = SC_GATEWAY_CAP_CONFIG,
                       .request_size_limit = 4096,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "authenticated",
                       .input_schema_ref = "sc.schema.gateway.config_set.request",
                       .output_schema_ref = "sc.schema.gateway.config_set.response",
                       .handler_name = "handle_config_set"},
        .handler = handle_config_set,
    },
    {
        .descriptor = {.struct_size = sizeof(sc_gateway_route_descriptor),
                       .method = SC_GATEWAY_GET,
                       .path = "/static/*",
                       .auth_mode = SC_GATEWAY_AUTH_NONE,
                       .required_capability = SC_GATEWAY_CAP_STATIC,
                       .request_size_limit = 0,
                       .timeout_ms = 0,
                       .rate_limit_bucket = "static",
                       .input_schema_ref = "sc.schema.gateway.static.request",
                       .output_schema_ref = "sc.schema.gateway.static.response",
                       .handler_name = "handle_static"},
        .handler = handle_static,
    },
};

sc_status sc_gateway_server_new(sc_allocator *alloc,
                                const sc_gateway_options *options,
                                sc_gateway_server **out)
{
    sc_gateway_server *server = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr) {
        return sc_status_invalid_argument("sc.gateway.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    server = sc_alloc(alloc, sizeof(*server), _Alignof(sc_gateway_server));
    if (server == nullptr) {
        return sc_status_no_memory();
    }
    *server = (sc_gateway_server){
        .alloc = alloc,
        .agent = options->agent,
        .config = options->config,
        .port = options->port == 0 ? 8080 : options->port,
        .public_bind_enabled = options->public_bind_enabled,
        .max_body_bytes = options->max_body_bytes == 0 ? 65536 : options->max_body_bytes,
        .rate_limit = options->rate_limit == 0 ? 1024 : options->rate_limit,
        .request_window_started = time(nullptr),
        .pairing_attempt_limit = options->pairing_attempt_limit == 0 ? 5 : options->pairing_attempt_limit,
        .pairing_window_started = time(nullptr),
        .pairing_ttl_secs = options->pairing_ttl_secs == 0 ? 300 : options->pairing_ttl_secs,
        .pairing_created_at = time(nullptr),
        .timeout_ms = options->timeout_ms == 0 ? 30000 : options->timeout_ms,
        .max_sse_events = options->max_sse_events == 0 ? 128 : options->max_sse_events,
        .max_ws_message_bytes = options->max_ws_message_bytes == 0 ? 65536 : options->max_ws_message_bytes,
        .max_ws_queued_messages = options->max_ws_queued_messages == 0 ? 16 : options->max_ws_queued_messages,
        .transport_kind = SC_GATEWAY_TRANSPORT_EMBEDDED,
        .websocket_enabled = true,
        .jsonrpc_enabled = true,
    };
    sc_vec_init(&server->sessions, alloc, sizeof(sc_gateway_session));
    sc_vec_init(&server->events, alloc, sizeof(gateway_event));
    sc_vec_init(&server->idempotency, alloc, sizeof(idempotency_entry));
    sc_vec_init(&server->static_roots, alloc, sizeof(gateway_static_root));

    status = copy_string(alloc, options->bind.ptr == nullptr ? sc_str_from_cstr("127.0.0.1") : options->bind, &server->bind);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->pairing_code, &server->pairing_code);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->auth_token, &server->auth_token);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, options->pairing_origin, &server->pairing_origin);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < options->static_root_count; i += 1) {
        gateway_static_root root = {0};
        status = copy_string(alloc, options->static_roots[i].url_prefix, &root.url_prefix);
        if (sc_status_is_ok(status)) {
            status = copy_string(alloc, options->static_roots[i].filesystem_root, &root.filesystem_root);
        }
        if (sc_status_is_ok(status)) {
            status = sc_vec_push(&server->static_roots, &root);
        }
        if (!sc_status_is_ok(status)) {
            static_root_clear(&root);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_str default_model = options->config == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&options->config->provider_default_model);
        sc_acp_options acp_options = {
            .struct_size = sizeof(acp_options),
            .agent = options->agent,
            .default_model = default_model,
            .max_sessions = options->max_ws_queued_messages == 0 ? 16 : options->max_ws_queued_messages,
            .idle_timeout_secs = 3'600,
            .approval_requests_enabled = true,
        };
        status = sc_acp_server_new(alloc, &acp_options, &server->acp);
    }
    if (!sc_status_is_ok(status)) {
        sc_gateway_server_destroy(server);
        return status;
    }
    *out = server;
    return sc_status_ok();
}

sc_status sc_gateway_server_start(sc_gateway_server *server)
{
    if (server == nullptr) {
        return sc_status_invalid_argument("sc.gateway.start_invalid_argument");
    }
    if (!server->public_bind_enabled && !str_equal_cstr(sc_string_as_str(&server->bind), "127.0.0.1") &&
        !str_equal_cstr(sc_string_as_str(&server->bind), "localhost")) {
        return sc_status_security_denied("sc.gateway.public_bind_disabled");
    }
    if (server->listener_enabled && !server->listener_started) {
        sc_status status = gateway_listener_start(server);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    server->running = true;
    return sc_status_ok();
}

sc_status sc_gateway_server_poll(sc_gateway_server *server, uint32_t timeout_ms)
{
    if (server == nullptr) {
        return sc_status_invalid_argument("sc.gateway.poll_invalid_argument");
    }
    (void)timeout_ms;
    if (server->loop_initialized && server->listener_started) {
        int rc = uv_run(&server->loop, UV_RUN_NOWAIT);
        if (rc < 0) {
            return sc_status_io("sc.gateway.listener.poll_failed");
        }
    }
    return sc_status_ok();
}

sc_status sc_gateway_server_stop(sc_gateway_server *server)
{
    if (server == nullptr) {
        return sc_status_invalid_argument("sc.gateway.stop_invalid_argument");
    }
    gateway_listener_stop(server);
    server->running = false;
    return sc_status_ok();
}

bool sc_gateway_server_running(const sc_gateway_server *server)
{
    return server != nullptr && server->running;
}

uint16_t sc_gateway_server_bound_port(const sc_gateway_server *server)
{
    return server == nullptr ? 0 : server->bound_port;
}

void sc_gateway_server_destroy(sc_gateway_server *server)
{
    if (server == nullptr) {
        return;
    }
    gateway_listener_stop(server);
    sc_string_clear(&server->bind);
    sc_string_clear(&server->pairing_code);
    sc_string_clear(&server->auth_token);
    sc_string_clear(&server->pairing_origin);
    sc_string_clear(&server->tls_cert_path);
    sc_string_clear(&server->tls_key_path);
    sc_acp_server_destroy(server->acp);
    for (size_t i = 0; i < server->sessions.len; i += 1) {
        sc_gateway_session *session = sc_vec_at(&server->sessions, i);
        session_clear(session);
    }
    for (size_t i = 0; i < server->events.len; i += 1) {
        gateway_event *event = sc_vec_at(&server->events, i);
        event_clear(event);
    }
    for (size_t i = 0; i < server->idempotency.len; i += 1) {
        idempotency_entry *entry = sc_vec_at(&server->idempotency, i);
        idempotency_entry_clear(entry);
    }
    for (size_t i = 0; i < server->static_roots.len; i += 1) {
        gateway_static_root *root = sc_vec_at(&server->static_roots, i);
        static_root_clear(root);
    }
    sc_vec_clear(&server->sessions);
    sc_vec_clear(&server->events);
    sc_vec_clear(&server->idempotency);
    sc_vec_clear(&server->static_roots);
    sc_free(server->alloc, server, sizeof(*server), _Alignof(sc_gateway_server));
}

sc_status sc_gateway_handle_request(sc_gateway_server *server,
                                    const sc_gateway_request *request,
                                    sc_allocator *alloc,
                                    sc_gateway_response *out)
{
    const gateway_route *route = nullptr;
    sc_status status = sc_status_ok();
    bool path_matched = false;

    if (server == nullptr || request == nullptr || out == nullptr || request->path.ptr == nullptr) {
        return sc_status_invalid_argument("sc.gateway.request_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_gateway_response){.struct_size = sizeof(*out)};
    sc_log_field fields[] = {
        {.key = "request_id", .value = request->request_id, .secret = false},
        {.key = "path", .value = request->path, .secret = false},
    };
    sc_log_write(SC_LOG_TRACE, "sc.gateway", "gateway.request.start", fields, SC_ARRAY_LEN(fields));

    route = route_find(server, request->path, request->method, &path_matched);
    if (route == nullptr) {
        return response_set(out,
                            alloc,
                            path_matched ? 405 : 404,
                            path_matched ? sc_str_from_cstr("method not allowed") : sc_str_from_cstr("not found"));
    }

    status = enforce_gates(server, route, request, alloc, out);
    if (!sc_status_is_ok(status) || out->status != 0) {
        return status;
    }
    if (route_is_mutation(request) && request->idempotency_key.len > 0) {
        const idempotency_entry *cached = idempotency_find(server, request->idempotency_key);
        if (cached != nullptr) {
            return response_copy(alloc, &cached->response, out);
        }
    }

    status = route->handler(server, request, alloc, out);

    if (sc_status_is_ok(status) && route_is_mutation(request) && request->idempotency_key.len > 0 && out->status >= 200 && out->status < 300) {
        status = idempotency_store(server, request->idempotency_key, out);
    }
    return status;
}

sc_status sc_gateway_transport_configure(sc_gateway_server *server,
                                         const sc_gateway_transport_options *options)
{
    sc_status status = sc_status_ok();
    if (server == nullptr || options == nullptr) {
        return sc_status_invalid_argument("sc.gateway_transport.invalid_argument");
    }
    if (options->tls_enabled && (options->tls_cert_path.len == 0 || options->tls_key_path.len == 0)) {
        return sc_status_invalid_argument("sc.gateway_transport.tls_material_missing");
    }
    server->transport_kind = options->kind;
    server->listener_enabled = options->listener_enabled;
    if (options->listener_enabled) {
        server->port = options->listen_port;
    }
    server->tls_enabled = options->tls_enabled;
    server->websocket_enabled = options->websocket_enabled;
    server->jsonrpc_enabled = options->jsonrpc_enabled;
    sc_string_clear(&server->tls_cert_path);
    sc_string_clear(&server->tls_key_path);
    status = copy_string(server->alloc, options->tls_cert_path, &server->tls_cert_path);
    if (sc_status_is_ok(status)) {
        status = copy_string(server->alloc, options->tls_key_path, &server->tls_key_path);
    }
    return status;
}

sc_status sc_gateway_handle_http(sc_gateway_server *server,
                                 sc_str raw_request,
                                 sc_allocator *alloc,
                                 sc_string *out_raw_response)
{
    sc_gateway_request request = {0};
    sc_gateway_response response = {0};
    sc_status status = sc_status_ok();
    if (server == nullptr || out_raw_response == nullptr) {
        return sc_status_invalid_argument("sc.gateway_http.invalid_argument");
    }
    status = parse_http_request(raw_request, &request);
    if (sc_status_is_ok(status)) {
        status = sc_gateway_handle_request(server, &request, alloc, &response);
    }
    if (sc_status_is_ok(status)) {
        status = write_http_response(&response, alloc, out_raw_response);
    }
    sc_gateway_response_clear(&response);
    return status;
}

sc_status sc_gateway_websocket_receive_text(sc_gateway_server *server,
                                            sc_str text,
                                            sc_str auth_token,
                                            sc_str session_id,
                                            sc_allocator *alloc,
                                            sc_string *out_text)
{
    sc_gateway_response response = {0};
    sc_status status = sc_status_ok();
    if (server == nullptr || out_text == nullptr) {
        return sc_status_invalid_argument("sc.gateway_websocket.invalid_argument");
    }
    if (!server->websocket_enabled) {
        return sc_status_unsupported("sc.gateway_websocket.disabled");
    }
    if (sc_str_equal(text, sc_str_from_cstr("[disconnect]"))) {
        return sc_gateway_session_disconnect(server, session_id);
    }
    if (text.len > server->max_ws_message_bytes) {
        return sc_status_invalid_argument("sc.gateway_websocket.message_too_large");
    }
    if (server->ws_queued_messages >= server->max_ws_queued_messages) {
        return sc_status_timeout("sc.gateway_websocket.backpressure");
    }
    server->ws_queued_messages += 1;
    sc_str path = sc_str_equal(session_id, sc_str_from_cstr("acp")) ? sc_str_from_cstr("/acp") : sc_str_from_cstr("/ws/chat");
    sc_gateway_request request = {
        .struct_size = sizeof(request),
        .method = SC_GATEWAY_POST,
        .path = path,
        .body = text,
        .auth_token = auth_token,
        .session_id = session_id,
        .request_id = sc_str_from_cstr("websocket"),
    };
    status = sc_gateway_handle_request(server, &request, alloc, &response);
    server->ws_queued_messages -= 1;
    if (sc_status_is_ok(status) && response.status == 200) {
        status = copy_string(alloc, sc_string_as_str(&response.body), out_text);
    } else if (sc_status_is_ok(status)) {
        status = sc_status_http("sc.gateway_websocket.request_failed");
    }
    sc_gateway_response_clear(&response);
    return status;
}

sc_status sc_gateway_jsonrpc_handle(sc_gateway_server *server,
                                    sc_str request_json,
                                    sc_allocator *alloc,
                                    sc_string *out_response_json)
{
    sc_json_value *root = nullptr;
    const sc_json_value *id = nullptr;
    sc_str version = {0};
    sc_str method = {0};
    sc_status status = sc_status_ok();

    if (server == nullptr || out_response_json == nullptr || request_json.len == 0) {
        return sc_status_invalid_argument("sc.gateway_jsonrpc.invalid_argument");
    }
    if (!server->jsonrpc_enabled) {
        return sc_status_unsupported("sc.gateway_jsonrpc.disabled");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = sc_json_parse(alloc, request_json, &root, nullptr);
    if (!sc_status_is_ok(status) || sc_json_type_of(root) != SC_JSON_OBJECT) {
        sc_status_clear(&status);
        sc_json_destroy(root);
        return append_jsonrpc_error(alloc, nullptr, -32700, sc_str_from_cstr("parse error"), out_response_json);
    }
    id = sc_json_object_get(root, sc_str_from_cstr("id"));
    if (!sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("jsonrpc")), &version) ||
        !sc_str_equal(version, sc_str_from_cstr("2.0")) ||
        !sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("method")), &method)) {
        status = append_jsonrpc_error(alloc, id, -32600, sc_str_from_cstr("invalid request"), out_response_json);
    } else {
        status = jsonrpc_handle_method(server, method, sc_json_object_get(root, sc_str_from_cstr("params")), id, alloc, out_response_json);
    }
    sc_json_destroy(root);
    return status;
}

void sc_gateway_response_clear(sc_gateway_response *response)
{
    if (response == nullptr) {
        return;
    }
    sc_string_clear(&response->body);
    sc_string_clear(&response->content_type);
    *response = (sc_gateway_response){0};
}

const sc_gateway_route_descriptor *sc_gateway_route_descriptors(size_t *count)
{
    static sc_gateway_route_descriptor descriptors[SC_ARRAY_LEN(gateway_routes)];
    static bool initialized = false;

    if (!initialized) {
        for (size_t i = 0; i < SC_ARRAY_LEN(gateway_routes); i += 1) {
            descriptors[i] = gateway_routes[i].descriptor;
        }
        initialized = true;
    }
    if (count != nullptr) {
        *count = SC_ARRAY_LEN(descriptors);
    }
    return descriptors;
}

sc_status sc_gateway_observer_new(sc_allocator *alloc, sc_gateway_server *server, sc_observer **out)
{
    gateway_observer_impl *impl = nullptr;
    sc_status status = sc_status_ok();

    if (server == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.gateway_observer.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(gateway_observer_impl));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (gateway_observer_impl){.alloc = alloc, .server = server};
    status = sc_observer_new(alloc, &gateway_observer_vtab, impl, out);
    if (!sc_status_is_ok(status)) {
        sc_free(alloc, impl, sizeof(*impl), _Alignof(gateway_observer_impl));
    }
    return status;
}

sc_status sc_gateway_session_abort(sc_gateway_server *server, sc_str session_id)
{
    sc_gateway_session *session = session_find_mut(server, session_id);
    if (session == nullptr) {
        return sc_status_invalid_argument("sc.gateway_session.not_found");
    }
    session->abort_requested = true;
    session->state = SC_GATEWAY_SESSION_ABORTED;
    (void)sc_clock_wall(&session->ended_at);
    return sc_status_ok();
}

sc_status sc_gateway_session_disconnect(sc_gateway_server *server, sc_str session_id)
{
    return session_remove(server, session_id);
}

const sc_gateway_session *sc_gateway_session_find(const sc_gateway_server *server, sc_str session_id)
{
    if (server == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < server->sessions.len; i += 1) {
        const sc_gateway_session *session = sc_vec_at_const(&server->sessions, i);
        if (session != nullptr && sc_str_equal(sc_string_as_str(&session->id), session_id)) {
            return session;
        }
    }
    return nullptr;
}

static sc_status copy_string(sc_allocator *alloc, sc_str value, sc_string *out)
{
    return sc_string_from_str(alloc, value.ptr == nullptr ? sc_str_from_cstr("") : value, out);
}

static sc_status response_set(sc_gateway_response *response, sc_allocator *alloc, int status, sc_str body)
{
    return response_set_typed(response, alloc, status, body, sc_str_from_cstr("text/plain"));
}

static sc_status response_set_typed(sc_gateway_response *response, sc_allocator *alloc, int status, sc_str body, sc_str content_type)
{
    if (response == nullptr) {
        return sc_status_invalid_argument("sc.gateway_response.invalid_argument");
    }
    sc_string_clear(&response->body);
    sc_string_clear(&response->content_type);
    response->struct_size = sizeof(*response);
    response->status = status;
    sc_status set_status = copy_string(alloc, body, &response->body);
    if (sc_status_is_ok(set_status)) {
        set_status = copy_string(alloc, content_type, &response->content_type);
    }
    return set_status;
}

static bool route_is_mutation(const sc_gateway_request *request)
{
    return request != nullptr && (request->method == SC_GATEWAY_POST ||
                               request->method == SC_GATEWAY_PUT ||
                               request->method == SC_GATEWAY_DELETE);
}

static const gateway_route *route_find(const sc_gateway_server *server, sc_str path, sc_gateway_method method, bool *path_matched)
{
    if (path_matched != nullptr) {
        *path_matched = false;
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(gateway_routes); i += 1) {
        const gateway_route *route = &gateway_routes[i];
        bool matched = strcmp(route->descriptor.path, "/static/*") == 0
                           ? route_path_matches_static(server, path)
                           : str_equal_cstr(path, route->descriptor.path);
        if (matched) {
            if (path_matched != nullptr) {
                *path_matched = true;
            }
            if (route->descriptor.method == method) {
                return route;
            }
        }
    }
    return nullptr;
}

static bool route_path_matches_static(const sc_gateway_server *server, sc_str path)
{
    if (server == nullptr || path.len == 0) {
        return false;
    }
    for (size_t i = 0; i < server->static_roots.len; i += 1) {
        const gateway_static_root *root = sc_vec_at_const(&server->static_roots, i);
        sc_str prefix = root == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&root->url_prefix);
        if (prefix.len > 0 && path.len >= prefix.len && memcmp(path.ptr, prefix.ptr, prefix.len) == 0 &&
            (path.len == prefix.len || path.ptr[prefix.len] == '/')) {
            return true;
        }
    }
    return false;
}

static sc_status enforce_gates(sc_gateway_server *server,
                               const gateway_route *route,
                               const sc_gateway_request *request,
                               sc_allocator *alloc,
                               sc_gateway_response *out)
{
    constexpr time_t rate_window_seconds = 60;
    time_t now = time(nullptr);

    if (server->timeout_ms <= 0) {
        return response_set(out, alloc, 504, sc_str_from_cstr("gateway timeout"));
    }
    if (!server->running && !str_equal_cstr(request->path, "/status")) {
        return response_set(out, alloc, 503, sc_str_from_cstr("gateway stopped"));
    }
    size_t route_limit = route->descriptor.request_size_limit == 0 || route->descriptor.request_size_limit > server->max_body_bytes
                             ? server->max_body_bytes
                             : route->descriptor.request_size_limit;
    if (request->body.len > route_limit) {
        return response_set(out, alloc, 413, sc_str_from_cstr("body too large"));
    }
    if (route->descriptor.auth_mode == SC_GATEWAY_AUTH_BEARER && !token_matches(server, request->auth_token)) {
        return response_set(out, alloc, 401, sc_str_from_cstr("unauthorized"));
    }
    if (route->descriptor.auth_mode == SC_GATEWAY_AUTH_PAIRING) {
        if (now < server->pairing_window_started || now - server->pairing_window_started >= rate_window_seconds) {
            server->pairing_window_started = now;
            server->pairing_attempt_count = 0;
        }
        if (server->pairing_attempt_count >= server->pairing_attempt_limit) {
            return response_set(out, alloc, 429, sc_str_from_cstr("pairing rate limit exceeded"));
        }
        server->pairing_attempt_count += 1;
    } else {
        if (now < server->request_window_started || now - server->request_window_started >= rate_window_seconds) {
            server->request_window_started = now;
            server->request_count = 0;
        }
        if (server->request_count >= server->rate_limit) {
            return response_set(out, alloc, 429, sc_str_from_cstr("rate limit exceeded"));
        }
        server->request_count += 1;
    }
    if (route_is_mutation(request) && request->idempotency_key.len == 0 &&
        !str_equal_cstr(request->path, "/pair") &&
        !str_equal_cstr(request->path, "/ws/chat") &&
        !str_equal_cstr(request->path, "/jsonrpc") &&
        !str_equal_cstr(request->path, "/acp")) {
        return response_set(out, alloc, 409, sc_str_from_cstr("idempotency key required"));
    }
    return sc_status_ok();
}

static bool token_matches(sc_gateway_server *server, sc_str token)
{
    return server != nullptr && server->auth_token.len > 0 && sc_str_equal(sc_string_as_str(&server->auth_token), token);
}

static bool pairing_expired(const sc_gateway_server *server)
{
    time_t now;
    if (server == nullptr || server->pairing_ttl_secs == 0) {
        return false;
    }
    now = time(nullptr);
    return now < server->pairing_created_at || (uint64_t)(now - server->pairing_created_at) > server->pairing_ttl_secs;
}

static sc_status handle_pair(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    if (server->pairing_code.len == 0 || !sc_str_equal(sc_string_as_str(&server->pairing_code), request->body)) {
        return response_set(out, alloc, 401, sc_str_from_cstr("pairing denied"));
    }
    if (pairing_expired(server)) {
        return response_set(out, alloc, 401, sc_str_from_cstr("pairing expired"));
    }
    if (server->pairing_used) {
        return response_set(out, alloc, 409, sc_str_from_cstr("pairing replayed"));
    }
    if (server->pairing_origin.len > 0 && !sc_str_equal(sc_string_as_str(&server->pairing_origin), request->origin)) {
        return response_set(out, alloc, 401, sc_str_from_cstr("pairing origin denied"));
    }
    if (server->auth_token.len == 0) {
        sc_string_clear(&server->auth_token);
        sc_status status = gateway_generate_auth_token(server->alloc, &server->auth_token);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    server->pairing_used = true;
    return response_set(out, alloc, 200, sc_string_as_str(&server->auth_token));
}

static sc_status handle_status(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    (void)request;
    return response_set(out, alloc, 200, server->running ? sc_str_from_cstr("running") : sc_str_from_cstr("stopped"));
}

static sc_status handle_health(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    (void)request;
    return response_set(out, alloc, server->running ? 200 : 503, server->running ? sc_str_from_cstr("ok") : sc_str_from_cstr("stopped"));
}

static sc_status handle_chat(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    sc_gateway_session *session = nullptr;
    sc_agent_turn_result result = {0};
    sc_turn turn = {0};
    sc_status status = sc_status_ok();

    if (server->agent == nullptr) {
        return response_set(out, alloc, 503, sc_str_from_cstr("agent unavailable"));
    }
    status = session_get_or_create(server,
                                   request->session_id.len == 0 ? sc_str_from_cstr("default") : request->session_id,
                                   &session);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (session->abort_requested) {
        return response_set(out, alloc, 409, sc_str_from_cstr("session aborted"));
    }
    session->state = SC_GATEWAY_SESSION_RUNNING;
    (void)sc_clock_wall(&session->started_at);
    turn = (sc_turn){
        .struct_size = sizeof(turn),
        .input = request->body,
        .session_id = sc_string_as_str(&session->id),
        .cancel_requested = session->abort_requested,
    };
    status = sc_agent_process_message(server->agent, &turn, alloc, &result);
    if (sc_status_is_ok(status)) {
        sc_string_clear(&session->last_output);
        status = copy_string(server->alloc, sc_string_as_str(&result.output), &session->last_output);
    }
    if (sc_status_is_ok(status)) {
        session->state = SC_GATEWAY_SESSION_IDLE;
        (void)sc_clock_wall(&session->ended_at);
        status = response_set(out, alloc, 200, sc_string_as_str(&result.output));
    } else {
        session->state = status.code == SC_ERR_CANCELLED ? SC_GATEWAY_SESSION_ABORTED : SC_GATEWAY_SESSION_IDLE;
        (void)sc_clock_wall(&session->ended_at);
        status = response_set(out,
                              alloc,
                              status.code == SC_ERR_CANCELLED ? 499 : 500,
                              sc_str_from_cstr("chat failed"));
    }
    sc_agent_turn_result_clear(&result);
    return status;
}

static sc_status handle_jsonrpc_route(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    sc_string jsonrpc = {0};
    sc_status status = sc_gateway_jsonrpc_handle(server, request->body, alloc, &jsonrpc);
    if (sc_status_is_ok(status)) {
        status = response_set_typed(out, alloc, 200, sc_string_as_str(&jsonrpc), sc_str_from_cstr("application/json"));
    }
    sc_string_clear(&jsonrpc);
    return status;
}

static sc_status handle_acp_route(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    sc_string jsonrpc = {0};
    sc_status status = sc_status_ok();

    if (server->acp == nullptr) {
        return response_set(out, alloc, 503, sc_str_from_cstr("acp unavailable"));
    }
    status = sc_acp_handle_line(server->acp, request->body, alloc, &jsonrpc);
    if (sc_status_is_ok(status)) {
        status = response_set_typed(out, alloc, 200, sc_string_as_str(&jsonrpc), sc_str_from_cstr("application/json"));
    }
    sc_string_clear(&jsonrpc);
    return status;
}

static sc_status handle_sse(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    sc_string body = {0};

    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < server->events.len; i += 1) {
        const gateway_event *event = sc_vec_at_const(&server->events, i);
        if (event != nullptr && event_visible_to_session(event, request->session_id)) {
            status = sc_string_builder_append_cstr(&builder, "event: ");
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, sc_string_as_str(&event->name));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, "\ndata: ");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, sc_string_as_str(&event->data));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, "\n\n");
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &body);
    }
    if (sc_status_is_ok(status)) {
        status = response_set_typed(out, alloc, 200, sc_string_as_str(&body), sc_str_from_cstr("text/event-stream"));
    }
    sc_string_clear(&body);
    sc_string_builder_clear(&builder);
    return status;
}

static sc_status handle_sessions(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    sc_string_builder builder = {0};
    sc_string body = {0};
    sc_status status = sc_status_ok();
    (void)request;

    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < server->sessions.len; i += 1) {
        const sc_gateway_session *session = sc_vec_at_const(&server->sessions, i);
        if (session != nullptr) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&session->id));
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, "\n");
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &body);
    }
    if (sc_status_is_ok(status)) {
        status = response_set(out, alloc, 200, sc_string_as_str(&body));
    }
    sc_string_clear(&body);
    sc_string_builder_clear(&builder);
    return status;
}

static sc_status handle_abort(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    sc_status status = sc_gateway_session_abort(server, request->session_id.len == 0 ? request->body : request->session_id);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return response_set(out, alloc, 404, sc_str_from_cstr("session not found"));
    }
    return response_set(out, alloc, 200, sc_str_from_cstr("aborted"));
}

static sc_status handle_disconnect(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    sc_status status = sc_gateway_session_disconnect(server, request->session_id.len == 0 ? request->body : request->session_id);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return response_set(out, alloc, 404, sc_str_from_cstr("session not found"));
    }
    return response_set(out, alloc, 200, sc_str_from_cstr("disconnected"));
}

static sc_status handle_config_get(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    sc_string value = {0};
    sc_status status = sc_status_ok();
    if (server->config == nullptr) {
        return response_set(out, alloc, 503, sc_str_from_cstr("config unavailable"));
    }
    status = sc_config_get_prop(server->config, request->body, alloc, &value);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return response_set(out, alloc, 404, sc_str_from_cstr("config path not found"));
    }
    status = response_set(out, alloc, 200, sc_string_as_str(&value));
    sc_string_clear(&value);
    return status;
}

static sc_status handle_config_set(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    sc_str path = {0};
    sc_str value = {0};
    sc_status status = sc_status_ok();
    if (server->config == nullptr) {
        return response_set(out, alloc, 503, sc_str_from_cstr("config unavailable"));
    }
    status = parse_assignment(request->body, &path, &value);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return response_set(out, alloc, 400, sc_str_from_cstr("invalid assignment"));
    }
    status = sc_config_set_prop(server->config, path, value);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return response_set(out, alloc, 400, sc_str_from_cstr("config rejected"));
    }
    return response_set(out, alloc, 200, sc_str_from_cstr("updated"));
}

static sc_status handle_static(sc_gateway_server *server, const sc_gateway_request *request, sc_allocator *alloc, sc_gateway_response *out)
{
    const gateway_static_root *root = nullptr;
    sc_str relative = {0};
    sc_string body = {0};
    sc_status status = sc_status_ok();
    int fd = -1;

    status = static_find_root(server, request->path, &root, &relative);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return response_set(out, alloc, 404, sc_str_from_cstr("not found"));
    }
    if (!static_relative_path_safe(relative)) {
        return response_set(out, alloc, 403, sc_str_from_cstr("forbidden"));
    }
    status = static_open_file(root, relative, &fd);
    if (status.code == SC_ERR_SECURITY_DENIED) {
        sc_status_clear(&status);
        return response_set(out, alloc, 403, sc_str_from_cstr("forbidden"));
    }
    if (sc_status_is_ok(status)) {
        status = static_read_fd(alloc, fd, server->max_body_bytes, &body);
    }
    if (fd >= 0) {
        (void)close(fd);
    }
    if (status.code == SC_ERR_IO) {
        sc_status_clear(&status);
        sc_string_clear(&body);
        return response_set(out, alloc, 404, sc_str_from_cstr("not found"));
    }
    if (sc_status_is_ok(status)) {
        status = response_set_typed(out, alloc, 200, sc_string_as_str(&body), static_content_type(relative));
    }
    sc_string_clear(&body);
    return status;
}

static sc_gateway_session *session_find_mut(sc_gateway_server *server, sc_str session_id)
{
    if (server == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < server->sessions.len; i += 1) {
        sc_gateway_session *session = sc_vec_at(&server->sessions, i);
        if (session != nullptr && sc_str_equal(sc_string_as_str(&session->id), session_id)) {
            return session;
        }
    }
    return nullptr;
}

static sc_status session_remove(sc_gateway_server *server, sc_str session_id)
{
    if (server == nullptr || session_id.ptr == nullptr || session_id.len == 0) {
        return sc_status_invalid_argument("sc.gateway_session.invalid_argument");
    }
    for (size_t i = 0; i < server->sessions.len; i += 1) {
        sc_gateway_session *session = sc_vec_at(&server->sessions, i);
        if (session != nullptr && sc_str_equal(sc_string_as_str(&session->id), session_id)) {
            session_clear(session);
            if (i + 1 < server->sessions.len) {
                char *base = server->sessions.ptr;
                size_t item_size = server->sessions.item_size;
                memmove(base + (i * item_size),
                        base + ((i + 1) * item_size),
                        (server->sessions.len - i - 1) * item_size);
            }
            server->sessions.len -= 1;
            return sc_status_ok();
        }
    }
    return sc_status_invalid_argument("sc.gateway_session.not_found");
}

static sc_status session_get_or_create(sc_gateway_server *server, sc_str session_id, sc_gateway_session **out)
{
    sc_gateway_session session = {.struct_size = sizeof(session)};
    sc_gateway_session *existing = session_find_mut(server, session_id);
    sc_status status = sc_status_ok();

    if (out == nullptr || server == nullptr || session_id.ptr == nullptr || session_id.len == 0) {
        return sc_status_invalid_argument("sc.gateway_session.invalid_argument");
    }
    if (existing != nullptr) {
        *out = existing;
        return sc_status_ok();
    }
    status = copy_string(server->alloc, session_id, &session.id);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&server->sessions, &session);
    }
    if (!sc_status_is_ok(status)) {
        session_clear(&session);
        return status;
    }
    *out = sc_vec_at(&server->sessions, server->sessions.len - 1);
    return sc_status_ok();
}

static void session_clear(sc_gateway_session *session)
{
    if (session == nullptr) {
        return;
    }
    sc_string_clear(&session->id);
    sc_string_clear(&session->last_output);
    *session = (sc_gateway_session){0};
}

static void event_clear(gateway_event *event)
{
    if (event == nullptr) {
        return;
    }
    sc_string_clear(&event->name);
    sc_string_clear(&event->data);
    sc_string_clear(&event->session_id);
    *event = (gateway_event){0};
}

static void idempotency_entry_clear(idempotency_entry *entry)
{
    if (entry == nullptr) {
        return;
    }
    sc_string_clear(&entry->key);
    sc_gateway_response_clear(&entry->response);
    *entry = (idempotency_entry){0};
}

static void static_root_clear(gateway_static_root *root)
{
    if (root == nullptr) {
        return;
    }
    sc_string_clear(&root->url_prefix);
    sc_string_clear(&root->filesystem_root);
    *root = (gateway_static_root){0};
}

static const idempotency_entry *idempotency_find(const sc_gateway_server *server, sc_str key)
{
    if (server == nullptr || key.len == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < server->idempotency.len; i += 1) {
        const idempotency_entry *entry = sc_vec_at_const(&server->idempotency, i);
        if (entry != nullptr && sc_str_equal(sc_string_as_str(&entry->key), key)) {
            return entry;
        }
    }
    return nullptr;
}

static sc_status idempotency_store(sc_gateway_server *server, sc_str key, const sc_gateway_response *response)
{
    idempotency_entry entry = {0};
    sc_status status = copy_string(server->alloc, key, &entry.key);
    if (sc_status_is_ok(status)) {
        status = response_copy(server->alloc, response, &entry.response);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&server->idempotency, &entry);
    }
    if (!sc_status_is_ok(status)) {
        idempotency_entry_clear(&entry);
    }
    return status;
}

static sc_status response_copy(sc_allocator *alloc, const sc_gateway_response *src, sc_gateway_response *out)
{
    if (src == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.gateway_response.copy_invalid_argument");
    }
    return response_set_typed(out,
                              alloc,
                              src->status,
                              sc_string_as_str(&src->body),
                              src->content_type.len == 0 ? sc_str_from_cstr("text/plain") : sc_string_as_str(&src->content_type));
}

static sc_status parse_assignment(sc_str body, sc_str *path, sc_str *value)
{
    const char *equals = nullptr;
    if (body.ptr == nullptr || path == nullptr || value == nullptr) {
        return sc_status_invalid_argument("sc.gateway_config.assignment_invalid_argument");
    }
    equals = memchr(body.ptr, '=', body.len);
    if (equals == nullptr || equals == body.ptr || (size_t)(equals - body.ptr) + 1 >= body.len) {
        return sc_status_parse("sc.gateway_config.assignment_invalid");
    }
    *path = sc_str_from_parts(body.ptr, (size_t)(equals - body.ptr));
    *value = sc_str_from_parts(equals + 1, body.len - (size_t)(equals - body.ptr) - 1);
    return sc_status_ok();
}

static sc_status parse_http_request(sc_str raw, sc_gateway_request *out)
{
    const char *line_end = nullptr;
    const char *headers_end = nullptr;
    sc_str request_line = {0};
    sc_str method = {0};
    sc_str path = {0};
    size_t method_end = SIZE_MAX;
    size_t path_end = SIZE_MAX;

    if (out == nullptr || raw.ptr == nullptr || raw.len == 0) {
        return sc_status_invalid_argument("sc.gateway_http.parse_invalid_argument");
    }
    if (memchr(raw.ptr, '\0', raw.len) != nullptr) {
        return sc_status_parse("sc.gateway_http.embedded_nul");
    }
    line_end = gateway_find_bytes(raw, 0, "\r\n", 2);
    headers_end = gateway_find_bytes(raw, 0, "\r\n\r\n", 4);
    if (line_end == nullptr || headers_end == nullptr) {
        return sc_status_parse("sc.gateway_http.invalid_request");
    }
    request_line = sc_str_from_parts(raw.ptr, (size_t)(line_end - raw.ptr));
    for (size_t i = 0; i < request_line.len; i += 1) {
        if (request_line.ptr[i] == ' ') {
            if (method_end == SIZE_MAX) {
                method_end = i;
            } else {
                path_end = i;
                break;
            }
        }
    }
    if (method_end == SIZE_MAX || path_end == SIZE_MAX || method_end + 1 >= path_end) {
        return sc_status_parse("sc.gateway_http.invalid_request_line");
    }
    method = sc_str_from_parts(request_line.ptr, method_end);
    path = sc_str_from_parts(request_line.ptr + method_end + 1, path_end - method_end - 1);
    *out = (sc_gateway_request){
        .struct_size = sizeof(*out),
        .path = path,
        .body = sc_str_from_parts(headers_end + 4, raw.len - (size_t)((headers_end + 4) - raw.ptr)),
    };
    if (sc_str_equal(method, sc_str_from_cstr("GET"))) {
        out->method = SC_GATEWAY_GET;
    } else if (sc_str_equal(method, sc_str_from_cstr("POST"))) {
        out->method = SC_GATEWAY_POST;
    } else if (sc_str_equal(method, sc_str_from_cstr("PUT"))) {
        out->method = SC_GATEWAY_PUT;
    } else if (sc_str_equal(method, sc_str_from_cstr("DELETE"))) {
        out->method = SC_GATEWAY_DELETE;
    } else {
        return sc_status_parse("sc.gateway_http.unsupported_method");
    }
    for (const char *cursor = line_end + 2; cursor < headers_end;) {
        size_t offset = (size_t)(cursor - raw.ptr);
        const char *next = gateway_find_bytes(raw, offset, "\r\n", 2);
        const char *colon = nullptr;
        if (next == nullptr || next > headers_end) {
            break;
        }
        colon = memchr(cursor, ':', (size_t)(next - cursor));
        if (colon != nullptr) {
            sc_str name = sc_str_from_parts(cursor, (size_t)(colon - cursor));
            sc_str value = trim_header_value(sc_str_from_parts(colon + 1, (size_t)(next - colon - 1)));
            if (sc_str_equal(name, sc_str_from_cstr("Authorization")) && str_has_prefix(value, "Bearer ")) {
                out->auth_token = sc_str_from_parts(value.ptr + 7, value.len - 7);
            } else if (sc_str_equal(name, sc_str_from_cstr("X-Request-Id"))) {
                out->request_id = value;
            } else if (sc_str_equal(name, sc_str_from_cstr("X-Session-Id"))) {
                out->session_id = value;
            } else if (sc_str_equal(name, sc_str_from_cstr("Origin"))) {
                out->origin = value;
            } else if (sc_str_equal(name, sc_str_from_cstr("Idempotency-Key"))) {
                out->idempotency_key = value;
            }
        }
        cursor = next + 2;
    }
    return sc_status_ok();
}

static sc_status write_http_response(const sc_gateway_response *response, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    char status_text[32] = {0};
    char length_text[32] = {0};
    sc_str content_type = sc_string_as_str(&response->content_type);
    if (response == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.gateway_http.response_invalid_argument");
    }
    (void)snprintf(status_text, sizeof(status_text), "%d", response->status);
    (void)snprintf(length_text, sizeof(length_text), "%zu", response->body.len);
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "HTTP/1.1 ");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, status_text);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\r\nContent-Type: ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, content_type.len == 0 ? sc_str_from_cstr("text/plain") : content_type);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\r\nContent-Length: ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, length_text);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\r\n\r\n");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, sc_string_as_str(&response->body));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_jsonrpc_result(sc_allocator *alloc, const sc_json_value *id, sc_str result_text, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (sc_status_is_ok(status)) {
        status = jsonrpc_id_to_string(&builder, id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ",\"result\":\"");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < result_text.len; i += 1) {
        char ch = result_text.ptr[i];
        if (ch == '"' || ch == '\\') {
            char escaped[2] = {'\\', ch};
            status = sc_string_builder_append(&builder, sc_str_from_parts(escaped, sizeof(escaped)));
        } else if (ch == '\n') {
            status = sc_string_builder_append_cstr(&builder, "\\n");
        } else {
            status = sc_string_builder_append(&builder, sc_str_from_parts(&ch, 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\"}");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_jsonrpc_error(sc_allocator *alloc, const sc_json_value *id, int code, sc_str message, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();
    char code_text[32] = {0};
    (void)snprintf(code_text, sizeof(code_text), "%d", code);
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "{\"jsonrpc\":\"2.0\",\"id\":");
    if (sc_status_is_ok(status)) {
        status = jsonrpc_id_to_string(&builder, id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ",\"error\":{\"code\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, code_text);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ",\"message\":\"");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, message);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\"}}");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status jsonrpc_id_to_string(sc_string_builder *builder, const sc_json_value *id)
{
    sc_str text = {0};
    double number = 0.0;
    if (id == nullptr || sc_json_is_null(id)) {
        return sc_string_builder_append_cstr(builder, "null");
    }
    if (sc_json_as_str(id, &text)) {
        sc_status status = sc_string_builder_append_cstr(builder, "\"");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(builder, text);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(builder, "\"");
        }
        return status;
    }
    if (sc_json_as_number(id, &number)) {
        char number_text[64] = {0};

        (void)snprintf(number_text, sizeof(number_text), "%.0f", number);
        return sc_string_builder_append_cstr(builder, number_text);
    }
    return sc_string_builder_append_cstr(builder, "null");
}

static sc_status jsonrpc_handle_method(sc_gateway_server *server,
                                       sc_str method,
                                       const sc_json_value *params,
                                       const sc_json_value *id,
                                       sc_allocator *alloc,
                                       sc_string *out)
{
    sc_gateway_request request = {0};
    sc_gateway_response response = {0};
    sc_status status = sc_status_ok();
    if (sc_str_equal(method, sc_str_from_cstr("ping"))) {
        return append_jsonrpc_result(alloc, id, sc_str_from_cstr("pong"), out);
    }
    if (sc_str_equal(method, sc_str_from_cstr("gateway.status"))) {
        status = request_from_jsonrpc_params(sc_str_from_cstr("/status"), SC_GATEWAY_GET, params, &request);
    } else if (sc_str_equal(method, sc_str_from_cstr("gateway.health"))) {
        status = request_from_jsonrpc_params(sc_str_from_cstr("/health"), SC_GATEWAY_GET, params, &request);
    } else if (sc_str_equal(method, sc_str_from_cstr("gateway.chat"))) {
        status = request_from_jsonrpc_params(sc_str_from_cstr("/ws/chat"), SC_GATEWAY_POST, params, &request);
    } else if (sc_str_equal(method, sc_str_from_cstr("gateway.config.get"))) {
        status = request_from_jsonrpc_params(sc_str_from_cstr("/config/get"), SC_GATEWAY_GET, params, &request);
    } else if (sc_str_equal(method, sc_str_from_cstr("gateway.config.set"))) {
        status = request_from_jsonrpc_params(sc_str_from_cstr("/config/set"), SC_GATEWAY_POST, params, &request);
    } else if (sc_str_equal(method, sc_str_from_cstr("gateway.sessions"))) {
        status = request_from_jsonrpc_params(sc_str_from_cstr("/sessions"), SC_GATEWAY_GET, params, &request);
    } else {
        return append_jsonrpc_error(alloc, id, -32601, sc_str_from_cstr("method not found"), out);
    }
    if (sc_status_is_ok(status)) {
        status = sc_gateway_handle_request(server, &request, alloc, &response);
    }
    if (sc_status_is_ok(status) && response.status >= 200 && response.status < 300) {
        status = append_jsonrpc_result(alloc, id, sc_string_as_str(&response.body), out);
    } else if (sc_status_is_ok(status)) {
        status = append_jsonrpc_error(alloc, id, response.status, sc_string_as_str(&response.body), out);
    }
    sc_gateway_response_clear(&response);
    return status;
}

static sc_status request_from_jsonrpc_params(sc_str path,
                                             sc_gateway_method method,
                                             const sc_json_value *params,
                                             sc_gateway_request *out)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.gateway_jsonrpc.params_invalid_argument");
    }
    *out = (sc_gateway_request){
        .struct_size = sizeof(*out),
        .method = method,
        .path = path,
        .body = json_param_str(params, "body"),
        .auth_token = json_param_str(params, "auth_token"),
        .idempotency_key = json_param_str(params, "idempotency_key"),
        .request_id = json_param_str(params, "request_id"),
        .session_id = json_param_str(params, "session_id"),
        .origin = json_param_str(params, "origin"),
    };
    return sc_status_ok();
}

static sc_str json_param_str(const sc_json_value *params, const char *name)
{
    sc_str out = {0};
    if (params == nullptr || name == nullptr || sc_json_type_of(params) != SC_JSON_OBJECT) {
        return sc_str_from_cstr("");
    }
    if (!sc_json_as_str(sc_json_object_get(params, sc_str_from_cstr(name)), &out)) {
        return sc_str_from_cstr("");
    }
    return out;
}

static sc_status static_find_root(sc_gateway_server *server,
                                  sc_str path,
                                  const gateway_static_root **root_out,
                                  sc_str *relative_out)
{
    if (server == nullptr || root_out == nullptr || relative_out == nullptr) {
        return sc_status_invalid_argument("sc.gateway_static.invalid_argument");
    }
    for (size_t i = 0; i < server->static_roots.len; i += 1) {
        const gateway_static_root *root = sc_vec_at_const(&server->static_roots, i);
        sc_str prefix = root == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&root->url_prefix);
        if (prefix.len > 0 && path.len >= prefix.len && memcmp(path.ptr, prefix.ptr, prefix.len) == 0 &&
            (path.len == prefix.len || path.ptr[prefix.len] == '/')) {
            *root_out = root;
            *relative_out = sc_str_from_parts(path.ptr + prefix.len, path.len - prefix.len);
            if (relative_out->len > 0 && relative_out->ptr[0] == '/') {
                relative_out->ptr += 1;
                relative_out->len -= 1;
            }
            if (relative_out->len == 0) {
                *relative_out = sc_str_from_cstr("index.html");
            }
            return sc_status_ok();
        }
    }
    return sc_status_invalid_argument("sc.gateway_static.not_found");
}

static bool static_relative_path_safe(sc_str relative)
{
    size_t segment_start = 0;
    if (relative.ptr == nullptr || relative.len == 0 || relative.ptr[0] == '/' ||
        relative.ptr[0] == '\\' || static_path_has_encoded_traversal(relative)) {
        return false;
    }
    for (size_t i = 0; i <= relative.len; i += 1) {
        if (i == relative.len || relative.ptr[i] == '/') {
            size_t segment_len = i - segment_start;
            if (segment_len == 0) {
                return false;
            }
            if (segment_len == 1 && relative.ptr[segment_start] == '.') {
                return false;
            }
            if (segment_len == 2 && relative.ptr[segment_start] == '.' && relative.ptr[segment_start + 1] == '.') {
                return false;
            }
            segment_start = i + 1;
        } else if (relative.ptr[i] == '\\') {
            return false;
        }
    }
    return true;
}

static bool static_path_has_encoded_traversal(sc_str value)
{
    for (size_t i = 0; i + 2 < value.len; i += 1) {
        if (value.ptr[i] == '%') {
            char a = value.ptr[i + 1];
            char b = value.ptr[i + 2];
            if ((a == '2') && (b == 'e' || b == 'E' || b == 'f' || b == 'F')) {
                return true;
            }
            if ((a == '5') && (b == 'c' || b == 'C')) {
                return true;
            }
        }
    }
    return false;
}

static sc_status static_open_file(const gateway_static_root *root, sc_str relative, int *out_fd)
{
    constexpr size_t max_segment_len = 255;
    sc_str filesystem_root = root == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&root->filesystem_root);
    struct stat st = {0};
    int parent_fd = -1;
    size_t start = 0;

    if (out_fd == nullptr || filesystem_root.ptr == nullptr || filesystem_root.len == 0 ||
        relative.ptr == nullptr || relative.len == 0) {
        return sc_status_invalid_argument("sc.gateway_static.open_invalid_argument");
    }
    *out_fd = -1;
    parent_fd = open(filesystem_root.ptr, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (parent_fd < 0) {
        return sc_status_io("sc.gateway_static.root_open_failed");
    }
    while (start < relative.len) {
        char segment[256] = {0};
        size_t end = start;
        size_t segment_len = 0;
        bool final = false;
        int flags = O_RDONLY | O_CLOEXEC | O_NOFOLLOW;

        while (end < relative.len && relative.ptr[end] != '/') {
            end += 1;
        }
        segment_len = end - start;
        final = end == relative.len;
        if (segment_len == 0 || segment_len > max_segment_len) {
            (void)close(parent_fd);
            return sc_status_security_denied("sc.gateway_static.unsafe_path");
        }
        (void)memcpy(segment, relative.ptr + start, segment_len);
        if (!final) {
            flags |= O_DIRECTORY;
        }
        int child_fd = openat(parent_fd, segment, flags);
        if (child_fd < 0) {
            int saved_errno = errno;
            (void)close(parent_fd);
            return saved_errno == ELOOP || saved_errno == ENOTDIR
                       ? sc_status_security_denied("sc.gateway_static.unsafe_path")
                       : sc_status_io("sc.gateway_static.not_found");
        }
        (void)close(parent_fd);
        parent_fd = child_fd;
        start = end + 1;
    }
    if (fstat(parent_fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(parent_fd);
        return sc_status_security_denied("sc.gateway_static.unsafe_path");
    }
    *out_fd = parent_fd;
    return sc_status_ok();
}

static sc_str static_content_type(sc_str path)
{
    if (path.len >= 5 && memcmp(path.ptr + path.len - 5, ".html", 5) == 0) {
        return sc_str_from_cstr("text/html");
    }
    if (path.len >= 4 && memcmp(path.ptr + path.len - 4, ".css", 4) == 0) {
        return sc_str_from_cstr("text/css");
    }
    if (path.len >= 3 && memcmp(path.ptr + path.len - 3, ".js", 3) == 0) {
        return sc_str_from_cstr("application/javascript");
    }
    if (path.len >= 5 && memcmp(path.ptr + path.len - 5, ".json", 5) == 0) {
        return sc_str_from_cstr("application/json");
    }
    if (path.len >= 4 && memcmp(path.ptr + path.len - 4, ".txt", 4) == 0) {
        return sc_str_from_cstr("text/plain");
    }
    if (path.len >= 4 && memcmp(path.ptr + path.len - 4, ".svg", 4) == 0) {
        return sc_str_from_cstr("image/svg+xml");
    }
    return sc_str_from_cstr("application/octet-stream");
}

static sc_status static_read_fd(sc_allocator *alloc, int fd, size_t max_bytes, sc_string *out)
{
    struct stat st = {0};
    sc_string text = {0};
    size_t allocation_size = 0;
    size_t offset = 0;

    if (out == nullptr || fd < 0) {
        return sc_status_invalid_argument("sc.gateway_static.read_invalid_argument");
    }
    if (fstat(fd, &st) != 0 || st.st_size < 0 || (uintmax_t)st.st_size > (uintmax_t)max_bytes) {
        return st.st_size > 0 ? sc_status_http("sc.gateway_static.response_too_large")
                              : sc_status_io("sc.gateway_static.stat_failed");
    }
    if (ckd_add(&allocation_size, (size_t)st.st_size, 1U)) {
        return sc_status_invalid_argument("sc.gateway_static.size_overflow");
    }
    text.ptr = sc_alloc(alloc, allocation_size, _Alignof(char));
    if (text.ptr == nullptr) {
        return sc_status_no_memory();
    }
    text.alloc = alloc;
    while (offset < (size_t)st.st_size) {
        ssize_t count = read(fd, text.ptr + offset, (size_t)st.st_size - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            sc_string_clear(&text);
            return sc_status_io("sc.gateway_static.read_failed");
        }
        offset += (size_t)count;
    }
    text.ptr[offset] = '\0';
    text.len = offset;
    *out = text;
    return sc_status_ok();
}

static bool event_visible_to_session(const gateway_event *event, sc_str session_id)
{
    if (event == nullptr) {
        return false;
    }
    if (session_id.len == 0 || event->session_id.len == 0) {
        return true;
    }
    return sc_str_equal(sc_string_as_str(&event->session_id), session_id);
}

static sc_str event_field_session_id(const sc_observer_event *event)
{
    if (event == nullptr) {
        return sc_str_from_cstr("");
    }
    for (size_t i = 0; i < event->field_count; i += 1) {
        if (event->fields[i].key != nullptr && strcmp(event->fields[i].key, "session_id") == 0) {
            return event->fields[i].value;
        }
    }
    return sc_str_from_cstr("");
}

static sc_status gateway_events_push(sc_gateway_server *server, gateway_event *event)
{
    if (server == nullptr || event == nullptr) {
        return sc_status_invalid_argument("sc.gateway_events.invalid_argument");
    }
    if (server->max_sse_events == 0) {
        event_clear(event);
        return sc_status_ok();
    }
    while (server->events.len >= server->max_sse_events) {
        gateway_event *oldest = sc_vec_at(&server->events, 0);
        if (oldest != nullptr) {
            event_clear(oldest);
        }
        if (server->events.len > 1) {
            char *base = server->events.ptr;
            memmove(base,
                    base + server->events.item_size,
                    (server->events.len - 1) * server->events.item_size);
        }
        server->events.len -= 1;
    }
    return sc_vec_push(&server->events, event);
}

static sc_status gateway_observer_emit(void *impl, const sc_observer_event *event)
{
    gateway_observer_impl *observer = impl;
    gateway_event stored = {0};
    sc_status status = sc_status_ok();
    sc_string_builder builder = {0};
    sc_string data = {0};

    if (observer == nullptr || observer->server == nullptr || event == nullptr) {
        return sc_status_invalid_argument("sc.gateway_observer.emit_invalid_argument");
    }
    status = copy_string(observer->server->alloc, event->name, &stored.name);
    if (sc_status_is_ok(status)) {
        status = copy_string(observer->server->alloc, event_field_session_id(event), &stored.session_id);
    }
    sc_string_builder_init(&builder, observer->server->alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < event->field_count; i += 1) {
        sc_str redacted = sc_log_redact_field(&event->fields[i]);
        status = sc_string_builder_append_cstr(&builder, event->fields[i].key == nullptr ? "" : event->fields[i].key);
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "=");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, redacted);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\n");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &data);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(observer->server->alloc, sc_string_as_str(&data), &stored.data);
    }
    if (sc_status_is_ok(status)) {
        status = gateway_events_push(observer->server, &stored);
    }
    if (!sc_status_is_ok(status)) {
        event_clear(&stored);
    }
    sc_string_clear(&data);
    sc_string_builder_clear(&builder);
    return status;
}

static void gateway_observer_destroy(void *impl)
{
    gateway_observer_impl *observer = impl;
    if (observer == nullptr) {
        return;
    }
    sc_free(observer->alloc, observer, sizeof(*observer), _Alignof(gateway_observer_impl));
}

// cppcheck-suppress constParameterPointer
static sc_status gateway_listener_start(sc_gateway_server *server)
{
    if (server == nullptr || !server->listener_enabled || server->transport_kind != SC_GATEWAY_TRANSPORT_EMBEDDED) {
        return sc_status_ok();
    }
    if (server->tls_enabled) {
        return sc_status_unsupported("sc.gateway.listener.tls_unimplemented");
    }
    struct sockaddr_in addr;
    struct sockaddr_storage bound;
    int namelen = (int)sizeof(bound);
    const char *bind = str_equal_cstr(sc_string_as_str(&server->bind), "localhost") ? "127.0.0.1" : server->bind.ptr;
    int rc = uv_loop_init(&server->loop);
    if (rc != 0) {
        return sc_status_io("sc.gateway.listener.loop_init_failed");
    }
    server->loop_initialized = true;
    rc = uv_tcp_init(&server->loop, &server->listener);
    if (rc != 0) {
        gateway_listener_stop(server);
        return sc_status_io("sc.gateway.listener.tcp_init_failed");
    }
    server->listener_initialized = true;
    server->listener.data = server;
    rc = uv_ip4_addr(bind == nullptr ? "127.0.0.1" : bind, server->port, &addr);
    if (rc != 0) {
        gateway_listener_stop(server);
        return sc_status_io("sc.gateway.listener.addr_failed");
    }
    rc = uv_tcp_bind(&server->listener, (const struct sockaddr *)&addr, 0);
    if (rc != 0) {
        gateway_listener_stop(server);
        return sc_status_io("sc.gateway.listener.bind_failed");
    }
    rc = uv_listen((uv_stream_t *)&server->listener, 16, gateway_on_connection);
    if (rc != 0) {
        gateway_listener_stop(server);
        return sc_status_io("sc.gateway.listener.listen_failed");
    }
    if (uv_tcp_getsockname(&server->listener, (struct sockaddr *)&bound, &namelen) == 0 &&
        bound.ss_family == AF_INET) {
        const struct sockaddr_in *in = (const struct sockaddr_in *)&bound;
        server->bound_port = ntohs(in->sin_port);
    } else {
        server->bound_port = server->port;
    }
    server->listener_started = true;
    return sc_status_ok();
}

static void gateway_close_walk_cb(uv_handle_t *handle, void *arg)
{
    sc_gateway_server *server = arg;
    if (handle == nullptr || uv_is_closing(handle)) {
        return;
    }
    if (server != nullptr && handle == (uv_handle_t *)&server->listener) {
        uv_close(handle, nullptr);
    } else {
        uv_close(handle, gateway_client_close_cb);
    }
}

static void gateway_listener_stop(sc_gateway_server *server)
{
    if (server == nullptr) {
        return;
    }
    if (server->loop_initialized) {
        uv_walk(&server->loop, gateway_close_walk_cb, server);
        (void)uv_run(&server->loop, UV_RUN_DEFAULT);
        (void)uv_loop_close(&server->loop);
    }
    server->loop_initialized = false;
    server->listener_initialized = false;
    server->listener_started = false;
    server->bound_port = 0;
}

static void gateway_on_connection(uv_stream_t *listener, int status)
{
    sc_gateway_server *server = listener == nullptr ? nullptr : listener->data;
    gateway_client *client = nullptr;
    int rc;

    if (status < 0 || server == nullptr) {
        return;
    }
    client = sc_alloc(server->alloc, sizeof(*client), _Alignof(gateway_client));
    if (client == nullptr) {
        return;
    }
    *client = (gateway_client){.server = server};
    sc_bytes_init(&client->request, server->alloc);
    rc = uv_tcp_init(&server->loop, &client->handle);
    if (rc != 0) {
        sc_bytes_clear(&client->request);
        sc_free(server->alloc, client, sizeof(*client), _Alignof(gateway_client));
        return;
    }
    client->handle.data = client;
    rc = uv_accept(listener, (uv_stream_t *)&client->handle);
    if (rc != 0) {
        uv_close((uv_handle_t *)&client->handle, gateway_client_close_cb);
        return;
    }
    rc = uv_read_start((uv_stream_t *)&client->handle, gateway_alloc_cb, gateway_read_cb);
    if (rc != 0) {
        uv_close((uv_handle_t *)&client->handle, gateway_client_close_cb);
    }
}

static void gateway_alloc_cb(uv_handle_t *handle, size_t suggested_size, uv_buf_t *buf)
{
    gateway_client *client = handle == nullptr ? nullptr : handle->data;
    if (suggested_size < 4096) {
        suggested_size = 4096;
    }
    if (buf == nullptr || client == nullptr || client->server == nullptr) {
        if (buf != nullptr) {
            *buf = uv_buf_init(nullptr, 0);
        }
        return;
    }
    buf->base = sc_alloc(client->server->alloc, suggested_size, _Alignof(char));
    buf->len = buf->base == nullptr ? 0 : suggested_size;
}

static void gateway_read_cb(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf)
{
    constexpr size_t max_header_bytes = 16U * 1024U;
    gateway_client *client = stream == nullptr ? nullptr : stream->data;
    sc_status status = sc_status_ok();
    sc_string response = {0};
    gateway_write *write_req = nullptr;
    size_t max_request_bytes = 0;
    size_t next_request_bytes = 0;
    bool invalid_request = false;

    if (client == nullptr) {
        return;
    }
    if (nread < 0) {
        gateway_read_buf_free(client, buf);
        uv_close((uv_handle_t *)&client->handle, gateway_client_close_cb);
        return;
    }
    if (nread > 0 && buf != nullptr && buf->base != nullptr) {
        if (ckd_add(&max_request_bytes, client->server->max_body_bytes, max_header_bytes) ||
            ckd_add(&next_request_bytes, client->request.len, (size_t)nread) ||
            next_request_bytes > max_request_bytes) {
            status = sc_status_http("sc.gateway_http.request_too_large");
        } else {
            status = sc_bytes_append(&client->request, sc_buf_from_parts(buf->base, (size_t)nread));
        }
    }
    gateway_read_buf_free(client, buf);
    if (!sc_status_is_ok(status) ||
        !gateway_http_request_complete(sc_str_from_parts((const char *)client->request.ptr, client->request.len),
                                       &invalid_request)) {
        if (!sc_status_is_ok(status) || invalid_request) {
            uv_close((uv_handle_t *)&client->handle, gateway_client_close_cb);
        }
        return;
    }
    status = sc_gateway_handle_http(client->server,
                                    sc_str_from_parts((const char *)client->request.ptr, client->request.len),
                                    client->server->alloc,
                                    &response);
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&response);
        (void)sc_string_from_cstr(client->server->alloc,
                                  "HTTP/1.1 500\r\nContent-Type: text/plain\r\nContent-Length: 15\r\n\r\nlistener error",
                                  &response);
    }
    write_req = sc_alloc(client->server->alloc, sizeof(*write_req), _Alignof(gateway_write));
    if (write_req == nullptr) {
        sc_string_clear(&response);
        uv_close((uv_handle_t *)&client->handle, gateway_client_close_cb);
        return;
    }
    *write_req = (gateway_write){
        .buf = uv_buf_init(response.ptr, (unsigned int)response.len),
        .response = response,
        .client = client,
    };
    if (uv_write(&write_req->req, (uv_stream_t *)&client->handle, &write_req->buf, 1, gateway_write_cb) != 0) {
        sc_string_clear(&write_req->response);
        sc_free(client->server->alloc, write_req, sizeof(*write_req), _Alignof(gateway_write));
        uv_close((uv_handle_t *)&client->handle, gateway_client_close_cb);
    }
}

static void gateway_write_cb(uv_write_t *req, int status)
{
    gateway_write *write_req = (gateway_write *)req;
    (void)status;
    if (write_req == nullptr) {
        return;
    }
    sc_string_clear(&write_req->response);
    if (write_req->client != nullptr) {
        uv_close((uv_handle_t *)&write_req->client->handle, gateway_client_close_cb);
        sc_free(write_req->client->server->alloc, write_req, sizeof(*write_req), _Alignof(gateway_write));
    }
}

static void gateway_read_buf_free(gateway_client *client, const uv_buf_t *buf)
{
    if (client == nullptr || client->server == nullptr || buf == nullptr || buf->base == nullptr) {
        return;
    }
    sc_free(client->server->alloc, buf->base, buf->len, _Alignof(char));
}

static void gateway_client_close_cb(uv_handle_t *handle)
{
    gateway_client *client = handle == nullptr ? nullptr : handle->data;
    if (client == nullptr) {
        return;
    }
    sc_bytes_clear(&client->request);
    sc_free(client->server->alloc, client, sizeof(*client), _Alignof(gateway_client));
}

static bool gateway_http_request_complete(sc_str raw, bool *invalid)
{
    const char *headers_end = gateway_find_header_end(raw);
    size_t header_bytes = 0;
    size_t content_length = 0;
    size_t total = 0;

    if (invalid != nullptr) {
        *invalid = false;
    }

    if (headers_end == nullptr) {
        return false;
    }
    header_bytes = (size_t)(headers_end - raw.ptr) + 4U;
    if (!gateway_http_content_length(raw, &content_length) || ckd_add(&total, header_bytes, content_length)) {
        if (invalid != nullptr) {
            *invalid = true;
        }
        return false;
    }
    return raw.len >= total;
}

static bool gateway_http_content_length(sc_str raw, size_t *out)
{
    static const char header[] = "Content-Length:";
    const char *headers_end = gateway_find_header_end(raw);
    size_t limit = headers_end == nullptr ? raw.len : (size_t)(headers_end - raw.ptr);

    if (out == nullptr) {
        return false;
    }
    *out = 0;
    for (size_t i = 0; i + sizeof(header) - 1 <= limit; i += 1) {
        if (memcmp(raw.ptr + i, header, sizeof(header) - 1) == 0) {
            size_t cursor = i + sizeof(header) - 1;
            size_t value = 0;
            bool saw_digit = false;
            while (cursor < limit && (raw.ptr[cursor] == ' ' || raw.ptr[cursor] == '\t')) {
                cursor += 1;
            }
            while (cursor < limit && raw.ptr[cursor] >= '0' && raw.ptr[cursor] <= '9') {
                size_t next = 0;
                if (ckd_mul(&next, value, 10U) ||
                    ckd_add(&next, next, (size_t)(raw.ptr[cursor] - '0'))) {
                    return false;
                }
                value = next;
                saw_digit = true;
                cursor += 1;
            }
            if (!saw_digit || (cursor < limit && raw.ptr[cursor] != '\r' && raw.ptr[cursor] != ' ' && raw.ptr[cursor] != '\t')) {
                return false;
            }
            *out = value;
            return true;
        }
    }
    return true;
}

static const char *gateway_find_header_end(sc_str raw)
{
    if (raw.ptr == nullptr || raw.len < 4) {
        return nullptr;
    }
    for (size_t i = 0; i + 3 < raw.len; i += 1) {
        if (raw.ptr[i] == '\r' && raw.ptr[i + 1] == '\n' && raw.ptr[i + 2] == '\r' && raw.ptr[i + 3] == '\n') {
            return raw.ptr + i;
        }
    }
    return nullptr;
}

static const char *gateway_find_bytes(sc_str raw, size_t start, const char *needle, size_t needle_len)
{
    if (raw.ptr == nullptr || needle == nullptr || needle_len == 0 || start > raw.len ||
        raw.len - start < needle_len) {
        return nullptr;
    }
    for (size_t i = start; i <= raw.len - needle_len; i += 1) {
        if (memcmp(raw.ptr + i, needle, needle_len) == 0) {
            return raw.ptr + i;
        }
    }
    return nullptr;
}

static bool str_equal_cstr(sc_str value, const char *cstr)
{
    return sc_str_equal(value, sc_str_from_cstr(cstr));
}

static bool str_has_prefix(sc_str value, const char *prefix)
{
    size_t len = prefix == nullptr ? 0 : strlen(prefix);
    return value.ptr != nullptr && value.len >= len && memcmp(value.ptr, prefix, len) == 0;
}

static sc_str trim_header_value(sc_str value)
{
    while (value.len > 0 && (*value.ptr == ' ' || *value.ptr == '\t')) {
        value.ptr += 1;
        value.len -= 1;
    }
    while (value.len > 0 && (value.ptr[value.len - 1] == ' ' || value.ptr[value.len - 1] == '\t')) {
        value.len -= 1;
    }
    return value;
}

static sc_status gateway_generate_auth_token(sc_allocator *alloc, sc_string *out)
{
    static const char hex[] = "0123456789abcdef";
    unsigned char random[32] = {0};
    char token[65] = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.gateway.token_invalid_argument");
    }
    status = sc_random_bytes(random, sizeof(random));
    if (!sc_status_is_ok(status)) {
        sc_secure_zero(random, sizeof(random));
        return status;
    }
    for (size_t i = 0; i < sizeof(random); i += 1) {
        token[i * 2] = hex[random[i] >> 4];
        token[i * 2 + 1] = hex[random[i] & 0x0fU];
    }
    sc_secure_zero(random, sizeof(random));
    return sc_string_from_cstr(alloc, token, out);
}
