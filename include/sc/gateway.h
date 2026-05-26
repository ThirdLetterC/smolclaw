#pragma once

#include "sc/allocator.h"
#include "sc/api.h"
#include "sc/config.h"
#include "sc/observer.h"
#include "sc/result.h"
#include "sc/runtime.h"
#include "sc/string.h"
#include "sc/time.h"
#include "sc/vector.h"

SC_BEGIN_DECLS

typedef struct sc_gateway_server sc_gateway_server;

typedef enum sc_gateway_method {
    SC_GATEWAY_GET = 0,
    SC_GATEWAY_POST,
    SC_GATEWAY_PUT,
    SC_GATEWAY_DELETE
} sc_gateway_method;

typedef enum sc_gateway_session_state {
    SC_GATEWAY_SESSION_IDLE = 0,
    SC_GATEWAY_SESSION_RUNNING,
    SC_GATEWAY_SESSION_ABORTED
} sc_gateway_session_state;

typedef enum sc_gateway_transport_kind {
    SC_GATEWAY_TRANSPORT_EMBEDDED = 0,
    SC_GATEWAY_TRANSPORT_EXTERNAL
} sc_gateway_transport_kind;

typedef enum sc_gateway_auth_mode {
    SC_GATEWAY_AUTH_NONE = 0,
    SC_GATEWAY_AUTH_PAIRING,
    SC_GATEWAY_AUTH_BEARER
} sc_gateway_auth_mode;

typedef enum sc_gateway_capability {
    SC_GATEWAY_CAP_NONE = 0,
    SC_GATEWAY_CAP_STATUS = 1u << 0u,
    SC_GATEWAY_CAP_PAIR = 1u << 1u,
    SC_GATEWAY_CAP_CHAT = 1u << 2u,
    SC_GATEWAY_CAP_EVENTS = 1u << 3u,
    SC_GATEWAY_CAP_SESSIONS = 1u << 4u,
    SC_GATEWAY_CAP_CONFIG = 1u << 5u,
    SC_GATEWAY_CAP_STATIC = 1u << 6u,
    SC_GATEWAY_CAP_ACP = 1u << 7u
} sc_gateway_capability;

typedef struct sc_gateway_request {
    size_t struct_size;
    sc_gateway_method method;
    sc_str path;
    sc_str body;
    sc_str auth_token;
    sc_str idempotency_key;
    sc_str request_id;
    sc_str session_id;
    sc_str origin;
} sc_gateway_request;

typedef struct sc_gateway_response {
    size_t struct_size;
    int status;
    sc_string body;
    sc_string content_type;
} sc_gateway_response;

typedef struct sc_gateway_static_root {
    size_t struct_size;
    sc_str url_prefix;
    sc_str filesystem_root;
} sc_gateway_static_root;

typedef struct sc_gateway_route_descriptor {
    size_t struct_size;
    sc_gateway_method method;
    const char *path;
    sc_gateway_auth_mode auth_mode;
    uint64_t required_capability;
    size_t request_size_limit;
    int64_t timeout_ms;
    const char *rate_limit_bucket;
    const char *input_schema_ref;
    const char *output_schema_ref;
    const char *handler_name;
} sc_gateway_route_descriptor;

typedef struct sc_gateway_options {
    size_t struct_size;
    sc_agent *agent;
    sc_config *config;
    sc_str bind;
    uint16_t port;
    bool public_bind_enabled;
    sc_str pairing_code;
    sc_str auth_token;
    size_t max_body_bytes;
    size_t rate_limit;
    int64_t timeout_ms;
    uint64_t pairing_ttl_secs;
    size_t pairing_attempt_limit;
    sc_str pairing_origin;
    size_t max_sse_events;
    size_t max_ws_message_bytes;
    size_t max_ws_queued_messages;
    const sc_gateway_static_root *static_roots;
    size_t static_root_count;
} sc_gateway_options;

typedef struct sc_gateway_transport_options {
    size_t struct_size;
    sc_gateway_transport_kind kind;
    bool listener_enabled;
    uint16_t listen_port;
    bool tls_enabled;
    sc_str tls_cert_path;
    sc_str tls_key_path;
    bool websocket_enabled;
    bool jsonrpc_enabled;
} sc_gateway_transport_options;

typedef struct sc_gateway_session {
    size_t struct_size;
    sc_string id;
    sc_gateway_session_state state;
    bool abort_requested;
    sc_wall_time started_at;
    sc_wall_time ended_at;
    sc_string last_output;
} sc_gateway_session;

sc_status sc_gateway_server_new(sc_allocator *alloc,
                                const sc_gateway_options *options,
                                sc_gateway_server **out);
sc_status sc_gateway_server_start(sc_gateway_server *server);
sc_status sc_gateway_server_poll(sc_gateway_server *server, uint32_t timeout_ms);
sc_status sc_gateway_server_stop(sc_gateway_server *server);
bool sc_gateway_server_running(const sc_gateway_server *server);
uint16_t sc_gateway_server_bound_port(const sc_gateway_server *server);
void sc_gateway_server_destroy(sc_gateway_server *server);

sc_status sc_gateway_handle_request(sc_gateway_server *server,
                                    const sc_gateway_request *request,
                                    sc_allocator *alloc,
                                    sc_gateway_response *out);
sc_status sc_gateway_transport_configure(sc_gateway_server *server,
                                         const sc_gateway_transport_options *options);
sc_status sc_gateway_handle_http(sc_gateway_server *server,
                                 sc_str raw_request,
                                 sc_allocator *alloc,
                                 sc_string *out_raw_response);
sc_status sc_gateway_websocket_receive_text(sc_gateway_server *server,
                                            sc_str text,
                                            sc_str auth_token,
                                            sc_str session_id,
                                            sc_allocator *alloc,
                                            sc_string *out_text);
sc_status sc_gateway_jsonrpc_handle(sc_gateway_server *server,
                                    sc_str request_json,
                                    sc_allocator *alloc,
                                    sc_string *out_response_json);
void sc_gateway_response_clear(sc_gateway_response *response);

sc_status sc_gateway_observer_new(sc_allocator *alloc, sc_gateway_server *server, sc_observer **out);
sc_status sc_gateway_session_abort(sc_gateway_server *server, sc_str session_id);
sc_status sc_gateway_session_disconnect(sc_gateway_server *server, sc_str session_id);
const sc_gateway_session *sc_gateway_session_find(const sc_gateway_server *server, sc_str session_id);
const sc_gateway_route_descriptor *sc_gateway_route_descriptors(size_t *count);

SC_END_DECLS
