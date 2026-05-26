#pragma once

#include "tools/tool_internal.h"

#include "net/http_client.h"

typedef enum http_tool_kind {
    HTTP_TOOL_REQUEST = 0,
    HTTP_TOOL_WEB_SEARCH
} http_tool_kind;

typedef struct http_tool {
    sc_tool_impl_context base;
    http_tool_kind kind;
} http_tool;

typedef struct http_tool_async_state {
    sc_allocator *alloc;
    http_tool *tool;
    sc_tool_invoke_complete_fn complete;
    void *complete_user_data;
    sc_string tool_name;
    sc_string args_summary;
    sc_string method;
    sc_string url;
    sc_string body;
    sc_vec headers;
    sc_http_op *op;
} http_tool_async_state;


void http_headers_clear(sc_vec *headers);
sc_status http_headers_from_json(sc_allocator *alloc, sc_str headers_json, sc_vec *out);
sc_status http_tool_schedule_async(http_tool_async_state *state,
                                   sc_async_context *context,
                                   sc_str method,
                                   sc_str url,
                                   sc_str headers_json,
                                   sc_str body,
                                   size_t max_bytes,
                                   int64_t timeout_ms);
void http_tool_async_state_destroy(http_tool_async_state *state);
