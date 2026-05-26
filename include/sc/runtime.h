#pragma once

#include "sc/allocator.h"
#include "sc/async.h"
#include "sc/contract.h"
#include "sc/media.h"
#include "sc/memory.h"
#include "sc/observer.h"
#include "sc/provider.h"
#include "sc/result.h"
#include "sc/security.h"
#include "sc/string.h"
#include "sc/tool.h"
#include "sc/vector.h"

SC_BEGIN_DECLS

/*
 * Ownership/threading: turns borrow input fields. Turn results are caller-owned.
 * The handle owns impl and calls destroy exactly once. The wrapper does not
 * synchronize turns; runtime adapters document their own thread-safety.
 */
typedef struct sc_runtime sc_runtime;
typedef struct sc_agent sc_agent;
typedef struct sc_agent_turn_result sc_agent_turn_result;
typedef struct sc_model_switch_request sc_model_switch_request;
typedef struct sc_runtime_loop sc_runtime_loop;
typedef struct sc_timer_handle sc_timer_handle;
typedef struct sc_runtime_turn_queue sc_runtime_turn_queue;

typedef uint64_t sc_runtime_turn_id;

typedef enum sc_turn_event_type {
    SC_TURN_EVENT_INPUT_VALIDATED = 0,
    SC_TURN_EVENT_MEMORY_LOADED,
    SC_TURN_EVENT_PROMPT_BUILT,
    SC_TURN_EVENT_PROVIDER_CALLED,
    SC_TURN_EVENT_TEXT_DELTA,
    SC_TURN_EVENT_TOOL_CALLED,
    SC_TURN_EVENT_TOOL_RESULT,
    SC_TURN_EVENT_TURN_PERSISTED,
    SC_TURN_EVENT_MODEL_SWITCHED,
    SC_TURN_EVENT_CANCELLED,
    SC_TURN_EVENT_ERROR,
    SC_TURN_EVENT_STARTED,
    SC_TURN_EVENT_PROVIDER_STARTED,
    SC_TURN_EVENT_TOOL_DENIED,
    SC_TURN_EVENT_MEMORY_WRITTEN,
    SC_TURN_EVENT_COMPLETED,
    SC_TURN_EVENT_TIMEOUT
} sc_turn_event_type;

typedef struct sc_turn_event {
    size_t struct_size;
    sc_turn_event_type type;
    sc_string name;
    sc_string message;
    sc_status_code status_code;
} sc_turn_event;

typedef struct sc_cancel_token {
    size_t struct_size;
    bool cancel_requested;
    sc_string reason;
} sc_cancel_token;

typedef sc_status (*sc_runtime_loop_task_fn)(void *user_data,
                                             const sc_cancel_token *cancel,
                                             sc_allocator *alloc);
typedef sc_status (*sc_runtime_shutdown_phase_fn)(void *user_data, sc_allocator *alloc);
typedef void (*sc_runtime_agent_job_done_fn)(void *user_data,
                                             const sc_agent_turn_result *result,
                                             sc_status status);
typedef void (*sc_agent_turn_complete_fn)(void *user_data,
                                          const sc_agent_turn_result *result,
                                          sc_status status);
typedef sc_status (*sc_turn_tool_approval_fn)(void *user_data,
                                              sc_str tool_name,
                                              sc_str arguments_json,
                                              sc_allocator *alloc,
                                              bool *out_approved);
typedef void (*sc_turn_event_fn)(void *user_data, const sc_turn_event *event);

typedef enum sc_shutdown_reason {
    SC_SHUTDOWN_NONE = 0,
    SC_SHUTDOWN_REQUESTED,
    SC_SHUTDOWN_CANCELLED,
    SC_SHUTDOWN_TIMEOUT,
    SC_SHUTDOWN_ERROR
} sc_shutdown_reason;

typedef enum sc_runtime_task_class {
    SC_RUNTIME_TASK_GENERIC = 0,
    SC_RUNTIME_TASK_GATEWAY,
    SC_RUNTIME_TASK_WEBSOCKET,
    SC_RUNTIME_TASK_CHANNEL,
    SC_RUNTIME_TASK_CRON,
    SC_RUNTIME_TASK_AGENT_TURN,
    SC_RUNTIME_TASK_PROVIDER,
    SC_RUNTIME_TASK_TOOL,
    SC_RUNTIME_TASK_SHUTDOWN
} sc_runtime_task_class;

typedef struct sc_runtime_loop_options {
    size_t struct_size;
    sc_observer *observer;
    size_t max_iterations;
    uint32_t idle_sleep_ms;
    bool hard_shutdown;
} sc_runtime_loop_options;

typedef struct sc_runtime_loop_task_options {
    size_t struct_size;
    sc_str name;
    sc_runtime_loop_task_fn run;
    void *user_data;
    uint32_t interval_ms;
    bool repeat;
    bool run_immediately;
    sc_runtime_task_class task_class;
    sc_str session_id;
    sc_str thread_id;
} sc_runtime_loop_task_options;

typedef struct sc_timer_options {
    size_t struct_size;
    sc_str name;
    sc_runtime_loop_task_fn run;
    void *user_data;
    uint32_t deadline_ms;
} sc_timer_options;

typedef struct sc_runtime_shutdown_options {
    size_t struct_size;
    sc_runtime_shutdown_phase_fn drain_receipts;
    sc_runtime_shutdown_phase_fn flush_observers;
    sc_runtime_shutdown_phase_fn close_transports;
    sc_runtime_shutdown_phase_fn destroy_subsystems;
    void *user_data;
    bool hard;
} sc_runtime_shutdown_options;

typedef struct sc_turn {
    size_t struct_size;
    sc_str input;
    sc_str session_id;
    bool cancel_requested;
    const sc_media_attachment *media;
    size_t media_count;
    sc_str media_context;
    const sc_str *allowed_tools;
    size_t allowed_tool_count;
    const sc_str *denied_tools;
    size_t denied_tool_count;
    bool autonomy_override_set;
    sc_autonomy_level autonomy_override;
    int64_t max_total_tokens;
    double max_cost_usd;
    const sc_model_switch_request *model_switch;
    sc_runtime_turn_id turn_id;
    uint32_t timeout_ms;
    bool use_streaming;
    bool emit_stream_deltas;
    const sc_cancel_token *cancel_token;
    sc_str route_hint;
    bool show_reasoning;
    sc_turn_tool_approval_fn request_tool_approval;
    void *request_tool_approval_user_data;
    sc_turn_event_fn event_callback;
    void *event_callback_user_data;
    bool disable_memory;
} sc_turn;

typedef enum sc_runtime_agent_job_source {
    SC_RUNTIME_AGENT_JOB_GATEWAY = 0,
    SC_RUNTIME_AGENT_JOB_WEBSOCKET,
    SC_RUNTIME_AGENT_JOB_CHANNEL,
    SC_RUNTIME_AGENT_JOB_CRON
} sc_runtime_agent_job_source;

typedef struct sc_runtime_agent_job {
    size_t struct_size;
    sc_runtime_agent_job_source source;
    sc_str input;
    sc_str session_id;
    sc_str thread_id;
    bool cancel_previous;
    size_t consecutive_message_limit;
    sc_runtime_agent_job_done_fn done;
    void *user_data;
} sc_runtime_agent_job;

typedef struct sc_prompt_builder {
    sc_allocator *alloc;
    sc_string_builder builder;
    size_t max_bytes;
} sc_prompt_builder;

typedef struct sc_history_message {
    size_t struct_size;
    sc_string role;
    sc_string content;
} sc_history_message;

typedef struct sc_history {
    sc_allocator *alloc;
    sc_vec messages;
    size_t max_messages;
} sc_history;

typedef struct sc_tool_loop {
    size_t struct_size;
    size_t max_iterations;
    size_t iteration_count;
    size_t tool_call_count;
    bool cancelled;
} sc_tool_loop;

typedef struct sc_model_switch_request {
    size_t struct_size;
    sc_str provider_name;
    sc_str model;
    sc_str reason;
} sc_model_switch_request;

typedef struct sc_agent_options {
    size_t struct_size;
    sc_provider *provider;
    sc_memory *memory;
    sc_tool **tools;
    size_t tool_count;
    sc_observer *observer;
    const sc_security_policy *policy;
    const sc_estop_state *estop;
    sc_str model;
    sc_str identity;
    sc_str workspace;
    sc_str runtime_environment;
    sc_str memory_namespace;
    sc_str turn_namespace;
    size_t max_history_messages;
    size_t max_memory_entries;
    size_t max_prompt_bytes;
    size_t max_tool_iterations;
    size_t max_tool_output_bytes;
    bool deterministic_prompts;
    bool include_wall_time;
    bool use_streaming;
    bool emit_stream_deltas;
    uint32_t default_timeout_ms;
} sc_agent_options;

struct sc_agent_turn_result {
    size_t struct_size;
    sc_string output;
    sc_receipt_chain receipts;
    sc_vec events;
    size_t provider_call_count;
    size_t tool_call_count;
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t total_tokens;
    double cost_usd;
    bool budget_exceeded;
    bool model_switched;
    sc_string active_model;
    bool cancelled;
    sc_runtime_turn_id turn_id;
    bool timed_out;
    sc_string attachment_content_type;
    sc_string attachment_filename;
    sc_bytes attachment_bytes;
    sc_attachment_delivery attachment_delivery;
};

typedef struct sc_runtime_config {
    size_t struct_size;
    sc_provider *provider;
    sc_memory *memory;
    sc_tool **tools;
    size_t tool_count;
    sc_observer *observer;
    const sc_security_policy *policy;
    const sc_estop_state *estop;
    sc_str model;
    sc_str identity;
    sc_str workspace;
    sc_str runtime_environment;
    sc_str memory_namespace;
    sc_str turn_namespace;
    size_t max_history_messages;
    size_t max_memory_entries;
    size_t max_prompt_bytes;
    size_t max_tool_iterations;
    size_t max_tool_output_bytes;
    bool deterministic_prompts;
    bool include_wall_time;
    bool use_streaming;
    bool emit_stream_deltas;
    uint32_t default_timeout_ms;
} sc_runtime_config;

typedef struct sc_runtime_message {
    size_t struct_size;
    sc_str input;
    sc_str session_id;
    const sc_media_attachment *media;
    size_t media_count;
    sc_str media_context;
    const sc_str *allowed_tools;
    size_t allowed_tool_count;
    const sc_str *denied_tools;
    size_t denied_tool_count;
    int64_t max_total_tokens;
    double max_cost_usd;
    const sc_model_switch_request *model_switch;
    uint32_t timeout_ms;
    bool use_streaming;
    bool emit_stream_deltas;
} sc_runtime_message;

typedef struct sc_runtime_response {
    size_t struct_size;
    sc_runtime_turn_id turn_id;
    sc_string output;
    sc_receipt_chain receipts;
    sc_vec events;
    size_t provider_call_count;
    size_t tool_call_count;
    int64_t input_tokens;
    int64_t output_tokens;
    int64_t total_tokens;
    double cost_usd;
    bool budget_exceeded;
    bool model_switched;
    sc_string active_model;
    bool cancelled;
    bool timed_out;
    sc_string attachment_content_type;
    sc_string attachment_filename;
    sc_bytes attachment_bytes;
    sc_attachment_delivery attachment_delivery;
} sc_runtime_response;

typedef struct sc_runtime_turn {
    size_t struct_size;
    sc_str input;
} sc_runtime_turn;

typedef struct sc_runtime_turn_result {
    size_t struct_size;
    sc_string output;
} sc_runtime_turn_result;

typedef struct sc_runtime_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    const char *display_name;
    const char *feature_flag;
    uint64_t capabilities;
    sc_stability_tier stability;
    sc_status (*run_turn)(void *impl,
                          const sc_runtime_turn *turn,
                          sc_allocator *alloc,
                          sc_runtime_turn_result *out);
    void (*destroy)(void *impl);
} sc_runtime_vtab;

static inline bool sc_runtime_handle_is_null(const sc_runtime *runtime)
{
    return runtime == nullptr;
}

bool sc_runtime_vtab_valid(const sc_runtime_vtab *vtab);
sc_status sc_runtime_new(sc_allocator *alloc, const sc_runtime_vtab *vtab, void *impl, sc_runtime **out);
sc_status sc_runtime_run_turn(sc_runtime *runtime,
                              const sc_runtime_turn *turn,
                              sc_allocator *alloc,
                              sc_runtime_turn_result *out);
const sc_runtime_vtab *sc_runtime_vtab_of(const sc_runtime *runtime);
void sc_runtime_destroy(sc_runtime *runtime);

sc_status sc_runtime_create(sc_allocator *alloc, const sc_runtime_config *config, sc_runtime **out);
sc_status sc_runtime_process_message(sc_runtime *runtime,
                                     const sc_runtime_message *message,
                                     sc_allocator *alloc,
                                     sc_runtime_response *out);
sc_status sc_runtime_cancel(sc_runtime *runtime, sc_runtime_turn_id turn_id);
void sc_runtime_response_clear(sc_runtime_response *response);

sc_status sc_cancel_token_init(sc_cancel_token *token, sc_allocator *alloc);
sc_status sc_cancel_token_cancel(sc_cancel_token *token, sc_allocator *alloc, sc_str reason);
void sc_cancel_token_clear(sc_cancel_token *token);
sc_status sc_runtime_loop_new(sc_allocator *alloc,
                              const sc_runtime_loop_options *options,
                              sc_runtime_loop **out);
sc_status sc_runtime_loop_add_task(sc_runtime_loop *loop, const sc_runtime_loop_task_options *options);
sc_status sc_runtime_loop_run(sc_runtime_loop *loop);
sc_status sc_runtime_loop_request_shutdown(sc_runtime_loop *loop, sc_shutdown_reason reason);
sc_status sc_runtime_loop_shutdown(sc_runtime_loop *loop, const sc_runtime_shutdown_options *options);
void sc_runtime_loop_destroy(sc_runtime_loop *loop);
sc_async_context *sc_runtime_loop_async_context(sc_runtime_loop *loop);
sc_status sc_runtime_timer_start(sc_runtime_loop *loop, const sc_timer_options *options, sc_timer_handle **out);
sc_status sc_runtime_timer_cancel(sc_timer_handle *handle);
bool sc_runtime_timer_active(const sc_timer_handle *handle);
int64_t sc_runtime_timer_deadline_ns(const sc_timer_handle *handle);
void sc_runtime_timer_destroy(sc_timer_handle *handle);
sc_status sc_runtime_turn_queue_new(sc_allocator *alloc,
                                    sc_runtime_loop *loop,
                                    sc_agent *agent,
                                    sc_runtime_turn_queue **out);
sc_status sc_runtime_turn_queue_enqueue(sc_runtime_turn_queue *queue, const sc_runtime_agent_job *job);
sc_status sc_runtime_turn_queue_schedule(sc_runtime_turn_queue *queue, uint32_t interval_ms);
sc_status sc_runtime_turn_queue_drain(sc_runtime_turn_queue *queue, size_t max_turns, sc_allocator *alloc);
size_t sc_runtime_turn_queue_len(const sc_runtime_turn_queue *queue);
void sc_runtime_turn_queue_destroy(sc_runtime_turn_queue *queue);

sc_status sc_agent_new(sc_allocator *alloc, const sc_agent_options *options, sc_agent **out);
sc_status sc_agent_process_message(sc_agent *agent,
                                   const sc_turn *turn,
                                   sc_allocator *alloc,
                                   sc_agent_turn_result *out);
sc_status sc_agent_process_message_async(sc_agent *agent,
                                         sc_async_context *context,
                                         const sc_turn *turn,
                                         sc_allocator *alloc,
                                         sc_agent_turn_complete_fn complete,
                                         void *complete_user_data,
                                         sc_async_op **out);
size_t sc_agent_history_len(const sc_agent *agent);
void sc_agent_destroy(sc_agent *agent);

void sc_agent_turn_result_init(sc_agent_turn_result *result, sc_allocator *alloc);
void sc_agent_turn_result_clear(sc_agent_turn_result *result);
void sc_turn_event_clear(sc_turn_event *event);

void sc_prompt_builder_init(sc_prompt_builder *builder, sc_allocator *alloc, size_t max_bytes);
sc_status sc_prompt_builder_append_section(sc_prompt_builder *builder, sc_str title, sc_str body);
sc_status sc_prompt_builder_finish(sc_prompt_builder *builder, sc_string *out);
void sc_prompt_builder_clear(sc_prompt_builder *builder);

void sc_history_init(sc_history *history, sc_allocator *alloc, size_t max_messages);
sc_status sc_history_append(sc_history *history, sc_str role, sc_str content);
sc_status sc_history_append_to_prompt(const sc_history *history, sc_prompt_builder *builder);
size_t sc_history_estimated_chars(const sc_history *history);
void sc_history_prune_orphans(sc_history *history);
sc_status sc_history_compress_to_recent(sc_history *history, size_t recent_messages, size_t max_summary_chars);
sc_status sc_history_prune_to_budget(sc_history *history, size_t max_chars);
void sc_history_trim(sc_history *history);
void sc_history_clear(sc_history *history);

SC_END_DECLS
