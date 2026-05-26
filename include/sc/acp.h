#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sc/allocator.h"
#include "sc/api.h"
#include "sc/result.h"
#include "sc/string.h"

SC_BEGIN_DECLS

typedef struct sc_agent sc_agent;
typedef struct sc_acp_server sc_acp_server;

typedef struct sc_acp_options {
    size_t struct_size;
    sc_agent *agent;
    sc_str default_model;
    size_t max_sessions;
    uint64_t idle_timeout_secs;
    bool approval_requests_enabled;
} sc_acp_options;

sc_status sc_acp_server_new(sc_allocator *alloc, const sc_acp_options *options, sc_acp_server **out);

/*
 * Handles one newline-delimited JSON-RPC 2.0 request. The returned buffer owns
 * all response and session/update notification frames emitted for that request.
 */
sc_status sc_acp_handle_line(sc_acp_server *server, sc_str line, sc_allocator *alloc, sc_string *out);

size_t sc_acp_session_count(const sc_acp_server *server);
void sc_acp_server_destroy(sc_acp_server *server);

SC_END_DECLS
