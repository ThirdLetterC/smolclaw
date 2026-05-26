// cppcheck-suppress-file redundantInitialization
#include "sc/acp.h"

#include "sc/runtime.h"
#include "sc/vector.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef SC_HAVE_JSONRPC
#include "jsonrpc/jsonrpc.h"
#endif

typedef struct acp_session {
    sc_string id;
    sc_string cwd;
    sc_string system_prompt;
    time_t last_used_at;
} acp_session;

struct sc_acp_server {
    sc_allocator *alloc;
    sc_agent *agent;
    sc_string default_model;
    sc_vec sessions;
    size_t max_sessions;
    uint64_t idle_timeout_secs;
    uint64_t next_session;
    bool approval_requests_enabled;
};

#ifdef SC_HAVE_JSONRPC
typedef struct acp_call_context {
    sc_acp_server *server;
    sc_allocator *alloc;
    sc_string_builder *builder;
    sc_status status;
} acp_call_context;
#endif

static sc_status acp_copy_string(sc_allocator *alloc, sc_str value, sc_string *out);
static void acp_session_clear(acp_session *session);
static acp_session *acp_session_find(sc_acp_server *server, sc_str id);
static sc_status acp_session_remove(sc_acp_server *server, sc_str id);
static void acp_evict_idle_sessions(sc_acp_server *server);

#ifdef SC_HAVE_JSONRPC
static bool acp_transport_send_raw(jsonrpc_transport_t *self, const uint8_t *data, size_t len);
static void acp_transport_close(jsonrpc_transport_t *self);
static bool acp_on_request(jsonrpc_conn_t *conn, const char *method, const JSON_Value *params, jsonrpc_response_t *response);
static JSON_Value *acp_initialize_result(const sc_acp_server *server);
static bool acp_handle_session_new(acp_call_context *ctx, const JSON_Value *params, jsonrpc_response_t *response);
static bool acp_handle_session_prompt(acp_call_context *ctx, const JSON_Value *params, jsonrpc_response_t *response);
static bool acp_handle_session_stop(acp_call_context *ctx, const JSON_Value *params, jsonrpc_response_t *response);
static sc_status acp_session_id_new(sc_acp_server *server, sc_string *out);
static JSON_Value *acp_bool_result(const char *name, bool value);
static const char *acp_json_string_param(const JSON_Value *params, const char *name);
static sc_status acp_emit_update(acp_call_context *ctx, sc_str session_id, sc_str kind, sc_str name, sc_str text, bool finished);
static sc_status acp_emit_result_events(acp_call_context *ctx, sc_str session_id, const sc_agent_turn_result *result);
static bool acp_result_has_approval_request(const sc_agent_turn_result *result);
static sc_status acp_append_json_string(sc_string_builder *builder, sc_str value);
static sc_status acp_append_json_field(sc_string_builder *builder, const char *name, sc_str value, bool *first);
#endif

sc_status sc_acp_server_new(sc_allocator *alloc, const sc_acp_options *options, sc_acp_server **out)
{
    sc_acp_server *server = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr || options == nullptr) {
        return sc_status_invalid_argument("sc.acp.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    server = sc_alloc(alloc, sizeof(*server), _Alignof(sc_acp_server));
    if (server == nullptr) {
        return sc_status_no_memory();
    }
    *server = (sc_acp_server){
        .alloc = alloc,
        .agent = options->agent,
        .max_sessions = options->max_sessions == 0 ? 16 : options->max_sessions,
        .idle_timeout_secs = options->idle_timeout_secs == 0 ? 3'600 : options->idle_timeout_secs,
        .next_session = 1,
        .approval_requests_enabled = options->approval_requests_enabled,
    };
    sc_vec_init(&server->sessions, alloc, sizeof(acp_session));
    status = acp_copy_string(alloc, options->default_model, &server->default_model);
    if (!sc_status_is_ok(status)) {
        sc_acp_server_destroy(server);
        return status;
    }

    *out = server;
    return sc_status_ok();
}

sc_status sc_acp_handle_line(sc_acp_server *server, sc_str line, sc_allocator *alloc, sc_string *out)
{
#ifdef SC_HAVE_JSONRPC
    sc_string_builder builder = {0};
    acp_call_context ctx = {0};
    jsonrpc_conn_t *conn = nullptr;
    jsonrpc_transport_t transport = {0};
    jsonrpc_callbacks_t callbacks = {0};
    sc_status status = sc_status_ok();
    const uint8_t newline = (uint8_t)'\n';

    if (server == nullptr || out == nullptr || line.ptr == nullptr || line.len == 0) {
        return sc_status_invalid_argument("sc.acp.line_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    acp_evict_idle_sessions(server);

    sc_string_builder_init(&builder, alloc);
    ctx = (acp_call_context){
        .server = server,
        .alloc = alloc,
        .builder = &builder,
        .status = sc_status_ok(),
    };
    transport = (jsonrpc_transport_t){
        .user_data = &ctx,
        .send_raw = acp_transport_send_raw,
        .close = acp_transport_close,
    };
    callbacks = (jsonrpc_callbacks_t){
        .on_request = acp_on_request,
    };
    conn = jsonrpc_conn_new(transport, callbacks, &ctx);
    if (conn == nullptr) {
        sc_string_builder_clear(&builder);
        return sc_status_no_memory();
    }

    jsonrpc_conn_feed(conn, (const uint8_t *)line.ptr, line.len);
    if (line.ptr[line.len - 1] != '\n') {
        jsonrpc_conn_feed(conn, &newline, 1);
    }
    jsonrpc_conn_free(conn);

    status = ctx.status;
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
        return status;
    }
    return sc_string_builder_finish(&builder, out);
#else
    (void)server;
    (void)line;
    (void)alloc;
    (void)out;
    return sc_status_unsupported("sc.acp.jsonrpc_unavailable");
#endif
}

size_t sc_acp_session_count(const sc_acp_server *server)
{
    return server == nullptr ? 0 : server->sessions.len;
}

void sc_acp_server_destroy(sc_acp_server *server)
{
    if (server == nullptr) {
        return;
    }
    for (size_t i = 0; i < server->sessions.len; i += 1) {
        acp_session *session = sc_vec_at(&server->sessions, i);
        acp_session_clear(session);
    }
    sc_vec_clear(&server->sessions);
    sc_string_clear(&server->default_model);
    sc_free(server->alloc, server, sizeof(*server), _Alignof(sc_acp_server));
}

static sc_status acp_copy_string(sc_allocator *alloc, sc_str value, sc_string *out)
{
    return sc_string_from_str(alloc, value.ptr == nullptr ? sc_str_from_cstr("") : value, out);
}

static void acp_session_clear(acp_session *session)
{
    if (session == nullptr) {
        return;
    }
    sc_string_clear(&session->id);
    sc_string_clear(&session->cwd);
    sc_string_clear(&session->system_prompt);
    *session = (acp_session){0};
}

static acp_session *acp_session_find(sc_acp_server *server, sc_str id)
{
    if (server == nullptr || id.len == 0) {
        return nullptr;
    }
    for (size_t i = 0; i < server->sessions.len; i += 1) {
        acp_session *session = sc_vec_at(&server->sessions, i);
        if (session != nullptr && sc_str_equal(sc_string_as_str(&session->id), id)) {
            return session;
        }
    }
    return nullptr;
}

static sc_status acp_session_remove(sc_acp_server *server, sc_str id)
{
    if (server == nullptr || id.len == 0) {
        return sc_status_invalid_argument("sc.acp.session_remove.invalid_argument");
    }
    for (size_t i = 0; i < server->sessions.len; i += 1) {
        acp_session *session = sc_vec_at(&server->sessions, i);
        if (session != nullptr && sc_str_equal(sc_string_as_str(&session->id), id)) {
            acp_session_clear(session);
            if (i + 1 < server->sessions.len) {
                char *base = server->sessions.ptr;
                size_t item_size = server->sessions.item_size;
                (void)memmove(base + (i * item_size),
                              base + ((i + 1) * item_size),
                              (server->sessions.len - i - 1) * item_size);
            }
            server->sessions.len -= 1;
            return sc_status_ok();
        }
    }
    return sc_status_invalid_argument("sc.acp.session.not_found");
}

static void acp_evict_idle_sessions(sc_acp_server *server)
{
    time_t now;
    if (server == nullptr || server->idle_timeout_secs == 0) {
        return;
    }
    now = time(nullptr);
    for (size_t i = 0; i < server->sessions.len;) {
        acp_session *session = sc_vec_at(&server->sessions, i);
        bool expired = false;
        if (session != nullptr && now >= session->last_used_at) {
            expired = (uint64_t)(now - session->last_used_at) > server->idle_timeout_secs;
        }
        if (expired) {
            sc_status status = acp_session_remove(server, sc_string_as_str(&session->id));
            sc_status_clear(&status);
        } else {
            i += 1;
        }
    }
}

#ifdef SC_HAVE_JSONRPC
static bool acp_transport_send_raw(jsonrpc_transport_t *self, const uint8_t *data, size_t len)
{
    acp_call_context *ctx = self == nullptr ? nullptr : self->user_data;
    if (ctx == nullptr || ctx->builder == nullptr || (data == nullptr && len > 0)) {
        return false;
    }
    if (!sc_status_is_ok(ctx->status)) {
        return false;
    }
    ctx->status = sc_string_builder_append(ctx->builder, sc_str_from_parts((const char *)data, len));
    return sc_status_is_ok(ctx->status);
}

static void acp_transport_close(jsonrpc_transport_t *self)
{
    (void)self;
}

static bool acp_on_request(jsonrpc_conn_t *conn, const char *method, const JSON_Value *params, jsonrpc_response_t *response)
{
    acp_call_context *ctx = jsonrpc_conn_get_context(conn);

    if (ctx == nullptr || method == nullptr || response == nullptr) {
        return false;
    }
    acp_evict_idle_sessions(ctx->server);
    if (strcmp(method, "initialize") == 0) {
        response->result = acp_initialize_result(ctx->server);
        response->error_code = response->result == nullptr ? -32603 : 0;
        return true;
    }
    if (strcmp(method, "session/new") == 0) {
        return acp_handle_session_new(ctx, params, response);
    }
    if (strcmp(method, "session/prompt") == 0) {
        return acp_handle_session_prompt(ctx, params, response);
    }
    if (strcmp(method, "session/stop") == 0) {
        return acp_handle_session_stop(ctx, params, response);
    }
    return false;
}

static JSON_Value *acp_initialize_result(const sc_acp_server *server)
{
    JSON_Value *value = json_value_init_object();
    JSON_Value *caps = nullptr;
    JSON_Object *object = value == nullptr ? nullptr : json_value_get_object(value);
    JSON_Object *caps_object = nullptr;

    if (object == nullptr || server == nullptr) {
        json_value_free(value);
        return nullptr;
    }
    caps = json_value_init_object();
    caps_object = caps == nullptr ? nullptr : json_value_get_object(caps);
    if (caps_object == nullptr) {
        json_value_free(caps);
        json_value_free(value);
        return nullptr;
    }
    if (json_object_set_string(object, "protocolVersion", "0.1") != JSONSuccess ||
        json_object_set_number(object, "maxSessions", (double)server->max_sessions) != JSONSuccess ||
        json_object_set_number(object, "idleTimeoutSecs", (double)server->idle_timeout_secs) != JSONSuccess ||
        json_object_set_boolean(caps_object, "sessionUpdate", true) != JSONSuccess ||
        json_object_set_boolean(caps_object, "text", true) != JSONSuccess ||
        json_object_set_boolean(caps_object, "reasoning", true) != JSONSuccess ||
        json_object_set_boolean(caps_object, "toolCalls", true) != JSONSuccess ||
        json_object_set_boolean(caps_object, "toolResults", true) != JSONSuccess ||
        json_object_set_boolean(caps_object, "final", true) != JSONSuccess ||
        json_object_set_boolean(caps_object, "approvalRequests", server->approval_requests_enabled) != JSONSuccess ||
        json_object_set_value(object, "capabilities", caps) != JSONSuccess) {
        json_value_free(caps);
        json_value_free(value);
        return nullptr;
    }
    caps = nullptr;
    if (server->default_model.len > 0 &&
        json_object_set_string_with_len(object, "defaultModel", server->default_model.ptr, server->default_model.len) != JSONSuccess) {
        json_value_free(value);
        return nullptr;
    }
    return value;
}

static bool acp_handle_session_new(acp_call_context *ctx, const JSON_Value *params, jsonrpc_response_t *response)
{
    const char *cwd = acp_json_string_param(params, "cwd");
    const char *system_prompt = acp_json_string_param(params, "systemPrompt");
    acp_session session = {0};
    JSON_Value *result = nullptr;
    JSON_Object *result_object = nullptr;
    sc_status status = sc_status_ok();

    if (ctx == nullptr || response == nullptr) {
        return false;
    }
    if (ctx->server->sessions.len >= ctx->server->max_sessions) {
        response->error_code = -32001;
        response->error_message = "ACP session limit reached";
        return true;
    }
    status = acp_session_id_new(ctx->server, &session.id);
    if (sc_status_is_ok(status)) {
        status = acp_copy_string(ctx->server->alloc, sc_str_from_cstr(cwd == nullptr ? "" : cwd), &session.cwd);
    }
    if (sc_status_is_ok(status)) {
        status = acp_copy_string(ctx->server->alloc, sc_str_from_cstr(system_prompt == nullptr ? "" : system_prompt), &session.system_prompt);
    }
    if (sc_status_is_ok(status)) {
        session.last_used_at = time(nullptr);
        status = sc_vec_push(&ctx->server->sessions, &session);
    }
    if (!sc_status_is_ok(status)) {
        acp_session_clear(&session);
        response->error_code = status.code == SC_ERR_NO_MEMORY ? -32603 : -32602;
        response->error_message = status.error_key;
        sc_status_clear(&status);
        return true;
    }
    result = json_value_init_object();
    result_object = result == nullptr ? nullptr : json_value_get_object(result);
    if (result_object == nullptr ||
        json_object_set_string_with_len(result_object,
                                        "sessionId",
                                        session.id.ptr,
                                        session.id.len) != JSONSuccess) {
        json_value_free(result);
        response->error_code = -32603;
        return true;
    }
    response->result = result;
    return true;
}

static bool acp_handle_session_prompt(acp_call_context *ctx, const JSON_Value *params, jsonrpc_response_t *response)
{
    const char *session_id_text = acp_json_string_param(params, "sessionId");
    const char *prompt_text = acp_json_string_param(params, "prompt");
    sc_str session_id = sc_str_from_cstr(session_id_text == nullptr ? "" : session_id_text);
    sc_str prompt = sc_str_from_cstr(prompt_text == nullptr ? "" : prompt_text);
    acp_session *session = nullptr;
    sc_agent_turn_result result = {0};
    bool approval_pending = false;
    sc_status status = sc_status_ok();

    if (ctx == nullptr || response == nullptr) {
        return false;
    }
    if (session_id.len == 0 || prompt.len == 0) {
        response->error_code = -32602;
        response->error_message = "sessionId and prompt are required";
        return true;
    }
    session = acp_session_find(ctx->server, session_id);
    if (session == nullptr) {
        response->error_code = -32002;
        response->error_message = "ACP session not found";
        return true;
    }
    session->last_used_at = time(nullptr);
    status = acp_emit_update(ctx,
                             sc_string_as_str(&session->id),
                             sc_str_from_cstr("reasoning"),
                             sc_str_from_cstr("request"),
                             sc_str_from_cstr("accepted"),
                             false);
    if (sc_status_is_ok(status) && ctx->server->agent != nullptr) {
        sc_turn turn = {
            .struct_size = sizeof(turn),
            .input = prompt,
            .session_id = sc_string_as_str(&session->id),
            .use_streaming = true,
            .emit_stream_deltas = true,
        };
        status = sc_agent_process_message(ctx->server->agent, &turn, ctx->alloc, &result);
        if (sc_status_is_ok(status)) {
            status = acp_emit_result_events(ctx, sc_string_as_str(&session->id), &result);
        } else if (status.code == SC_ERR_CANCELLED && acp_result_has_approval_request(&result)) {
            sc_status_clear(&status);
            status = acp_emit_result_events(ctx, sc_string_as_str(&session->id), &result);
            approval_pending = sc_status_is_ok(status);
        }
    } else if (sc_status_is_ok(status)) {
        status = acp_emit_update(ctx,
                                 sc_string_as_str(&session->id),
                                 sc_str_from_cstr("text"),
                                 sc_str_from_cstr("assistant"),
                                 prompt,
                                 false);
        if (sc_status_is_ok(status)) {
            status = acp_emit_update(ctx,
                                     sc_string_as_str(&session->id),
                                     sc_str_from_cstr("final"),
                                     sc_str_from_cstr("assistant"),
                                     prompt,
                                     true);
        }
    }

    sc_agent_turn_result_clear(&result);
    if (!sc_status_is_ok(status)) {
        response->error_code = status.code == SC_ERR_NO_MEMORY ? -32603 : -32003;
        response->error_message = status.error_key;
        sc_status_clear(&status);
        return true;
    }
    response->result = acp_bool_result("finished", !approval_pending);
    if (response->result == nullptr) {
        response->error_code = -32603;
    }
    return true;
}

static bool acp_handle_session_stop(acp_call_context *ctx, const JSON_Value *params, jsonrpc_response_t *response)
{
    const char *session_id_text = acp_json_string_param(params, "sessionId");
    sc_status status = sc_status_ok();

    if (ctx == nullptr || response == nullptr || session_id_text == nullptr || session_id_text[0] == '\0') {
        if (response != nullptr) {
            response->error_code = -32602;
            response->error_message = "sessionId is required";
        }
        return true;
    }
    status = acp_session_remove(ctx->server, sc_str_from_cstr(session_id_text));
    if (!sc_status_is_ok(status)) {
        response->error_code = -32002;
        response->error_message = "ACP session not found";
        sc_status_clear(&status);
        return true;
    }
    response->result = acp_bool_result("stopped", true);
    if (response->result == nullptr) {
        response->error_code = -32603;
    }
    return true;
}

static sc_status acp_session_id_new(sc_acp_server *server, sc_string *out)
{
    char id[40] = {0};
    if (server == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.acp.session_id.invalid_argument");
    }
    (void)snprintf(id, sizeof(id), "acp-%llu", (unsigned long long)server->next_session);
    server->next_session += 1;
    return sc_string_from_cstr(server->alloc, id, out);
}

static JSON_Value *acp_bool_result(const char *name, bool value)
{
    JSON_Value *result = json_value_init_object();
    JSON_Object *object = result == nullptr ? nullptr : json_value_get_object(result);
    if (object == nullptr || json_object_set_boolean(object, name, value) != JSONSuccess) {
        json_value_free(result);
        return nullptr;
    }
    return result;
}

static const char *acp_json_string_param(const JSON_Value *params, const char *name)
{
    const JSON_Object *object = params == nullptr ? nullptr : json_value_get_object(params);
    if (object == nullptr || name == nullptr) {
        return nullptr;
    }
    return json_object_get_string(object, name);
}

static sc_status acp_emit_update(acp_call_context *ctx, sc_str session_id, sc_str kind, sc_str name, sc_str text, bool finished)
{
    bool first = true;
    sc_status status = sc_status_ok();

    if (ctx == nullptr || ctx->builder == nullptr) {
        return sc_status_invalid_argument("sc.acp.update.invalid_argument");
    }
    status = sc_string_builder_append_cstr(ctx->builder, "{\"jsonrpc\":\"2.0\",\"method\":\"session/update\",\"params\":{");
    if (sc_status_is_ok(status)) {
        status = acp_append_json_field(ctx->builder, "sessionId", session_id, &first);
    }
    if (sc_status_is_ok(status)) {
        status = acp_append_json_field(ctx->builder, "kind", kind, &first);
    }
    if (sc_status_is_ok(status) && name.len > 0) {
        status = acp_append_json_field(ctx->builder, "name", name, &first);
    }
    if (sc_status_is_ok(status) && text.len > 0) {
        status = acp_append_json_field(ctx->builder, "text", text, &first);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(ctx->builder, first ? "\"finished\":" : ",\"finished\":");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(ctx->builder, finished ? "true" : "false");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(ctx->builder, "}}\n");
    }
    return status;
}

static sc_status acp_emit_result_events(acp_call_context *ctx, sc_str session_id, const sc_agent_turn_result *result)
{
    sc_status status = sc_status_ok();
    bool text_emitted = false;
    bool approval_requested = false;

    if (ctx == nullptr || result == nullptr) {
        return sc_status_invalid_argument("sc.acp.result_events.invalid_argument");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < result->events.len; i += 1) {
        const sc_turn_event *event = sc_vec_at_const(&result->events, i);
        if (event == nullptr) {
            continue;
        }
        switch (event->type) {
        case SC_TURN_EVENT_TEXT_DELTA:
            text_emitted = true;
            status = acp_emit_update(ctx, session_id, sc_str_from_cstr("text"), sc_string_as_str(&event->name), sc_string_as_str(&event->message), false);
            break;
        case SC_TURN_EVENT_TOOL_CALLED:
            status = acp_emit_update(ctx, session_id, sc_str_from_cstr("tool_call"), sc_string_as_str(&event->name), sc_string_as_str(&event->message), false);
            break;
        case SC_TURN_EVENT_TOOL_RESULT:
            status = acp_emit_update(ctx, session_id, sc_str_from_cstr("tool_result"), sc_string_as_str(&event->name), sc_string_as_str(&event->message), false);
            break;
        case SC_TURN_EVENT_TOOL_DENIED:
            if (ctx->server->approval_requests_enabled) {
                approval_requested = true;
                status = acp_emit_update(ctx,
                                         session_id,
                                         sc_str_from_cstr("approval_request"),
                                         sc_string_as_str(&event->name),
                                         sc_string_as_str(&event->message),
                                         false);
            }
            break;
        case SC_TURN_EVENT_PROMPT_BUILT:
        case SC_TURN_EVENT_PROVIDER_STARTED:
        case SC_TURN_EVENT_PROVIDER_CALLED:
            status = acp_emit_update(ctx, session_id, sc_str_from_cstr("reasoning"), sc_string_as_str(&event->name), sc_string_as_str(&event->message), false);
            break;
        default:
            break;
        }
    }
    if (sc_status_is_ok(status) && approval_requested && result->output.len == 0) {
        return status;
    }
    if (sc_status_is_ok(status) && !text_emitted && result->output.len > 0) {
        status = acp_emit_update(ctx,
                                 session_id,
                                 sc_str_from_cstr("text"),
                                 sc_str_from_cstr("assistant"),
                                 sc_string_as_str(&result->output),
                                 false);
    }
    if (sc_status_is_ok(status)) {
        status = acp_emit_update(ctx,
                                 session_id,
                                 sc_str_from_cstr("final"),
                                 sc_str_from_cstr("assistant"),
                                 sc_string_as_str(&result->output),
                                 true);
    }
    return status;
}

static bool acp_result_has_approval_request(const sc_agent_turn_result *result)
{
    if (result == nullptr) {
        return false;
    }
    for (size_t i = 0; i < result->events.len; i += 1) {
        const sc_turn_event *event = sc_vec_at_const(&result->events, i);
        if (event != nullptr && event->type == SC_TURN_EVENT_TOOL_DENIED &&
            sc_str_equal(sc_string_as_str(&event->message), sc_str_from_cstr("sc.tool.approval_required"))) {
            return true;
        }
    }
    return false;
}

static sc_status acp_append_json_string(sc_string_builder *builder, sc_str value)
{
    sc_status status = sc_string_builder_append_cstr(builder, "\"");
    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        unsigned char ch = (unsigned char)value.ptr[i];
        if (ch == '"' || ch == '\\') {
            char escaped[2] = {'\\', (char)ch};
            status = sc_string_builder_append(builder, sc_str_from_parts(escaped, sizeof(escaped)));
        } else if (ch == '\n') {
            status = sc_string_builder_append_cstr(builder, "\\n");
        } else if (ch == '\r') {
            status = sc_string_builder_append_cstr(builder, "\\r");
        } else if (ch == '\t') {
            status = sc_string_builder_append_cstr(builder, "\\t");
        } else if (ch < 0x20U) {
            char escaped[7] = {0};
            (void)snprintf(escaped, sizeof(escaped), "\\u%04x", ch);
            status = sc_string_builder_append_cstr(builder, escaped);
        } else {
            char plain = (char)ch;
            status = sc_string_builder_append(builder, sc_str_from_parts(&plain, 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    return status;
}

static sc_status acp_append_json_field(sc_string_builder *builder, const char *name, sc_str value, bool *first)
{
    sc_status status = sc_status_ok();
    if (builder == nullptr || name == nullptr || first == nullptr) {
        return sc_status_invalid_argument("sc.acp.json_field.invalid_argument");
    }
    status = sc_string_builder_append_cstr(builder, *first ? "" : ",");
    if (sc_status_is_ok(status)) {
        status = acp_append_json_string(builder, sc_str_from_cstr(name));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, ":");
    }
    if (sc_status_is_ok(status)) {
        status = acp_append_json_string(builder, value);
    }
    if (sc_status_is_ok(status)) {
        *first = false;
    }
    return status;
}
#endif
