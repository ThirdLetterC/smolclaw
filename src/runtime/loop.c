// cppcheck-suppress-file redundantInitialization
#include "sc/runtime.h"

#include "sc/log.h"
#include "sc/time.h"

#include <signal.h>
#include <string.h>
#include <uv.h>

#include "runtime/loop_internal.h"

static void runtime_loop_task_clear(runtime_loop_task *task);
static void runtime_loop_task_destroy(runtime_loop_task *task);
static sc_status runtime_loop_add_task_internal(sc_runtime_loop *loop,
                                                const sc_runtime_loop_task_options *options,
                                                sc_timer_handle *timer_handle);
static sc_status runtime_loop_emit(sc_runtime_loop *loop, sc_str name, sc_str status_text);
static sc_status runtime_loop_set_due(runtime_loop_task *task, bool immediate);
static sc_status runtime_loop_remove_task(sc_runtime_loop *loop, runtime_loop_task *task);
static void runtime_loop_timer_cb(uv_timer_t *timer);
static void runtime_loop_close_cb(uv_handle_t *handle);
static void runtime_loop_signal_cb(uv_signal_t *signal, int signum);
static void runtime_loop_signal_close_cb(uv_handle_t *handle);
static void runtime_loop_stop_all(sc_runtime_loop *loop);
static void runtime_loop_drain_closing_handles(sc_runtime_loop *loop);
static sc_status runtime_loop_start_signal_handlers(sc_runtime_loop *loop);
static void runtime_loop_stop_signal_handlers(sc_runtime_loop *loop);
static void timer_handle_detach(sc_timer_handle *handle);

sc_status sc_cancel_token_init(sc_cancel_token *token, sc_allocator *alloc)
{
    if (token == nullptr) {
        return sc_status_invalid_argument("sc.cancel_token.invalid_argument");
    }
    *token = (sc_cancel_token){.struct_size = sizeof(*token)};
    return sc_string_from_cstr(alloc, "", &token->reason);
}

sc_status sc_cancel_token_cancel(sc_cancel_token *token, sc_allocator *alloc, sc_str reason)
{
    if (token == nullptr) {
        return sc_status_invalid_argument("sc.cancel_token.invalid_argument");
    }
    token->cancel_requested = true;
    sc_string_clear(&token->reason);
    return sc_string_from_str(alloc, reason.ptr == nullptr ? sc_str_from_cstr("") : reason, &token->reason);
}

void sc_cancel_token_clear(sc_cancel_token *token)
{
    if (token == nullptr) {
        return;
    }
    sc_string_clear(&token->reason);
    *token = (sc_cancel_token){0};
}

sc_status sc_runtime_loop_new(sc_allocator *alloc,
                              const sc_runtime_loop_options *options,
                              sc_runtime_loop **out)
{
    sc_runtime_loop *loop = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.runtime_loop.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    loop = sc_alloc(alloc, sizeof(*loop), _Alignof(sc_runtime_loop));
    if (loop == nullptr) {
        return sc_status_no_memory();
    }
    *loop = (sc_runtime_loop){
        .alloc = alloc,
        .observer = options == nullptr ? nullptr : options->observer,
        .max_iterations = options == nullptr ? 0 : options->max_iterations,
        .idle_sleep_ms = options == nullptr ? 0 : options->idle_sleep_ms,
        .hard_shutdown = options != nullptr &&
            options->struct_size >= offsetof(sc_runtime_loop_options, hard_shutdown) + sizeof(options->hard_shutdown) &&
            options->hard_shutdown,
        .status = sc_status_ok(),
    };
    sc_vec_init(&loop->tasks, alloc, sizeof(runtime_loop_task *));
    if (uv_loop_init(&loop->uv_loop) != 0) {
        sc_vec_clear(&loop->tasks);
        sc_free(alloc, loop, sizeof(*loop), _Alignof(sc_runtime_loop));
        return sc_status_io("sc.runtime_loop.libuv_init_failed");
    }
    loop->uv_initialized = true;
    status = sc_async_context_new(&(sc_async_context_options){
                                      .struct_size = sizeof(sc_async_context_options),
                                      .alloc = alloc,
                                      .backend_loop = &loop->uv_loop,
                                  },
                                  &loop->async_context);
    if (!sc_status_is_ok(status)) {
        sc_runtime_loop_destroy(loop);
        return status;
    }
    status = sc_cancel_token_init(&loop->shutdown, alloc);
    if (!sc_status_is_ok(status)) {
        sc_runtime_loop_destroy(loop);
        return status;
    }
    *out = loop;
    return sc_status_ok();
}

sc_status sc_runtime_loop_add_task(sc_runtime_loop *loop, const sc_runtime_loop_task_options *options)
{
    return runtime_loop_add_task_internal(loop, options, nullptr);
}

sc_async_context *sc_runtime_loop_async_context(sc_runtime_loop *loop)
{
    return loop == nullptr ? nullptr : loop->async_context;
}

sc_status sc_runtime_loop_run(sc_runtime_loop *loop)
{
    sc_status status = sc_status_ok();

    if (loop == nullptr) {
        return sc_status_invalid_argument("sc.runtime_loop.invalid_argument");
    }
    status = runtime_loop_emit(loop, sc_str_from_cstr("runtime.loop.start"), sc_str_from_cstr("ok"));
    if (sc_status_is_ok(status) && loop->uv_initialized) {
        status = runtime_loop_start_signal_handlers(loop);
    }
    if (sc_status_is_ok(status) && loop->uv_initialized && loop->tasks.len > 0 && !loop->shutdown.cancel_requested) {
        int rc = uv_run(&loop->uv_loop, UV_RUN_DEFAULT);
        if (rc < 0) {
            status = sc_status_io("sc.runtime_loop.libuv_run_failed");
        } else if (!sc_status_is_ok(loop->status)) {
            status = loop->status;
            loop->status = sc_status_ok();
        }
    }
    if (loop->uv_initialized) {
        runtime_loop_stop_signal_handlers(loop);
        runtime_loop_drain_closing_handles(loop);
    }
    if (sc_status_is_ok(status)) {
        status = runtime_loop_emit(loop, sc_str_from_cstr("runtime.loop.stopped"), sc_str_from_cstr("ok"));
    }
    return status;
}

static sc_status runtime_loop_add_task_internal(sc_runtime_loop *loop,
                                                const sc_runtime_loop_task_options *options,
                                                sc_timer_handle *timer_handle)
{
    runtime_loop_task *task = nullptr;
    sc_status status = sc_status_ok();

    if (loop == nullptr || options == nullptr || options->run == nullptr || options->name.ptr == nullptr || options->name.len == 0) {
        return sc_status_invalid_argument("sc.runtime_loop.task_invalid_argument");
    }
    if (loop->shutdown.cancel_requested) {
        (void)runtime_loop_emit(loop, sc_str_from_cstr("runtime.task.rejected"), options->name);
        return sc_status_cancelled("sc.runtime_loop.shutdown");
    }
    task = sc_alloc(loop->alloc, sizeof(*task), _Alignof(runtime_loop_task));
    if (task == nullptr) {
        return sc_status_no_memory();
    }
    *task = (runtime_loop_task){.struct_size = sizeof(*task), .owner = loop};
    status = sc_string_from_str(loop->alloc, options->name, &task->name);
    if (sc_status_is_ok(status)) {
        task->run = options->run;
        task->user_data = options->user_data;
        task->timer_handle = timer_handle;
        task->interval_ms = options->interval_ms;
        task->repeat = options->repeat;
        task->task_class = options->task_class;
        status = sc_string_from_str(loop->alloc,
                                    options->session_id.ptr == nullptr ? sc_str_from_cstr("") : options->session_id,
                                    &task->session_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(loop->alloc,
                                    options->thread_id.ptr == nullptr ? sc_str_from_cstr("") : options->thread_id,
                                    &task->thread_id);
    }
    if (sc_status_is_ok(status)) {
        status = runtime_loop_set_due(task, options->run_immediately);
    }
    if (sc_status_is_ok(status) && loop->uv_initialized) {
        int rc = uv_timer_init(&loop->uv_loop, &task->timer);
        if (rc != 0) {
            status = sc_status_io("sc.runtime_loop.timer_init_failed");
        } else {
            task->timer_initialized = true;
            task->timer.data = task;
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&loop->tasks, &task);
    }
    if (sc_status_is_ok(status) && timer_handle != nullptr) {
        timer_handle->loop = loop;
        timer_handle->task = task;
        timer_handle->deadline_ns = task->next_due_ns;
        timer_handle->active = true;
    }
    if (sc_status_is_ok(status) && loop->uv_initialized) {
        uint64_t initial_ms = options->run_immediately ? 0U : options->interval_ms;
        uint64_t repeat_ms = options->repeat ? (options->interval_ms == 0 ? 1U : options->interval_ms) : 0U;
        int rc = uv_timer_start(&task->timer, runtime_loop_timer_cb, initial_ms, repeat_ms);
        if (rc != 0) {
            (void)runtime_loop_remove_task(loop, task);
            return sc_status_io("sc.runtime_loop.timer_start_failed");
        }
    }
    if (!sc_status_is_ok(status)) {
        if (task != nullptr && task->timer_initialized && !task->closing) {
            task->closing = true;
            uv_close((uv_handle_t *)&task->timer, runtime_loop_close_cb);
            (void)uv_run(&loop->uv_loop, UV_RUN_DEFAULT);
        } else {
            runtime_loop_task_destroy(task);
        }
    }
    return status;
}

sc_status sc_runtime_loop_request_shutdown(sc_runtime_loop *loop, sc_shutdown_reason reason)
{
    sc_status status = sc_status_ok();

    if (loop == nullptr) {
        return sc_status_invalid_argument("sc.runtime_loop.invalid_argument");
    }
    if (loop->shutdown.cancel_requested) {
        loop->shutdown_reason = reason == SC_SHUTDOWN_NONE ? loop->shutdown_reason : reason;
        if (loop->uv_initialized) {
            uv_stop(&loop->uv_loop);
        }
        return sc_status_ok();
    }
    loop->shutdown_reason = reason;
    (void)runtime_loop_emit(loop, sc_str_from_cstr("runtime.loop.stop_requested"), sc_str_from_cstr("shutdown"));
    status = sc_cancel_token_cancel(&loop->shutdown, loop->alloc, sc_str_from_cstr("shutdown"));
    if (sc_status_is_ok(status) && loop->uv_initialized) {
        runtime_loop_stop_all(loop);
        uv_stop(&loop->uv_loop);
    }
    return status;
}

sc_status sc_runtime_loop_shutdown(sc_runtime_loop *loop, const sc_runtime_shutdown_options *options)
{
    sc_status status = sc_status_ok();
    bool hard = false;

    if (loop == nullptr) {
        return sc_status_invalid_argument("sc.runtime_loop.invalid_argument");
    }
    if (loop->shutdown_phases_completed) {
        return sc_status_ok();
    }
    hard = options != nullptr &&
        options->struct_size >= offsetof(sc_runtime_shutdown_options, hard) + sizeof(options->hard) &&
        options->hard;
    status = sc_runtime_loop_request_shutdown(loop, SC_SHUTDOWN_REQUESTED);
    if (!hard && sc_status_is_ok(status) && options != nullptr && options->drain_receipts != nullptr) {
        (void)runtime_loop_emit(loop, sc_str_from_cstr("runtime.shutdown.drain"), sc_str_from_cstr("receipts"));
        status = options->drain_receipts(options->user_data, loop->alloc);
    }
    if (!hard && sc_status_is_ok(status) && loop->observer != nullptr) {
        status = sc_observer_flush(loop->observer);
    }
    if (!hard && sc_status_is_ok(status) && options != nullptr && options->flush_observers != nullptr) {
        status = options->flush_observers(options->user_data, loop->alloc);
    }
    if (sc_status_is_ok(status) && options != nullptr && options->close_transports != nullptr) {
        status = options->close_transports(options->user_data, loop->alloc);
    }
    if (sc_status_is_ok(status) && options != nullptr && options->destroy_subsystems != nullptr) {
        status = options->destroy_subsystems(options->user_data, loop->alloc);
    }
    if (sc_status_is_ok(status)) {
        loop->shutdown_phases_completed = true;
    }
    return status;
}

sc_status sc_runtime_timer_start(sc_runtime_loop *loop, const sc_timer_options *options, sc_timer_handle **out)
{
    sc_timer_handle *handle = nullptr;
    sc_status status = sc_status_ok();

    if (loop == nullptr || options == nullptr || options->run == nullptr || options->name.ptr == nullptr || options->name.len == 0 ||
        out == nullptr) {
        return sc_status_invalid_argument("sc.runtime_timer.invalid_argument");
    }
    handle = sc_alloc(loop->alloc, sizeof(*handle), _Alignof(sc_timer_handle));
    if (handle == nullptr) {
        return sc_status_no_memory();
    }
    *handle = (sc_timer_handle){.alloc = loop->alloc, .loop = loop};
    status = sc_string_from_str(loop->alloc, options->name, &handle->name);
    if (sc_status_is_ok(status)) {
        sc_runtime_loop_task_options task_options = {
            .struct_size = sizeof(task_options),
            .name = options->name,
            .run = options->run,
            .user_data = options->user_data,
            .interval_ms = options->deadline_ms,
            .repeat = false,
            .run_immediately = options->deadline_ms == 0,
        };
        status = runtime_loop_add_task_internal(loop, &task_options, handle);
    }
    if (!sc_status_is_ok(status)) {
        sc_runtime_timer_destroy(handle);
        return status;
    }
    *out = handle;
    return sc_status_ok();
}

sc_status sc_runtime_timer_cancel(sc_timer_handle *handle)
{
    sc_status status = sc_status_ok();

    if (handle == nullptr) {
        return sc_status_invalid_argument("sc.runtime_timer.invalid_argument");
    }
    if (handle->active && handle->loop != nullptr && handle->task != nullptr) {
        sc_runtime_loop *loop = handle->loop;
        runtime_loop_task *task = handle->task;
        task->timer_handle = nullptr;
        timer_handle_detach(handle);
        status = runtime_loop_remove_task(loop, task);
    }
    handle->active = false;
    return status;
}

bool sc_runtime_timer_active(const sc_timer_handle *handle)
{
    return handle != nullptr && handle->active;
}

int64_t sc_runtime_timer_deadline_ns(const sc_timer_handle *handle)
{
    return handle == nullptr ? 0 : handle->deadline_ns;
}

void sc_runtime_timer_destroy(sc_timer_handle *handle)
{
    if (handle == nullptr) {
        return;
    }
    if (handle->active) {
        (void)sc_runtime_timer_cancel(handle);
    }
    sc_string_clear(&handle->name);
    sc_free(handle->alloc == nullptr ? sc_allocator_heap() : handle->alloc,
            handle,
            sizeof(*handle),
            _Alignof(sc_timer_handle));
}

void sc_runtime_loop_destroy(sc_runtime_loop *loop)
{
    if (loop == nullptr) {
        return;
    }
    sc_async_context_destroy(loop->async_context);
    loop->async_context = nullptr;
    if (loop->uv_initialized) {
        runtime_loop_stop_all(loop);
        runtime_loop_drain_closing_handles(loop);
        (void)uv_loop_close(&loop->uv_loop);
    } else {
        for (size_t i = 0; i < loop->tasks.len; i += 1) {
            runtime_loop_task **slot = sc_vec_at(&loop->tasks, i);
            runtime_loop_task_destroy(slot == nullptr ? nullptr : *slot);
        }
    }
    sc_vec_clear(&loop->tasks);
    sc_cancel_token_clear(&loop->shutdown);
    sc_free(loop->alloc, loop, sizeof(*loop), _Alignof(sc_runtime_loop));
}

static void runtime_loop_task_clear(runtime_loop_task *task)
{
    if (task == nullptr) {
        return;
    }
    sc_string_clear(&task->name);
    sc_string_clear(&task->session_id);
    sc_string_clear(&task->thread_id);
    *task = (runtime_loop_task){0};
}

static void runtime_loop_task_destroy(runtime_loop_task *task)
{
    if (task == nullptr) {
        return;
    }
    if (task->timer_initialized && !task->closing) {
        uv_timer_stop(&task->timer);
    }
    timer_handle_detach(task->timer_handle);
    sc_allocator *alloc = task->owner == nullptr ? sc_allocator_heap() : task->owner->alloc;
    runtime_loop_task_clear(task);
    sc_free(alloc, task, sizeof(*task), _Alignof(runtime_loop_task));
}

static sc_status runtime_loop_emit(sc_runtime_loop *loop, sc_str name, sc_str status_text)
{
    sc_log_field fields[] = {
        {.key = "status", .value = status_text, .secret = false},
    };
    sc_observer_event event = {
        .struct_size = sizeof(event),
        .target = sc_str_from_cstr("sc.runtime_loop"),
        .name = name,
        .fields = fields,
        .field_count = 1,
    };
    if (loop == nullptr || loop->observer == nullptr) {
        return sc_status_ok();
    }
    return sc_observer_emit_safe(loop->observer, &event, nullptr);
}

static sc_status runtime_loop_set_due(runtime_loop_task *task, bool immediate)
{
    sc_instant now = {0};
    sc_status status = sc_status_ok();

    if (task == nullptr) {
        return sc_status_invalid_argument("sc.runtime_loop.task_invalid_argument");
    }
    status = sc_clock_monotonic(&now);
    if (sc_status_is_ok(status)) {
        task->next_due_ns = immediate ? now.ns : now.ns + (int64_t)task->interval_ms * 1000000;
    }
    return status;
}

static sc_status runtime_loop_remove_task(sc_runtime_loop *loop, runtime_loop_task *task)
{
    if (loop == nullptr || task == nullptr) {
        return sc_status_invalid_argument("sc.runtime_loop.remove_invalid_argument");
    }
    for (size_t i = 0; i < loop->tasks.len; i += 1) {
        runtime_loop_task **slot = sc_vec_at(&loop->tasks, i);
        if (slot != nullptr && *slot == task) {
            if (i + 1 < loop->tasks.len) {
                (void)memmove((unsigned char *)loop->tasks.ptr + i * loop->tasks.item_size,
                              (unsigned char *)loop->tasks.ptr + (i + 1) * loop->tasks.item_size,
                              (loop->tasks.len - i - 1) * loop->tasks.item_size);
            }
            loop->tasks.len -= 1;
            if (loop->uv_initialized && task->timer_initialized && !task->closing) {
                task->closing = true;
                uv_timer_stop(&task->timer);
                uv_close((uv_handle_t *)&task->timer, runtime_loop_close_cb);
                return sc_status_ok();
            }
            runtime_loop_task_destroy(task);
            return sc_status_ok();
        }
    }
    return sc_status_invalid_argument("sc.runtime_loop.task_not_found");
}

static void runtime_loop_timer_cb(uv_timer_t *timer)
{
    runtime_loop_task *task = timer == nullptr ? nullptr : timer->data;
    sc_runtime_loop *loop = task == nullptr ? nullptr : task->owner;

    if (task == nullptr || loop == nullptr || task->run == nullptr || task->closing) {
        return;
    }
    loop->iteration_count += 1;
    loop->status = runtime_loop_emit(loop, sc_str_from_cstr("runtime.task.start"), sc_string_as_str(&task->name));
    if (sc_status_is_ok(loop->status)) {
        loop->status = task->run(task->user_data, &loop->shutdown, loop->alloc);
    }
    if (sc_status_is_ok(loop->status)) {
        loop->status = runtime_loop_emit(loop, sc_str_from_cstr("runtime.task.complete"), sc_string_as_str(&task->name));
    }
    if (!sc_status_is_ok(loop->status)) {
        runtime_loop_stop_all(loop);
        uv_stop(&loop->uv_loop);
        return;
    }
    if (loop->max_iterations > 0 && loop->iteration_count >= loop->max_iterations) {
        loop->status = sc_runtime_loop_request_shutdown(loop, SC_SHUTDOWN_REQUESTED);
        return;
    }
    if (!task->repeat || loop->shutdown.cancel_requested) {
        (void)runtime_loop_remove_task(loop, task);
    }
}

static void runtime_loop_signal_cb(uv_signal_t *signal, int signum)
{
    sc_runtime_loop *loop = signal == nullptr ? nullptr : signal->data;
    (void)signum;

    if (loop == nullptr) {
        return;
    }
    loop->shutdown_signal_count += 1;
    if (loop->hard_shutdown) {
        runtime_loop_stop_all(loop);
        uv_stop(&loop->uv_loop);
        return;
    }
    loop->status = sc_runtime_loop_request_shutdown(loop, SC_SHUTDOWN_REQUESTED);
}

static void runtime_loop_close_cb(uv_handle_t *handle)
{
    runtime_loop_task *task = handle == nullptr ? nullptr : handle->data;
    if (task != nullptr) {
        task->timer_initialized = false;
        runtime_loop_task_destroy(task);
    }
}

static void runtime_loop_signal_close_cb(uv_handle_t *handle)
{
    sc_runtime_loop *loop = handle == nullptr ? nullptr : handle->data;

    if (loop == nullptr) {
        return;
    }
    if (handle == (uv_handle_t *)&loop->sigint) {
        loop->sigint_initialized = false;
        loop->sigint_closing = false;
    } else if (handle == (uv_handle_t *)&loop->sigterm) {
        loop->sigterm_initialized = false;
        loop->sigterm_closing = false;
    }
}

static sc_status runtime_loop_start_signal_handlers(sc_runtime_loop *loop)
{
    int rc = 0;

    if (loop == nullptr || !loop->uv_initialized) {
        return sc_status_ok();
    }
    rc = uv_signal_init(&loop->uv_loop, &loop->sigint);
    if (rc != 0) {
        return sc_status_io("sc.runtime_loop.signal_init_failed");
    }
    loop->sigint_initialized = true;
    loop->sigint.data = loop;
    rc = uv_signal_start(&loop->sigint, runtime_loop_signal_cb, SIGINT);
    if (rc != 0) {
        return sc_status_io("sc.runtime_loop.signal_start_failed");
    }
#ifdef SIGTERM
    rc = uv_signal_init(&loop->uv_loop, &loop->sigterm);
    if (rc != 0) {
        return sc_status_io("sc.runtime_loop.signal_init_failed");
    }
    loop->sigterm_initialized = true;
    loop->sigterm.data = loop;
    rc = uv_signal_start(&loop->sigterm, runtime_loop_signal_cb, SIGTERM);
    if (rc != 0) {
        return sc_status_io("sc.runtime_loop.signal_start_failed");
    }
#endif
    return sc_status_ok();
}

static void runtime_loop_stop_signal_handlers(sc_runtime_loop *loop)
{
    if (loop == nullptr || !loop->uv_initialized) {
        return;
    }
    if (loop->sigint_initialized && !loop->sigint_closing) {
        loop->sigint_closing = true;
        uv_signal_stop(&loop->sigint);
        uv_close((uv_handle_t *)&loop->sigint, runtime_loop_signal_close_cb);
    }
    if (loop->sigterm_initialized && !loop->sigterm_closing) {
        loop->sigterm_closing = true;
        uv_signal_stop(&loop->sigterm);
        uv_close((uv_handle_t *)&loop->sigterm, runtime_loop_signal_close_cb);
    }
}

static void runtime_loop_stop_all(sc_runtime_loop *loop)
{
    if (loop == nullptr) {
        return;
    }
    for (size_t i = 0; i < loop->tasks.len; i += 1) {
        runtime_loop_task **slot = sc_vec_at(&loop->tasks, i);
        runtime_loop_task *task = slot == nullptr ? nullptr : *slot;
        if (task != nullptr && task->timer_initialized && !task->closing) {
            task->closing = true;
            uv_timer_stop(&task->timer);
            uv_close((uv_handle_t *)&task->timer, runtime_loop_close_cb);
        }
    }
    loop->tasks.len = 0;
}

static void runtime_loop_drain_closing_handles(sc_runtime_loop *loop)
{
    if (loop == nullptr || !loop->uv_initialized) {
        return;
    }
    while (uv_run(&loop->uv_loop, UV_RUN_DEFAULT) != 0) {
    }
}

static void timer_handle_detach(sc_timer_handle *handle)
{
    if (handle == nullptr) {
        return;
    }
    handle->task = nullptr;
    handle->loop = nullptr;
    handle->active = false;
}
