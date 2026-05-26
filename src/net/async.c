#include "sc/async.h"

#include "net/http_client.h"

#include <uv.h>

struct sc_async_context {
    sc_allocator *alloc;
    uv_loop_t *loop;
    uv_loop_t owned_loop;
    bool owns_loop;
    bool loop_initialized;
    sc_http_client *http_client;
};

struct sc_async_op {
    bool completed;
    bool cancelled;
};

sc_status sc_async_context_new(const sc_async_context_options *options, sc_async_context **out)
{
    sc_allocator *alloc = nullptr;
    sc_async_context *context = nullptr;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.async_context.invalid_argument");
    }
    alloc = options == nullptr || options->alloc == nullptr ? sc_allocator_heap() : options->alloc;
    context = sc_alloc(alloc, sizeof(*context), _Alignof(sc_async_context));
    if (context == nullptr) {
        return sc_status_no_memory();
    }
    *context = (sc_async_context){.alloc = alloc};
    if (options != nullptr && options->backend_loop != nullptr) {
        context->loop = options->backend_loop;
    } else {
        if (uv_loop_init(&context->owned_loop) != 0) {
            sc_free(alloc, context, sizeof(*context), _Alignof(sc_async_context));
            return sc_status_io("sc.async_context.uv_loop_init_failed");
        }
        context->loop = &context->owned_loop;
        context->owns_loop = true;
        context->loop_initialized = true;
    }
    *out = context;
    return sc_status_ok();
}

sc_status sc_async_context_run(sc_async_context *context)
{
    if (context == nullptr || context->loop == nullptr) {
        return sc_status_invalid_argument("sc.async_context.invalid_argument");
    }
    (void)uv_run(context->loop, UV_RUN_DEFAULT);
    return sc_status_ok();
}

sc_status sc_async_context_stop(sc_async_context *context)
{
    if (context == nullptr || context->loop == nullptr) {
        return sc_status_invalid_argument("sc.async_context.invalid_argument");
    }
    uv_stop(context->loop);
    return sc_status_ok();
}

void sc_async_context_destroy(sc_async_context *context)
{
    if (context == nullptr) {
        return;
    }
    sc_http_client_destroy(context->http_client);
    context->http_client = nullptr;
    if (context->owns_loop && context->loop_initialized) {
        while (uv_run(context->loop, UV_RUN_DEFAULT) != 0) {
        }
        (void)uv_loop_close(context->loop);
    }
    sc_free(context->alloc, context, sizeof(*context), _Alignof(sc_async_context));
}

sc_status sc_async_context_http_client(sc_async_context *context, sc_http_client **out)
{
#if defined(SC_HAVE_ASYNC_HTTP)
    sc_status status;

    if (context == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.async_context.invalid_argument");
    }
    if (context->http_client == nullptr) {
        status = sc_http_client_new(context->alloc, context->loop, &context->http_client);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    *out = context->http_client;
    return sc_status_ok();
#else
    (void)context;
    (void)out;
    return sc_status_unsupported("sc.http_client.async_http_unavailable");
#endif
}

bool sc_async_op_completed(const sc_async_op *op)
{
    return op != nullptr && op->completed;
}

sc_status sc_async_op_cancel(sc_async_op *op)
{
    if (op == nullptr) {
        return sc_status_invalid_argument("sc.async_op.invalid_argument");
    }
    op->cancelled = true;
    return sc_status_ok();
}

void sc_async_op_destroy(sc_async_op *op)
{
    (void)op;
}
