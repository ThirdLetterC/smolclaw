#include "runtime/loop_internal.h"

#include <stdio.h>
#include <string.h>

enum {
    RUNTIME_TURN_QUEUE_DEFAULT_CONSECUTIVE_LIMIT = 5
};

static size_t job_consecutive_limit(const sc_runtime_agent_job *job);
static sc_status queued_turn_init(sc_runtime_turn_queue *queue,
                                  const sc_runtime_agent_job *job,
                                  const runtime_queued_turn *previous,
                                  runtime_queued_turn *out);
static sc_status queued_turn_copy_message_window(sc_allocator *alloc,
                                                 const runtime_queued_turn *previous,
                                                 sc_str input,
                                                 size_t limit,
                                                 sc_vec *out);
static sc_status queued_turn_push_input(sc_allocator *alloc, sc_vec *messages, sc_str input);
static void queued_turn_trim_message_window(sc_vec *messages, size_t limit);
static void queued_turn_remove_message_at(sc_vec *messages, size_t index);
static void queued_turn_messages_clear(sc_vec *messages);
static sc_status queued_turn_build_effective_input(sc_allocator *alloc, const sc_vec *messages, sc_string *out);
static const runtime_queued_turn *latest_matching_queued_turn(const sc_runtime_turn_queue *queue,
                                                              sc_str session_id,
                                                              sc_str thread_id);

sc_status sc_runtime_turn_queue_new(sc_allocator *alloc,
                                    sc_runtime_loop *loop,
                                    sc_agent *agent,
                                    sc_runtime_turn_queue **out)
{
    sc_runtime_turn_queue *queue = nullptr;

    if (loop == nullptr || agent == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.runtime_turn_queue.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    queue = sc_alloc(alloc, sizeof(*queue), _Alignof(sc_runtime_turn_queue));
    if (queue == nullptr) {
        return sc_status_no_memory();
    }
    *queue = (sc_runtime_turn_queue){.alloc = alloc, .loop = loop, .agent = agent};
    sc_vec_init(&queue->jobs, alloc, sizeof(runtime_queued_turn));
    *out = queue;
    return sc_status_ok();
}

sc_status sc_runtime_turn_queue_enqueue(sc_runtime_turn_queue *queue, const sc_runtime_agent_job *job)
{
    runtime_queued_turn copy = {0};
    const runtime_queued_turn *previous = nullptr;
    sc_status status;

    if (queue == nullptr || job == nullptr || job->input.ptr == nullptr || job->input.len == 0) {
        return sc_status_invalid_argument("sc.runtime_turn_queue.job_invalid_argument");
    }
    if (queue->loop != nullptr && queue->loop->shutdown.cancel_requested) {
        return sc_status_cancelled("sc.runtime_turn_queue.shutdown");
    }
    if (job->cancel_previous) {
        for (size_t i = 0; i < queue->jobs.len; i += 1) {
            runtime_queued_turn *queued = sc_vec_at(&queue->jobs, i);
            if (runtime_queued_turn_matches(queued, job->session_id, job->thread_id)) {
                queued->cancel_requested = true;
                (void)sc_cancel_token_cancel(&queued->cancel_token,
                                             queue->alloc,
                                             sc_str_from_cstr("runtime_turn_queue.consecutive_message"));
            }
        }
    }
    previous = job->cancel_previous ? latest_matching_queued_turn(queue, job->session_id, job->thread_id) : nullptr;
    status = queued_turn_init(queue, job, previous, &copy);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&queue->jobs, &copy);
    }
    if (!sc_status_is_ok(status)) {
        runtime_queued_turn_clear(&copy);
    }
    return status;
}

sc_status sc_runtime_turn_queue_schedule(sc_runtime_turn_queue *queue, uint32_t interval_ms)
{
    if (queue == nullptr) {
        return sc_status_invalid_argument("sc.runtime_turn_queue.invalid_argument");
    }
    if (queue->scheduled) {
        return sc_status_ok();
    }
    sc_status status = sc_runtime_loop_add_task(queue->loop,
                                                &(sc_runtime_loop_task_options){
                                                    .struct_size = sizeof(sc_runtime_loop_task_options),
                                                    .name = sc_str_from_cstr("runtime.agent_turn_queue"),
                                                    .run = runtime_turn_queue_task,
                                                    .user_data = queue,
                                                    .interval_ms = interval_ms,
                                                    .repeat = true,
                                                    .run_immediately = true,
                                                    .task_class = SC_RUNTIME_TASK_AGENT_TURN,
                                                });
    if (sc_status_is_ok(status)) {
        queue->scheduled = true;
    }
    return status;
}

sc_status sc_runtime_turn_queue_drain(sc_runtime_turn_queue *queue, size_t max_turns, sc_allocator *alloc)
{
    sc_status status = sc_status_ok();
    size_t processed = 0;

    if (queue == nullptr) {
        return sc_status_invalid_argument("sc.runtime_turn_queue.invalid_argument");
    }
    alloc = alloc == nullptr ? queue->alloc : alloc;
    while (sc_status_is_ok(status) && queue->jobs.len > 0 && (max_turns == 0 || processed < max_turns)) {
        runtime_queued_turn *job = sc_vec_at(&queue->jobs, 0);
        sc_agent_turn_result result = {0};
        sc_turn turn = {
            .struct_size = sizeof(turn),
            .input = sc_string_as_str(&job->input),
            .session_id = sc_string_as_str(&job->session_id),
            .cancel_requested = job->cancel_requested,
            .cancel_token = &job->cancel_token,
        };
        sc_runtime_agent_job_done_fn done = job->done;
        void *user_data = job->user_data;
        status = sc_agent_process_message(queue->agent, &turn, alloc, &result);
        if (done != nullptr) {
            done(user_data, &result, status);
        }
        sc_agent_turn_result_clear(&result);
        runtime_turn_queue_remove_at(queue, 0);
        if (status.code == SC_ERR_CANCELLED) {
            sc_status_clear(&status);
            status = sc_status_ok();
        }
        processed += 1;
    }
    return status;
}

size_t sc_runtime_turn_queue_len(const sc_runtime_turn_queue *queue)
{
    return queue == nullptr ? 0 : queue->jobs.len;
}

void sc_runtime_turn_queue_destroy(sc_runtime_turn_queue *queue)
{
    if (queue == nullptr) {
        return;
    }
    for (size_t i = 0; i < queue->jobs.len; i += 1) {
        runtime_queued_turn *job = sc_vec_at(&queue->jobs, i);
        runtime_queued_turn_clear(job);
    }
    sc_vec_clear(&queue->jobs);
    sc_free(queue->alloc, queue, sizeof(*queue), _Alignof(sc_runtime_turn_queue));
}

void runtime_queued_turn_clear(runtime_queued_turn *job)
{
    if (job == nullptr) {
        return;
    }
    sc_string_clear(&job->input);
    sc_string_clear(&job->session_id);
    sc_string_clear(&job->thread_id);
    queued_turn_messages_clear(&job->consecutive_inputs);
    sc_cancel_token_clear(&job->cancel_token);
    *job = (runtime_queued_turn){0};
}

void runtime_turn_queue_remove_at(sc_runtime_turn_queue *queue, size_t index)
{
    runtime_queued_turn *job = nullptr;

    if (queue == nullptr || index >= queue->jobs.len) {
        return;
    }
    job = sc_vec_at(&queue->jobs, index);
    runtime_queued_turn_clear(job);
    if (index + 1 < queue->jobs.len) {
        (void)memmove((unsigned char *)queue->jobs.ptr + index * queue->jobs.item_size,
                      (unsigned char *)queue->jobs.ptr + (index + 1) * queue->jobs.item_size,
                      (queue->jobs.len - index - 1) * queue->jobs.item_size);
    }
    queue->jobs.len -= 1;
}

bool runtime_queued_turn_matches(const runtime_queued_turn *job, sc_str session_id, sc_str thread_id)
{
    sc_str queued_session = {0};
    sc_str queued_thread = {0};

    if (job == nullptr) {
        return false;
    }
    queued_session = sc_string_as_str(&job->session_id);
    queued_thread = sc_string_as_str(&job->thread_id);
    if (session_id.ptr == nullptr) {
        session_id = sc_str_from_cstr("");
    }
    if (thread_id.ptr == nullptr) {
        thread_id = sc_str_from_cstr("");
    }
    return sc_str_equal(queued_session, session_id) && sc_str_equal(queued_thread, thread_id);
}

sc_status runtime_turn_queue_task(void *user_data, const sc_cancel_token *cancel, sc_allocator *alloc)
{
    sc_runtime_turn_queue *queue = user_data;
    runtime_queued_turn *job = nullptr;
    sc_status status;

    if (queue == nullptr) {
        return sc_status_invalid_argument("sc.runtime_turn_queue.task_invalid_argument");
    }
    if (cancel != nullptr && cancel->cancel_requested) {
        return sc_status_cancelled("sc.runtime_turn_queue.cancelled");
    }
    if (queue->in_flight || queue->jobs.len == 0) {
        return sc_status_ok();
    }
    job = sc_vec_at(&queue->jobs, 0);
    if (job == nullptr) {
        return sc_status_invalid_argument("sc.runtime_turn_queue.job_invalid_argument");
    }
    job->active_turn = (sc_turn){
        .struct_size = sizeof(sc_turn),
        .input = sc_string_as_str(&job->input),
        .session_id = sc_string_as_str(&job->session_id),
        .cancel_requested = job->cancel_requested,
        .cancel_token = &job->cancel_token,
    };
    queue->in_flight = true;
    status = sc_agent_process_message_async(queue->agent,
                                            sc_runtime_loop_async_context(queue->loop),
                                            &job->active_turn,
                                            alloc == nullptr ? queue->alloc : alloc,
                                            runtime_turn_queue_async_done,
                                            queue,
                                            nullptr);
    if (!sc_status_is_ok(status)) {
        queue->in_flight = false;
    }
    return status;
}

void runtime_turn_queue_async_done(void *user_data, const sc_agent_turn_result *result, sc_status status)
{
    sc_runtime_turn_queue *queue = user_data;
    const runtime_queued_turn *job = nullptr;
    sc_runtime_agent_job_done_fn done = nullptr;
    void *done_user_data = nullptr;

    if (queue == nullptr) {
        sc_status_clear(&status);
        return;
    }
    job = queue->jobs.len == 0 ? nullptr : sc_vec_at(&queue->jobs, 0);
    if (job != nullptr) {
        done = job->done;
        done_user_data = job->user_data;
    }
    if (done != nullptr) {
        done(done_user_data, result, status);
    }
    if (queue->jobs.len > 0) {
        runtime_turn_queue_remove_at(queue, 0);
    }
    queue->in_flight = false;
    sc_status_clear(&status);
}

static size_t job_consecutive_limit(const sc_runtime_agent_job *job)
{
    return job != nullptr && job->consecutive_message_limit > 0 ?
        job->consecutive_message_limit :
        RUNTIME_TURN_QUEUE_DEFAULT_CONSECUTIVE_LIMIT;
}

static sc_status queued_turn_init(sc_runtime_turn_queue *queue,
                                  const sc_runtime_agent_job *job,
                                  const runtime_queued_turn *previous,
                                  runtime_queued_turn *out)
{
    sc_status status;

    if (queue == nullptr || job == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.runtime_turn_queue.job_invalid_argument");
    }
    *out = (runtime_queued_turn){
        .struct_size = sizeof(*out),
        .source = job->source,
        .cancel_requested = false,
        .done = job->done,
        .user_data = job->user_data,
    };
    status = sc_cancel_token_init(&out->cancel_token, queue->alloc);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(queue->alloc,
                                    job->session_id.ptr == nullptr ? sc_str_from_cstr("") : job->session_id,
                                    &out->session_id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(queue->alloc,
                                    job->thread_id.ptr == nullptr ? sc_str_from_cstr("") : job->thread_id,
                                    &out->thread_id);
    }
    if (sc_status_is_ok(status)) {
        status = queued_turn_copy_message_window(queue->alloc,
                                                 previous,
                                                 job->input,
                                                 job_consecutive_limit(job),
                                                 &out->consecutive_inputs);
    }
    if (sc_status_is_ok(status)) {
        status = queued_turn_build_effective_input(queue->alloc, &out->consecutive_inputs, &out->input);
    }
    if (!sc_status_is_ok(status)) {
        runtime_queued_turn_clear(out);
    }
    return status;
}

static sc_status queued_turn_copy_message_window(sc_allocator *alloc,
                                                 const runtime_queued_turn *previous,
                                                 sc_str input,
                                                 size_t limit,
                                                 sc_vec *out)
{
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.runtime_turn_queue.window_invalid_argument");
    }
    sc_vec_init(out, alloc, sizeof(sc_string));
    if (previous != nullptr) {
        for (size_t i = 0; sc_status_is_ok(status) && i < previous->consecutive_inputs.len; i += 1) {
            const sc_string *message = sc_vec_at_const(&previous->consecutive_inputs, i);
            status = queued_turn_push_input(alloc,
                                            out,
                                            message == nullptr ? sc_str_from_cstr("") : sc_string_as_str(message));
        }
    }
    if (sc_status_is_ok(status)) {
        status = queued_turn_push_input(alloc, out, input);
    }
    if (sc_status_is_ok(status)) {
        queued_turn_trim_message_window(out, limit);
    } else {
        queued_turn_messages_clear(out);
    }
    return status;
}

static sc_status queued_turn_push_input(sc_allocator *alloc, sc_vec *messages, sc_str input)
{
    sc_string copy = {0};
    sc_status status;

    if (messages == nullptr) {
        return sc_status_invalid_argument("sc.runtime_turn_queue.window_invalid_argument");
    }
    status = sc_string_from_str(alloc, input.ptr == nullptr ? sc_str_from_cstr("") : input, &copy);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(messages, &copy);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&copy);
    }
    return status;
}

static void queued_turn_trim_message_window(sc_vec *messages, size_t limit)
{
    if (messages == nullptr) {
        return;
    }
    if (limit == 0) {
        limit = RUNTIME_TURN_QUEUE_DEFAULT_CONSECUTIVE_LIMIT;
    }
    while (messages->len > limit) {
        queued_turn_remove_message_at(messages, 0);
    }
}

static void queued_turn_remove_message_at(sc_vec *messages, size_t index)
{
    sc_string *message = nullptr;

    if (messages == nullptr || index >= messages->len) {
        return;
    }
    message = sc_vec_at(messages, index);
    sc_string_clear(message);
    if (index + 1 < messages->len) {
        (void)memmove((unsigned char *)messages->ptr + index * messages->item_size,
                      (unsigned char *)messages->ptr + (index + 1) * messages->item_size,
                      (messages->len - index - 1) * messages->item_size);
    }
    messages->len -= 1;
}

static void queued_turn_messages_clear(sc_vec *messages)
{
    if (messages == nullptr) {
        return;
    }
    for (size_t i = 0; i < messages->len; i += 1) {
        sc_string *message = sc_vec_at(messages, i);
        sc_string_clear(message);
    }
    sc_vec_clear(messages);
}

static sc_status queued_turn_build_effective_input(sc_allocator *alloc, const sc_vec *messages, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    if (messages == nullptr || out == nullptr || messages->len == 0) {
        return sc_status_invalid_argument("sc.runtime_turn_queue.window_invalid_argument");
    }
    if (messages->len == 1) {
        const sc_string *message = sc_vec_at_const(messages, 0);
        return sc_string_from_str(alloc,
                                  message == nullptr ? sc_str_from_cstr("") : sc_string_as_str(message),
                                  out);
    }

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(
        &builder,
        "Consecutive user messages received while an earlier run was active. Respond to the combined latest intent.\n\n");
    for (size_t i = 0; sc_status_is_ok(status) && i < messages->len; i += 1) {
        const sc_string *message = sc_vec_at_const(messages, i);
        char index_text[32] = {0};

        (void)snprintf(index_text, sizeof(index_text), "%zu. ", i + 1);
        status = sc_string_builder_append_cstr(&builder, index_text);
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder,
                                              message == nullptr ? sc_str_from_cstr("") : sc_string_as_str(message));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\n");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static const runtime_queued_turn *latest_matching_queued_turn(const sc_runtime_turn_queue *queue,
                                                              sc_str session_id,
                                                              sc_str thread_id)
{
    if (queue == nullptr) {
        return nullptr;
    }
    for (size_t i = queue->jobs.len; i > 0; i -= 1) {
        const runtime_queued_turn *queued = sc_vec_at_const(&queue->jobs, i - 1);
        if (runtime_queued_turn_matches(queued, session_id, thread_id)) {
            return queued;
        }
    }
    return nullptr;
}
