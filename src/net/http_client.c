#include "net/http_client.h"

#include "sc/api.h"
#include "sc/runtime.h"

#include "net/curl_global.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(SC_HAVE_ASYNC_HTTP)
#include <curl/curl.h>
#include <uv.h>
#endif

#if defined(SC_HAVE_ASYNC_HTTP)
typedef struct http_socket_context {
    sc_http_client *client;
    uv_poll_t poll;
    curl_socket_t socket;
    bool closing;
} http_socket_context;
#endif

struct sc_http_client {
    sc_allocator *alloc;
#if defined(SC_HAVE_ASYNC_HTTP)
    uv_loop_t *loop;
    uv_loop_t owned_loop;
    bool owns_loop;
    bool loop_initialized;
    CURLM *multi;
    uv_timer_t timer;
    bool timer_initialized;
    size_t active_ops;
#endif
};

struct sc_http_op {
#if defined(SC_HAVE_ASYNC_HTTP)
    sc_allocator *alloc;
    sc_http_client *client;
    sc_http_complete_fn complete;
    void *user_data;
    sc_http_response response;
    sc_status status;
    sc_http_chunk_fn on_chunk;
    void *chunk_user_data;
    const sc_cancel_token *cancel_token;
    size_t max_response_bytes;
    bool completed;
    bool cancelled;
    CURL *easy;
    struct curl_slist *headers;
    char error_buffer[CURL_ERROR_SIZE];
    bool multi_added;
#else
    // cppcheck-suppress unusedStructMember
    unsigned char placeholder;
#endif
};

#if defined(SC_HAVE_ASYNC_HTTP)
static sc_status http_curl_global_init(void);
static sc_status http_request_validate(const sc_http_request *request);
static sc_status http_build_headers(sc_allocator *alloc, const sc_http_request *request, struct curl_slist **out);
static sc_status http_append_header(sc_allocator *alloc, struct curl_slist **headers, sc_str name, sc_str value);
static size_t http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata);
static int http_progress_cb(void *userdata, curl_off_t download_total, curl_off_t download_now, curl_off_t upload_total, curl_off_t upload_now);
static int http_socket_cb(CURL *easy, curl_socket_t socket, int action, void *user_data, void *socket_user_data);
static int http_timer_cb(CURLM *multi, long timeout_ms, void *user_data);
static void http_poll_cb(uv_poll_t *poll, int status, int events);
static void http_timer_fire_cb(uv_timer_t *timer);
static void http_socket_close_cb(uv_handle_t *handle);
static void http_socket_context_destroy(http_socket_context *context);
static void http_check_multi_info(sc_http_client *client);
static void http_complete_op(sc_http_op *op, sc_status status);
static void http_cleanup_easy(sc_http_op *op);
static sc_status http_curlm_status(CURLMcode code, const char *error_key);
static const char *http_ca_bundle_path(void);
#endif

sc_status sc_http_client_new(sc_allocator *alloc, void *backend_loop, sc_http_client **out)
{
#if defined(SC_HAVE_ASYNC_HTTP)
    sc_http_client *client = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.http_client.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = http_curl_global_init();
    if (!sc_status_is_ok(status)) {
        return status;
    }
    client = sc_alloc(alloc, sizeof(*client), _Alignof(sc_http_client));
    if (client == nullptr) {
        return sc_status_no_memory();
    }
    *client = (sc_http_client){.alloc = alloc};
    if (backend_loop != nullptr) {
        client->loop = backend_loop;
    } else {
        if (uv_loop_init(&client->owned_loop) != 0) {
            sc_free(alloc, client, sizeof(*client), _Alignof(sc_http_client));
            return sc_status_io("sc.http_client.uv_loop_init_failed");
        }
        client->loop = &client->owned_loop;
        client->owns_loop = true;
        client->loop_initialized = true;
    }
    client->multi = curl_multi_init();
    if (client->multi == nullptr) {
        sc_http_client_destroy(client);
        return sc_status_no_memory();
    }
    if (uv_timer_init(client->loop, &client->timer) != 0) {
        sc_http_client_destroy(client);
        return sc_status_io("sc.http_client.timer_init_failed");
    }
    client->timer.data = client;
    client->timer_initialized = true;
    (void)curl_multi_setopt(client->multi, CURLMOPT_SOCKETFUNCTION, http_socket_cb);
    (void)curl_multi_setopt(client->multi, CURLMOPT_SOCKETDATA, client);
    (void)curl_multi_setopt(client->multi, CURLMOPT_TIMERFUNCTION, http_timer_cb);
    (void)curl_multi_setopt(client->multi, CURLMOPT_TIMERDATA, client);
    *out = client;
    return sc_status_ok();
#else
    (void)alloc;
    (void)backend_loop;
    (void)out;
    return sc_status_unsupported("sc.http_client.async_http_unavailable");
#endif
}

sc_status sc_http_client_perform(sc_http_client *client,
                                 const sc_http_request *request,
                                 sc_allocator *response_alloc,
                                 sc_http_complete_fn complete,
                                 void *user_data,
                                 sc_http_op **out)
{
#if defined(SC_HAVE_ASYNC_HTTP)
    sc_http_op *op = nullptr;
    sc_status status;
    CURLMcode multi_code;

    if (client == nullptr || request == nullptr || complete == nullptr) {
        return sc_status_invalid_argument("sc.http_client.invalid_argument");
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    status = http_request_validate(request);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (request->cancel_token != nullptr && request->cancel_token->cancel_requested) {
        return sc_status_cancelled("sc.http_client.cancelled");
    }
    response_alloc = response_alloc == nullptr ? client->alloc : response_alloc;
    op = sc_alloc(client->alloc, sizeof(*op), _Alignof(sc_http_op));
    if (op == nullptr) {
        return sc_status_no_memory();
    }
    *op = (sc_http_op){
        .alloc = client->alloc,
        .client = client,
        .complete = complete,
        .user_data = user_data,
        .status = sc_status_ok(),
        .cancel_token = request->cancel_token,
    };
    sc_bytes_init(&op->response.body, response_alloc);
    op->response.struct_size = sizeof(op->response);
    op->easy = curl_easy_init();
    if (op->easy == nullptr) {
        sc_http_op_destroy(op);
        return sc_status_no_memory();
    }
    status = http_build_headers(client->alloc, request, &op->headers);
    if (!sc_status_is_ok(status)) {
        sc_http_op_destroy(op);
        return status;
    }

    (void)curl_easy_setopt(op->easy, CURLOPT_PRIVATE, op);
    (void)curl_easy_setopt(op->easy, CURLOPT_URL, request->url.ptr);
    (void)curl_easy_setopt(op->easy, CURLOPT_CUSTOMREQUEST, request->method.ptr == nullptr ? "GET" : request->method.ptr);
    (void)curl_easy_setopt(op->easy, CURLOPT_HTTPHEADER, op->headers);
    (void)curl_easy_setopt(op->easy, CURLOPT_WRITEFUNCTION, http_write_cb);
    (void)curl_easy_setopt(op->easy, CURLOPT_WRITEDATA, op);
    (void)curl_easy_setopt(op->easy, CURLOPT_XFERINFOFUNCTION, http_progress_cb);
    (void)curl_easy_setopt(op->easy, CURLOPT_XFERINFODATA, op);
    (void)curl_easy_setopt(op->easy, CURLOPT_NOPROGRESS, 0L);
    (void)curl_easy_setopt(op->easy, CURLOPT_USERAGENT, request->user_agent.ptr == nullptr ? "smolclaw-c/0.1 http-client" : request->user_agent.ptr);
    (void)curl_easy_setopt(op->easy, CURLOPT_CONNECTTIMEOUT_MS, request->connect_timeout_ms <= 0 ? 10000L : (long)request->connect_timeout_ms);
    (void)curl_easy_setopt(op->easy, CURLOPT_TIMEOUT_MS, request->timeout_ms <= 0 ? 30000L : (long)request->timeout_ms);
    (void)curl_easy_setopt(op->easy, CURLOPT_FOLLOWLOCATION, request->follow_location ? 1L : 0L);
    (void)curl_easy_setopt(op->easy, CURLOPT_SSL_VERIFYPEER, 1L);
    (void)curl_easy_setopt(op->easy, CURLOPT_SSL_VERIFYHOST, 2L);
    const char *ca_bundle = http_ca_bundle_path();
    if (ca_bundle != nullptr && ca_bundle[0] != '\0') {
        (void)curl_easy_setopt(op->easy, CURLOPT_CAINFO, ca_bundle);
    }
    (void)curl_easy_setopt(op->easy, CURLOPT_ERRORBUFFER, op->error_buffer);
    if (request->http_version != 0) {
        (void)curl_easy_setopt(op->easy, CURLOPT_HTTP_VERSION, request->http_version);
    }
    if (request->body.len > 0) {
        (void)curl_easy_setopt(op->easy, CURLOPT_POSTFIELDS, request->body.ptr == nullptr ? "" : request->body.ptr);
        (void)curl_easy_setopt(op->easy, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request->body.len);
    }
    if (request->aws_sigv4.len > 0) {
#if defined(SC_HAVE_CURL_AWS_SIGV4)
        (void)curl_easy_setopt(op->easy, CURLOPT_HTTPAUTH, (long)CURLAUTH_AWS_SIGV4);
        (void)curl_easy_setopt(op->easy, CURLOPT_AWS_SIGV4, request->aws_sigv4.ptr);
        (void)curl_easy_setopt(op->easy, CURLOPT_USERNAME, request->username.ptr);
        (void)curl_easy_setopt(op->easy, CURLOPT_PASSWORD, request->password.ptr);
#else
        sc_http_op_destroy(op);
        return sc_status_unsupported("sc.http_client.aws_sigv4_unavailable");
#endif
    }
    op->on_chunk = request->on_chunk;
    op->chunk_user_data = request->chunk_user_data;
    op->max_response_bytes = request->max_response_bytes;
    op->response.too_large = false;
    op->response.curl_code = (int)CURLE_OK;

    multi_code = curl_multi_add_handle(client->multi, op->easy);
    if (multi_code != CURLM_OK) {
        sc_http_op_destroy(op);
        return http_curlm_status(multi_code, "sc.http_client.multi_add_failed");
    }
    op->multi_added = true;
    client->active_ops += 1;
    if (out != nullptr) {
        *out = op;
    }
    return sc_status_ok();
#else
    (void)client;
    (void)request;
    (void)response_alloc;
    (void)complete;
    (void)user_data;
    (void)out;
    return sc_status_unsupported("sc.http_client.async_http_unavailable");
#endif
}

typedef struct http_sync_wait {
    bool done;
    sc_status status;
    sc_http_response response;
} http_sync_wait;

static void http_sync_complete(void *user_data, const sc_http_response *response, sc_status status)
{
    http_sync_wait *wait = user_data;
    sc_status copy_status;

    if (wait == nullptr) {
        return;
    }
    wait->status = status;
    if (response != nullptr) {
        /*
         * Async completions own their response storage through the operation.
         * The synchronous wrapper keeps a deep copy before the op is destroyed.
         */
        wait->response = *response;
        wait->response.body = (sc_bytes){0};
        copy_status = sc_bytes_from_buf(response->body.alloc,
                                        sc_buf_from_parts(response->body.ptr, response->body.len),
                                        &wait->response.body);
        if (!sc_status_is_ok(copy_status)) {
            wait->status = copy_status;
        }
    }
    wait->done = true;
}

sc_status sc_http_client_perform_sync(sc_allocator *alloc, const sc_http_request *request, sc_http_response *out)
{
#if defined(SC_HAVE_ASYNC_HTTP)
    sc_http_client *client = nullptr;
    sc_http_op *op = nullptr;
    http_sync_wait wait = {.status = sc_status_ok()};
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.http_client.invalid_argument");
    }
    *out = (sc_http_response){0};
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = sc_http_client_new(alloc, nullptr, &client);
    if (sc_status_is_ok(status)) {
        status = sc_http_client_perform(client, request, alloc, http_sync_complete, &wait, &op);
    }
    if (sc_status_is_ok(status)) {
        while (!wait.done) {
            int rc = uv_run(client->loop, UV_RUN_DEFAULT);
            // cppcheck-suppress knownConditionTrueFalse
            if (rc == 0 && !wait.done) {
                status = sc_status_io("sc.http_client.loop_stopped");
                break;
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = wait.status;
    }
    if (sc_status_is_ok(status)) {
        *out = wait.response;
        wait.response = (sc_http_response){0};
    }
    sc_http_response_clear(&wait.response);
    sc_http_op_destroy(op);
    sc_http_client_destroy(client);
    return status;
#else
    (void)alloc;
    (void)request;
    (void)out;
    return sc_status_unsupported("sc.http_client.async_http_unavailable");
#endif
}

const char *sc_http_curl_strerror(int curl_code)
{
#if defined(SC_HAVE_ASYNC_HTTP)
    return curl_easy_strerror((CURLcode)curl_code);
#else
    (void)curl_code;
    return "libcurl unavailable";
#endif
}

void sc_http_response_clear(sc_http_response *response)
{
    if (response == nullptr) {
        return;
    }
    sc_bytes_clear(&response->body);
    *response = (sc_http_response){0};
}

void sc_http_op_cancel(sc_http_op *op)
{
#if defined(SC_HAVE_ASYNC_HTTP)
    if (op == nullptr || op->completed) {
        return;
    }
    op->cancelled = true;
    http_complete_op(op, sc_status_cancelled("sc.http_client.cancelled"));
#else
    (void)op;
#endif
}

void sc_http_op_destroy(sc_http_op *op)
{
#if defined(SC_HAVE_ASYNC_HTTP)
    if (op == nullptr) {
        return;
    }
    http_cleanup_easy(op);
    sc_http_response_clear(&op->response);
    sc_free(op->alloc, op, sizeof(*op), _Alignof(sc_http_op));
#else
    (void)op;
#endif
}

void sc_http_client_destroy(sc_http_client *client)
{
    if (client == nullptr) {
        return;
    }
#if defined(SC_HAVE_ASYNC_HTTP)
    if (client->timer_initialized && !uv_is_closing((uv_handle_t *)&client->timer)) {
        uv_close((uv_handle_t *)&client->timer, nullptr);
    }
    if (client->owns_loop && client->loop_initialized) {
        while (uv_run(client->loop, UV_RUN_DEFAULT) != 0) {
        }
    }
    if (client->multi != nullptr) {
        curl_multi_cleanup(client->multi);
    }
    if (client->owns_loop && client->loop_initialized) {
        (void)uv_loop_close(client->loop);
    }
#endif
    sc_free(client->alloc, client, sizeof(*client), _Alignof(sc_http_client));
}

#if defined(SC_HAVE_ASYNC_HTTP)
static sc_status http_curl_global_init(void)
{
    return sc_curl_global_init("sc.http_client.curl_init_failed");
}

static sc_status http_request_validate(const sc_http_request *request)
{
    if (request == nullptr || request->url.ptr == nullptr || request->url.len == 0) {
        return sc_status_invalid_argument("sc.http_client.invalid_argument");
    }
    if ((request->body.len > 0 && request->body.ptr == nullptr) ||
        (request->header_count > 0 && request->headers == nullptr)) {
        return sc_status_invalid_argument("sc.http_client.invalid_argument");
    }
    return sc_status_ok();
}

static sc_status http_build_headers(sc_allocator *alloc, const sc_http_request *request, struct curl_slist **out)
{
    struct curl_slist *headers = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.http_client.invalid_argument");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < request->header_count; i += 1) {
        status = http_append_header(alloc, &headers, request->headers[i].name, request->headers[i].value);
    }
    if (!sc_status_is_ok(status)) {
        curl_slist_free_all(headers);
        return status;
    }
    *out = headers;
    return sc_status_ok();
}

static sc_status http_append_header(sc_allocator *alloc, struct curl_slist **headers, sc_str name, sc_str value)
{
    sc_string_builder builder = {0};
    sc_string line = {0};
    struct curl_slist *next = nullptr;
    sc_status status;

    if (headers == nullptr || name.ptr == nullptr || name.len == 0) {
        return sc_status_invalid_argument("sc.http_client.header_invalid");
    }
    for (size_t i = 0; i < name.len; i += 1) {
        if (name.ptr[i] == '\r' || name.ptr[i] == '\n') {
            return sc_status_invalid_argument("sc.http_client.header_invalid");
        }
    }
    for (size_t i = 0; i < value.len; i += 1) {
        if (value.ptr == nullptr || value.ptr[i] == '\r' || value.ptr[i] == '\n') {
            return sc_status_invalid_argument("sc.http_client.header_invalid");
        }
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, name);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ": ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &line);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    next = curl_slist_append(*headers, line.ptr);
    sc_string_clear(&line);
    if (next == nullptr) {
        return sc_status_no_memory();
    }
    *headers = next;
    return sc_status_ok();
}

static size_t http_write_cb(char *ptr, size_t size, size_t nmemb, void *userdata)
{
    sc_http_op *op = userdata;
    size_t bytes = 0;
    size_t next_len = 0;
    sc_status status;

    if (op == nullptr || ptr == nullptr || sc_size_mul_overflow(size, nmemb, &bytes)) {
        return 0;
    }
    if (op->cancelled || (op->cancel_token != nullptr && op->cancel_token->cancel_requested)) {
        op->cancelled = true;
        return CURL_WRITEFUNC_ERROR;
    }
    if (op->max_response_bytes > 0 &&
        (sc_size_add_overflow(op->response.body.len, bytes, &next_len) || next_len > op->max_response_bytes)) {
        op->response.too_large = true;
        return CURL_WRITEFUNC_ERROR;
    }
    /*
     * Returning CURL_WRITEFUNC_ERROR is libcurl's only backpressure path here;
     * preserve the richer sc_status on the op for http_check_multi_info().
     */
    if (op->on_chunk != nullptr) {
        status = op->on_chunk(op->chunk_user_data, sc_buf_from_parts(ptr, bytes));
        if (!sc_status_is_ok(status)) {
            op->status = status;
            return CURL_WRITEFUNC_ERROR;
        }
    }
    status = sc_bytes_append(&op->response.body, sc_buf_from_parts(ptr, bytes));
    if (!sc_status_is_ok(status)) {
        op->status = status;
        return CURL_WRITEFUNC_ERROR;
    }
    return bytes;
}

static int http_progress_cb(void *userdata,
                            curl_off_t download_total,
                            curl_off_t download_now,
                            curl_off_t upload_total,
                            curl_off_t upload_now)
{
    sc_http_op *op = userdata;
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;

    if (op != nullptr && (op->cancelled || (op->cancel_token != nullptr && op->cancel_token->cancel_requested))) {
        op->cancelled = true;
        return 1;
    }
    return 0;
}

static int http_socket_cb(CURL *easy, curl_socket_t socket, int action, void *user_data, void *socket_user_data)
{
    sc_http_client *client = user_data;
    http_socket_context *context = socket_user_data;
    int events = 0;
    (void)easy;

    if (client == nullptr) {
        return 0;
    }
    switch (action) {
    case CURL_POLL_IN:
    case CURL_POLL_OUT:
    case CURL_POLL_INOUT:
        if (context == nullptr) {
            /*
             * libcurl reports socket interest changes; libuv owns the actual
             * polling handle until CURL_POLL_REMOVE asks us to close it.
             */
            context = sc_alloc(client->alloc, sizeof(*context), _Alignof(http_socket_context));
            if (context == nullptr) {
                return -1;
            }
            *context = (http_socket_context){.client = client, .socket = socket};
            if (uv_poll_init_socket(client->loop, &context->poll, socket) != 0) {
                sc_free(client->alloc, context, sizeof(*context), _Alignof(http_socket_context));
                return -1;
            }
            context->poll.data = context;
            (void)curl_multi_assign(client->multi, socket, context);
        }
        if (action != CURL_POLL_IN) {
            events |= UV_WRITABLE;
        }
        if (action != CURL_POLL_OUT) {
            events |= UV_READABLE;
        }
        (void)uv_poll_start(&context->poll, events, http_poll_cb);
        break;
    case CURL_POLL_REMOVE:
        if (context != nullptr) {
            (void)uv_poll_stop(&context->poll);
            (void)curl_multi_assign(client->multi, socket, nullptr);
            http_socket_context_destroy(context);
        }
        break;
    default:
        break;
    }
    return 0;
}

static int http_timer_cb(CURLM *multi, long timeout_ms, void *user_data)
{
    sc_http_client *client = user_data;
    (void)multi;

    if (client == nullptr || !client->timer_initialized) {
        return 0;
    }
    if (timeout_ms < 0) {
        uv_timer_stop(&client->timer);
    } else {
        if (timeout_ms == 0) {
            timeout_ms = 1;
        }
        (void)uv_timer_start(&client->timer, http_timer_fire_cb, (uint64_t)timeout_ms, 0);
    }
    return 0;
}

static void http_poll_cb(uv_poll_t *poll, int status, int events)
{
    http_socket_context *context = poll == nullptr ? nullptr : poll->data;
    int running = 0;
    int flags = 0;

    if (context == nullptr || context->client == nullptr) {
        return;
    }
    if (status < 0) {
        flags = CURL_CSELECT_ERR;
    } else {
        if ((events & UV_READABLE) != 0) {
            flags |= CURL_CSELECT_IN;
        }
        if ((events & UV_WRITABLE) != 0) {
            flags |= CURL_CSELECT_OUT;
        }
    }
    (void)curl_multi_socket_action(context->client->multi, context->socket, flags, &running);
    http_check_multi_info(context->client);
}

static void http_timer_fire_cb(uv_timer_t *timer)
{
    sc_http_client *client = timer == nullptr ? nullptr : timer->data;
    int running = 0;

    if (client == nullptr) {
        return;
    }
    (void)curl_multi_socket_action(client->multi, CURL_SOCKET_TIMEOUT, 0, &running);
    http_check_multi_info(client);
}

static void http_socket_close_cb(uv_handle_t *handle)
{
    http_socket_context *context = handle == nullptr ? nullptr : handle->data;
    if (context == nullptr || context->client == nullptr) {
        return;
    }
    sc_free(context->client->alloc, context, sizeof(*context), _Alignof(http_socket_context));
}

static void http_socket_context_destroy(http_socket_context *context)
{
    if (context == nullptr || context->closing) {
        return;
    }
    context->closing = true;
    uv_close((uv_handle_t *)&context->poll, http_socket_close_cb);
}

static void http_check_multi_info(sc_http_client *client)
{
    CURLMsg *message = nullptr;
    int pending = 0;

    if (client == nullptr) {
        return;
    }
    while ((message = curl_multi_info_read(client->multi, &pending)) != nullptr) {
        if (message->msg == CURLMSG_DONE) {
            sc_http_op *op = nullptr;
            long response_code = 0;
            (void)curl_easy_getinfo(message->easy_handle, CURLINFO_PRIVATE, &op);
            if (op == nullptr) {
                continue;
            }
            (void)curl_easy_getinfo(message->easy_handle, CURLINFO_RESPONSE_CODE, &response_code);
            op->response.http_status = response_code;
            op->response.curl_code = (int)message->data.result;
            /* Prefer statuses captured by callbacks; libcurl only reports the transfer abort. */
            if (!sc_status_is_ok(op->status)) {
                http_complete_op(op, op->status);
                op->status = sc_status_ok();
            } else if (op->cancelled) {
                http_complete_op(op, sc_status_cancelled("sc.http_client.cancelled"));
            } else if (op->cancel_token != nullptr && op->cancel_token->cancel_requested) {
                http_complete_op(op, sc_status_cancelled("sc.http_client.cancelled"));
            } else if (op->response.too_large) {
                http_complete_op(op, sc_status_http("sc.http_client.response_too_large"));
            } else if (message->data.result == CURLE_OPERATION_TIMEDOUT) {
                http_complete_op(op, sc_status_timeout("sc.http_client.timeout"));
            } else if (message->data.result != CURLE_OK) {
                http_complete_op(op, sc_status_http("sc.http_client.request_failed"));
            } else if (response_code >= 400) {
                http_complete_op(op, sc_status_http("sc.http_client.http_status"));
            } else {
                http_complete_op(op, sc_status_ok());
            }
        }
    }
}

static void http_complete_op(sc_http_op *op, sc_status status)
{
    if (op == nullptr || op->completed) {
        sc_status_clear(&status);
        return;
    }
    op->completed = true;
    if (op->client != nullptr && op->client->active_ops > 0) {
        op->client->active_ops -= 1;
    }
    http_cleanup_easy(op);
    op->complete(op->user_data, &op->response, status);
}

static void http_cleanup_easy(sc_http_op *op)
{
    if (op == nullptr) {
        return;
    }
    if (op->client != nullptr && op->client->multi != nullptr && op->easy != nullptr && op->multi_added) {
        (void)curl_multi_remove_handle(op->client->multi, op->easy);
        op->multi_added = false;
    }
    if (op->easy != nullptr) {
        curl_easy_cleanup(op->easy);
        op->easy = nullptr;
    }
    if (op->headers != nullptr) {
        curl_slist_free_all(op->headers);
        op->headers = nullptr;
    }
}

static sc_status http_curlm_status(CURLMcode code, const char *error_key)
{
    if (code == CURLM_OK) {
        return sc_status_ok();
    }
    (void)curl_multi_strerror(code);
    return sc_status_http(error_key);
}

static const char *http_ca_bundle_path(void)
{
    const char *value = getenv("SC_HTTP_CA_BUNDLE");
    if (value == nullptr || value[0] == '\0') {
        value = getenv("SC_CA_BUNDLE");
    }
    if (value == nullptr || value[0] == '\0') {
        value = getenv("CURL_CA_BUNDLE");
    }
    return value;
}
#endif
