#pragma once

#include "sc/async.h"
#include "sc/buffer.h"
#include "sc/result.h"
#include "sc/string.h"

typedef struct sc_cancel_token sc_cancel_token;
typedef struct sc_http_client sc_http_client;
typedef struct sc_http_op sc_http_op;

typedef sc_status (*sc_http_chunk_fn)(void *user_data, sc_buf chunk);

typedef struct sc_http_header {
    sc_str name;
    sc_str value;
} sc_http_header;

typedef struct sc_http_request {
    size_t struct_size;
    sc_str method;
    sc_str url;
    const sc_http_header *headers;
    size_t header_count;
    sc_str body;
    sc_str user_agent;
    size_t max_response_bytes;
    int64_t timeout_ms;
    int64_t connect_timeout_ms;
    long http_version;
    bool follow_location;
    bool allow_private_network;
    sc_str aws_sigv4;
    sc_str username;
    sc_str password;
    const sc_cancel_token *cancel_token;
    sc_http_chunk_fn on_chunk;
    void *chunk_user_data;
} sc_http_request;

typedef struct sc_http_response {
    size_t struct_size;
    sc_bytes body;
    long http_status;
    int curl_code;
    bool too_large;
} sc_http_response;

typedef void (*sc_http_complete_fn)(void *user_data,
                                    const sc_http_response *response,
                                    sc_status status);

sc_status sc_http_client_new(sc_allocator *alloc, void *backend_loop, sc_http_client **out);
sc_status sc_http_client_perform(sc_http_client *client,
                                 const sc_http_request *request,
                                 sc_allocator *response_alloc,
                                 sc_http_complete_fn complete,
                                 void *user_data,
                                 sc_http_op **out);
sc_status sc_http_client_perform_sync(sc_allocator *alloc,
                                      const sc_http_request *request,
                                      sc_http_response *out);
sc_status sc_async_context_http_client(sc_async_context *context, sc_http_client **out);
const char *sc_http_curl_strerror(int curl_code);
void sc_http_response_clear(sc_http_response *response);
void sc_http_op_cancel(sc_http_op *op);
void sc_http_op_destroy(sc_http_op *op);
void sc_http_client_destroy(sc_http_client *client);
