#pragma once

#include "sc/runtime.h"

#include <uv.h>

typedef struct runtime_loop_task {
    size_t struct_size;
    struct sc_runtime_loop *owner;
    sc_string name;
    sc_runtime_loop_task_fn run;
    void *user_data;
    sc_timer_handle *timer_handle;
    uint32_t interval_ms;
    bool repeat;
    sc_runtime_task_class task_class;
    sc_string session_id;
    sc_string thread_id;
    int64_t next_due_ns;
    uv_timer_t timer;
    bool timer_initialized;
    bool closing;
} runtime_loop_task;

struct sc_timer_handle {
    sc_allocator *alloc;
    sc_runtime_loop *loop;
    runtime_loop_task *task;
    sc_string name;
    int64_t deadline_ns;
    bool active;
};

typedef struct runtime_queued_turn {
    size_t struct_size;
    sc_runtime_agent_job_source source;
    sc_string input;
    sc_string session_id;
    sc_string thread_id;
    sc_vec consecutive_inputs;
    sc_cancel_token cancel_token;
    bool cancel_requested;
    sc_runtime_agent_job_done_fn done;
    void *user_data;
    sc_turn active_turn;
} runtime_queued_turn;

struct sc_runtime_turn_queue {
    sc_allocator *alloc;
    sc_runtime_loop *loop;
    sc_agent *agent;
    sc_vec jobs;
    bool scheduled;
    bool in_flight;
};

struct sc_runtime_loop {
    sc_allocator *alloc;
    sc_observer *observer;
    sc_vec tasks;
    sc_cancel_token shutdown;
    sc_shutdown_reason shutdown_reason;
    size_t max_iterations;
    uint32_t idle_sleep_ms;
    size_t iteration_count;
    sc_status status;
    bool hard_shutdown;
    bool shutdown_phases_completed;
    size_t shutdown_signal_count;
    sc_async_context *async_context;
    uv_loop_t uv_loop;
    bool uv_initialized;
    uv_signal_t sigint;
    uv_signal_t sigterm;
    bool sigint_initialized;
    bool sigterm_initialized;
    bool sigint_closing;
    bool sigterm_closing;
};


void runtime_queued_turn_clear(runtime_queued_turn *job);
void runtime_turn_queue_remove_at(sc_runtime_turn_queue *queue, size_t index);
bool runtime_queued_turn_matches(const runtime_queued_turn *job, sc_str session_id, sc_str thread_id);
sc_status runtime_turn_queue_task(void *user_data, const sc_cancel_token *cancel, sc_allocator *alloc);
void runtime_turn_queue_async_done(void *user_data, const sc_agent_turn_result *result, sc_status status);
