#pragma once

#include "sc/allocator.h"
#include "sc/result.h"

SC_BEGIN_DECLS

/*
 * Async objects are opaque. Backend handles stay private to implementation
 * files and must not cross the public ABI.
 */
typedef struct sc_async_context sc_async_context;
typedef struct sc_async_op sc_async_op;

typedef void (*sc_async_complete_fn)(void *user_data, sc_status status);

typedef struct sc_async_context_options {
    size_t struct_size;
    sc_allocator *alloc;
    void *backend_loop;
} sc_async_context_options;

sc_status sc_async_context_new(const sc_async_context_options *options, sc_async_context **out);
sc_status sc_async_context_run(sc_async_context *context);
sc_status sc_async_context_stop(sc_async_context *context);
void sc_async_context_destroy(sc_async_context *context);

bool sc_async_op_completed(const sc_async_op *op);
sc_status sc_async_op_cancel(sc_async_op *op);
void sc_async_op_destroy(sc_async_op *op);

SC_END_DECLS
