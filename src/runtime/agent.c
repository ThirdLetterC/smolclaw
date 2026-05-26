// cppcheck-suppress-file redundantInitialization
#include "sc/runtime.h"
#include "sc/time.h"
#include "tools/tool_internal.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct runtime_stream_collect {
    sc_allocator *alloc;
    sc_agent *agent;
    const sc_turn *turn;
    sc_agent_turn_result *result;
    sc_provider_response *response;
} runtime_stream_collect;

typedef struct agent_session_history {
    sc_string session_id;
    sc_history history;
} agent_session_history;

typedef struct agent_async_turn {
    sc_allocator *alloc;
    sc_agent *agent;
    sc_async_context *context;
    const sc_turn *turn;
    sc_history *history;
    sc_agent_turn_complete_fn complete;
    void *complete_user_data;
    sc_agent_turn_result result;
    sc_arena scratch;
    sc_allocator *scratch_alloc;
    sc_string memory_context;
    sc_string prompt_base;
    sc_string_builder output;
    sc_string_builder prompt;
    sc_json_value *tool_specs;
    sc_string tool_specs_json;
    sc_provider_response response;
    runtime_stream_collect stream_collect;
    sc_string_builder feedback;
    sc_vec terminal_failures;
    int64_t deadline_ns;
    size_t iterations;
    size_t tool_index;
    bool retried_context_overflow;
    bool feedback_initialized;
} agent_async_turn;

typedef struct tool_approval_denial {
    sc_string tool_name;
} tool_approval_denial;

typedef struct tool_approval_grant {
    sc_string tool_name;
    sc_string arguments_json;
} tool_approval_grant;

typedef struct tool_success_cache {
    sc_string tool_name;
    sc_string arguments_json;
    sc_string output;
    sc_string attachment_content_type;
    sc_string attachment_filename;
    sc_bytes attachment_bytes;
    sc_attachment_delivery attachment_delivery;
    size_t replay_count;
} tool_success_cache;

typedef struct tool_terminal_failure {
    sc_string tool_name;
    sc_string error_key;
} tool_terminal_failure;

struct sc_agent {
    sc_allocator *alloc;
    sc_provider *provider;
    sc_memory *memory;
    sc_observer *observer;
    const sc_security_policy *policy;
    const sc_estop_state *estop;
    sc_vec tools;
    sc_history history;
    sc_vec session_histories;
    sc_string model;
    sc_string identity;
    sc_string workspace;
    sc_string runtime_environment;
    sc_string memory_namespace;
    sc_string turn_namespace;
    sc_string cached_tool_specs_json;
    size_t cached_tool_specs_tool_count;
    size_t max_memory_entries;
    size_t max_prompt_bytes;
    size_t max_tool_iterations;
    size_t max_tool_output_bytes;
    uint64_t turn_counter;
    bool deterministic_prompts;
    bool include_wall_time;
    bool use_streaming;
    bool emit_stream_deltas;
    uint32_t default_timeout_ms;
};

static sc_status copy_option_string(sc_allocator *alloc, sc_str value, const char *fallback, sc_string *out);
static sc_status agent_copy_tools(sc_agent *agent, sc_tool **tools, size_t tool_count);
static sc_status agent_history_for_turn(sc_agent *agent, const sc_turn *turn, sc_history **out);
static sc_status agent_history_for_session(sc_agent *agent, sc_str session_id, sc_history **out);
static void agent_session_history_clear(agent_session_history *session);
static void history_message_clear(sc_history_message *message);
static sc_status result_add_event(sc_agent_turn_result *result,
                                  sc_turn_event_type type,
                                  sc_str name,
                                  sc_str message,
                                  sc_status_code status_code);
static sc_status turn_add_event(const sc_turn *turn,
                                sc_agent_turn_result *result,
                                sc_turn_event_type type,
                                sc_str name,
                                sc_str message,
                                sc_status_code status_code);
static sc_status agent_emit_observer(sc_agent *agent, sc_str event_name, sc_str status_text);
static sc_status load_memory_context(sc_agent *agent, const sc_turn *turn, sc_allocator *alloc, sc_string *out);
static sc_status apply_model_switch(sc_agent *agent,
                                    const sc_turn *turn,
                                    const sc_model_switch_request *request,
                                    sc_allocator *alloc,
                                    sc_agent_turn_result *out);
static sc_status build_prompt(sc_agent *agent,
                              const sc_turn *turn,
                              const sc_history *history,
                              sc_str memory_context,
                              sc_allocator *alloc,
                              sc_string *out);
static sc_status prune_history_for_prompt_budget(const sc_agent *agent,
                                                 sc_history *history,
                                                 const sc_turn *turn,
                                                 sc_str memory_context,
                                                 sc_agent_turn_result *out);
static sc_status compact_history_for_overflow_retry(const sc_agent *agent,
                                                    sc_history *history,
                                                    const sc_turn *turn,
                                                    sc_str memory_context,
                                                    sc_agent_turn_result *out);
static size_t agent_history_prompt_budget(const sc_agent *agent, const sc_turn *turn, sc_str memory_context);
static size_t history_recent_messages_for_budget(const sc_history *history, size_t max_chars);
static size_t history_message_estimated_chars(const sc_history_message *message);
static void size_add_saturating(size_t *target, size_t value);
static sc_status append_date_time_section(sc_prompt_builder *builder);
static sc_status append_tool_specs_section(sc_agent *agent, const sc_turn *turn, sc_prompt_builder *builder);
static sc_status tool_specs_json_for_turn(sc_agent *agent,
                                          const sc_turn *turn,
                                          sc_allocator *alloc,
                                          sc_string *owned,
                                          sc_str *out);
static bool turn_uses_default_tool_specs(const sc_turn *turn);
static bool turn_memory_disabled(const sc_turn *turn);
static bool turn_cancel_requested(const sc_turn *turn);
static bool turn_deadline_expired(int64_t deadline_ns);
static sc_status turn_check_continue(const sc_turn *turn, int64_t deadline_ns);
static sc_status run_provider_loop(sc_agent *agent,
                                   const sc_turn *turn,
                                   sc_str initial_prompt,
                                   int64_t deadline_ns,
                                   sc_allocator *scratch_alloc,
                                   sc_allocator *result_alloc,
                                   sc_agent_turn_result *out);
static sc_status execute_tool_calls(sc_agent *agent,
                                    const sc_turn *turn,
                                    const sc_provider_response *response,
                                    int64_t deadline_ns,
                                    sc_allocator *alloc,
                                    sc_allocator *result_alloc,
                                    sc_agent_turn_result *out,
                                    sc_string_builder *feedback,
                                    sc_vec *approval_denials,
                                    sc_vec *approval_grants,
                                    sc_vec *successful_tools,
                                    bool *out_repeated_success_stop,
                                    sc_vec *terminal_failures);
static sc_status provider_request_once(sc_agent *agent,
                                       const sc_turn *turn,
                                       const sc_provider_request *request,
                                       sc_allocator *alloc,
                                       sc_agent_turn_result *out,
                                       sc_provider_response *response);
static sc_status runtime_stream_callback(void *user_data, const sc_provider_stream_event *event);
static sc_status agent_async_start(agent_async_turn *state);
static void agent_async_provider_next(agent_async_turn *state);
static void agent_async_provider_generate_done(void *user_data, const sc_provider_response *response, sc_status status);
static void agent_async_provider_stream_done(void *user_data, sc_status status);
static void agent_async_provider_ready(agent_async_turn *state, sc_status status);
static bool agent_async_retry_context_overflow(agent_async_turn *state, sc_status status);
static sc_status agent_async_rebuild_prompt(agent_async_turn *state, sc_str event_name);
static void agent_async_tool_next(agent_async_turn *state);
static void agent_async_tool_done(void *user_data, const sc_tool_result *result, sc_status status);
static sc_status agent_async_tool_finish(agent_async_turn *state,
                                         const sc_provider_tool_call *provider_call,
                                         sc_status tool_status,
                                         const sc_tool_result *tool_result);
static sc_status agent_async_finalize_success(agent_async_turn *state);
static void agent_async_complete(agent_async_turn *state, sc_status status);
static void agent_async_destroy(agent_async_turn *state);
static sc_status provider_response_clone(sc_allocator *alloc, const sc_provider_response *source, sc_provider_response *out);
static sc_status copy_optional_string(sc_allocator *alloc, const sc_string *source, sc_string *out);
static sc_status result_copy_tool_attachment(sc_allocator *alloc, sc_agent_turn_result *out, const sc_tool_result *tool_result);
static sc_tool *find_tool_by_name(sc_agent *agent, sc_str name);
static bool turn_allows_tool(const sc_turn *turn, sc_str name);
static bool tool_name_is_memory(sc_str name);
static bool turn_autonomy_allows_risk(const sc_turn *turn, sc_tool_risk risk);
static bool turn_autonomy_allows_spec(const sc_turn *turn, const sc_tool_spec *spec);
static sc_status validate_tool_call_args(const sc_tool_spec *spec, const sc_provider_tool_call *provider_call);
static bool tool_status_is_approval_required(sc_status tool_status);
static bool tool_status_is_turn_terminal(sc_status tool_status);
static bool approval_denials_contains(const sc_vec *denials, const sc_provider_tool_call *provider_call);
static sc_status approval_denials_add(sc_vec *denials, sc_allocator *alloc, const sc_provider_tool_call *provider_call);
static void approval_denials_clear(sc_vec *denials);
static void tool_approval_denial_clear(tool_approval_denial *denial);
static bool approval_grants_contains(const sc_vec *grants, const sc_provider_tool_call *provider_call);
static sc_status approval_grants_add(sc_vec *grants, sc_allocator *alloc, const sc_provider_tool_call *provider_call);
static void approval_grants_clear(sc_vec *grants);
static void tool_approval_grant_clear(tool_approval_grant *grant);
static tool_success_cache *tool_success_cache_find(sc_vec *successes, const sc_provider_tool_call *provider_call);
static tool_success_cache *tool_success_cache_find_attachment_by_name(sc_vec *successes,
                                                                      const sc_provider_tool_call *provider_call);
static sc_status tool_success_cache_add(sc_vec *successes,
                                        sc_allocator *alloc,
                                        const sc_provider_tool_call *provider_call,
                                        const sc_tool_result *tool_result);
static sc_status tool_result_from_success_cache(sc_allocator *alloc,
                                                const tool_success_cache *cached,
                                                sc_tool_result *out);
static void tool_success_cache_clear(sc_vec *successes);
static void tool_success_cache_entry_clear(tool_success_cache *entry);
static bool tool_terminal_failures_contains(const sc_vec *failures, const sc_provider_tool_call *provider_call);
static sc_status tool_terminal_failures_add(sc_vec *failures,
                                            sc_allocator *alloc,
                                            const sc_provider_tool_call *provider_call,
                                            sc_status tool_status);
static void tool_terminal_failures_clear(sc_vec *failures);
static void tool_terminal_failure_clear(tool_terminal_failure *failure);
static sc_status request_turn_tool_approval(const sc_turn *turn,
                                            const sc_provider_tool_call *provider_call,
                                            sc_allocator *alloc,
                                            bool *out_approved);
static sc_status append_bounded(sc_string_builder *builder, sc_str value, size_t max_bytes);
static sc_status tool_specs_for_turn(sc_agent *agent, const sc_turn *turn, sc_allocator *alloc, sc_json_value **out);
static sc_status recover_text_tool_calls(sc_provider_response *response, sc_allocator *alloc);
static size_t find_literal(sc_str haystack, size_t start, const char *needle);
static size_t find_byte(sc_str haystack, size_t start, char needle);
static sc_status add_recovered_json_tool_call(sc_provider_response *response, sc_allocator *alloc, sc_str payload, size_t index);
static sc_status add_recovered_named_tool_call(sc_provider_response *response,
                                               sc_allocator *alloc,
                                               sc_str name,
                                               sc_str args_json,
                                               size_t index);
static sc_str trim_ascii(sc_str value);
static bool is_context_overflow_status(sc_status status);
static bool history_role_is(sc_str role, const char *expected);
static bool history_role_is_tool_result(sc_str role);
static void history_remove_at(sc_history *history, size_t index);
static sc_status history_push_message(sc_history *history, sc_history_message *message);
static void result_add_usage(sc_agent_turn_result *out, const sc_provider_response *response);
static bool result_exceeds_budget(const sc_turn *turn, const sc_agent_turn_result *out);
static sc_status persist_turn(sc_agent *agent, const sc_turn *turn, const sc_agent_turn_result *result);
static sc_str empty_if_null(sc_str value);
static sc_status append_line(sc_string_builder *builder, sc_str left, sc_str right);
static sc_status receipt_summary_for_turn(sc_allocator *alloc, sc_str value, sc_string *out);
static uint64_t receipt_hash_for_turn(sc_str value);
static bool agent_receipts_enabled(const sc_agent *agent);
static sc_status append_receipt_block_to_output(const sc_agent *agent,
                                                sc_allocator *alloc,
                                                sc_agent_turn_result *out);
static sc_status strip_receipt_tokens_from_output(sc_allocator *alloc, sc_agent_turn_result *out);
static sc_str tool_policy_decision(sc_status tool_status);
static sc_str tool_outcome(sc_status tool_status, bool success);
static sc_str tool_failure_reason(sc_status tool_status);

void sc_runtime_response_clear(sc_runtime_response *response)
{
    if (response == nullptr) {
        return;
    }
    sc_string_clear(&response->output);
    sc_string_clear(&response->attachment_content_type);
    sc_string_clear(&response->attachment_filename);
    sc_bytes_clear(&response->attachment_bytes);
    sc_receipt_chain_clear(&response->receipts);
    for (size_t i = 0; i < response->events.len; i += 1) {
        sc_turn_event *event = sc_vec_at(&response->events, i);
        sc_turn_event_clear(event);
    }
    sc_vec_clear(&response->events);
    sc_string_clear(&response->active_model);
    *response = (sc_runtime_response){0};
}

sc_status sc_agent_new(sc_allocator *alloc, const sc_agent_options *options, sc_agent **out)
{
    sc_agent *agent = nullptr;
    sc_status status;
    size_t max_history = 0;

    if (out == nullptr || options == nullptr || options->provider == nullptr) {
        return sc_status_invalid_argument("sc.agent.invalid_argument");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    agent = sc_alloc(alloc, sizeof(*agent), _Alignof(sc_agent));
    if (agent == nullptr) {
        return sc_status_no_memory();
    }
    *agent = (sc_agent){.alloc = alloc};
    agent->provider = options->provider;
    agent->memory = options->memory;
    agent->observer = options->observer;
    agent->policy = options->policy;
    agent->estop = options->estop;
    agent->max_memory_entries = options->max_memory_entries == 0 ? 4 : options->max_memory_entries;
    agent->max_prompt_bytes = options->max_prompt_bytes == 0 ? 16384 : options->max_prompt_bytes;
    agent->max_tool_iterations = options->max_tool_iterations == 0 ? 4 : options->max_tool_iterations;
    agent->max_tool_output_bytes = options->max_tool_output_bytes == 0 ? 4096 : options->max_tool_output_bytes;
    agent->deterministic_prompts = options->deterministic_prompts;
    agent->include_wall_time = options->include_wall_time && !options->deterministic_prompts;
    agent->use_streaming = options->use_streaming;
    agent->emit_stream_deltas = options->emit_stream_deltas;
    agent->default_timeout_ms = options->default_timeout_ms;
    max_history = options->max_history_messages == 0 ? 16 : options->max_history_messages;
    sc_vec_init(&agent->tools, alloc, sizeof(sc_tool *));
    sc_history_init(&agent->history, alloc, max_history);
    sc_vec_init(&agent->session_histories, alloc, sizeof(agent_session_history));

    status = copy_option_string(alloc, options->model, "mock-model", &agent->model);
    if (sc_status_is_ok(status)) {
        status = copy_option_string(alloc, options->identity, "SmolClaw agent", &agent->identity);
    }
    if (sc_status_is_ok(status)) {
        status = copy_option_string(alloc, options->workspace, "", &agent->workspace);
    }
    if (sc_status_is_ok(status)) {
        status = copy_option_string(alloc, options->runtime_environment, "smolclaw-runtime", &agent->runtime_environment);
    }
    if (sc_status_is_ok(status)) {
        status = copy_option_string(alloc, options->memory_namespace, "smolclaw.memory", &agent->memory_namespace);
    }
    if (sc_status_is_ok(status)) {
        status = copy_option_string(alloc, options->turn_namespace, "smolclaw.turns", &agent->turn_namespace);
    }
    if (sc_status_is_ok(status)) {
        status = agent_copy_tools(agent, options->tools, options->tool_count);
    }
    if (!sc_status_is_ok(status)) {
        sc_agent_destroy(agent);
        return status;
    }

    *out = agent;
    return sc_status_ok();
}

sc_status sc_agent_process_message(sc_agent *agent,
                                   const sc_turn *turn,
                                   sc_allocator *alloc,
                                   sc_agent_turn_result *out)
{
    sc_string memory_context = {0};
    sc_string prompt = {0};
    sc_arena scratch = {0};
    sc_allocator *scratch_alloc = nullptr;
    sc_history *history = nullptr;
    sc_status status;
    bool retried_context_overflow = false;
    sc_instant started = {0};
    int64_t deadline_ns = 0;

    if (agent == nullptr || turn == nullptr || out == nullptr || turn->input.ptr == nullptr || turn->input.len == 0) {
        return sc_status_invalid_argument("sc.agent.turn.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    /*
     * Prompt assembly and memory recall are per-turn scratch data. The final
     * result uses the caller allocator and is transferred out separately.
     */
    sc_arena_init(&scratch, alloc, agent->max_prompt_bytes == 0 ? 16'384 : agent->max_prompt_bytes);
    scratch_alloc = sc_arena_allocator(&scratch);
    sc_agent_turn_result_init(out, alloc);
    out->turn_id = turn->turn_id;
    status = agent_history_for_turn(agent, turn, &history);
    if ((turn->timeout_ms > 0 || agent->default_timeout_ms > 0) && sc_status_is_ok(sc_clock_monotonic(&started))) {
        uint32_t timeout_ms = turn->timeout_ms > 0 ? turn->timeout_ms : agent->default_timeout_ms;
        deadline_ns = started.ns + (int64_t)timeout_ms * 1'000'000;
    }

    if (!sc_status_is_ok(status)) {
        sc_arena_clear(&scratch);
        return status;
    }
    if (turn_cancel_requested(turn)) {
        out->cancelled = true;
        (void)turn_add_event(turn, out, SC_TURN_EVENT_CANCELLED, sc_str_from_cstr("cancelled"), sc_str_from_cstr("input"), SC_ERR_CANCELLED);
        sc_arena_clear(&scratch);
        return sc_status_cancelled("sc.agent.cancelled");
    }

    status = turn_add_event(turn, out, SC_TURN_EVENT_STARTED, sc_str_from_cstr("turn"), sc_str_from_cstr("started"), SC_OK);
    if (sc_status_is_ok(status)) {
        status = agent_emit_observer(agent, sc_str_from_cstr("runtime.turn.started"), sc_str_from_cstr("ok"));
    }
    if (sc_status_is_ok(status)) {
        status = apply_model_switch(agent, turn, turn->model_switch, alloc, out);
    }
    if (sc_status_is_ok(status)) {
        status = turn_add_event(turn, out, SC_TURN_EVENT_INPUT_VALIDATED, sc_str_from_cstr("input"), turn->input, SC_OK);
    }
    if (sc_status_is_ok(status)) {
        status = agent_emit_observer(agent, sc_str_from_cstr("turn.input_validated"), sc_str_from_cstr("ok"));
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(turn)) {
        status = load_memory_context(agent, turn, scratch_alloc, &memory_context);
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(turn)) {
        status = turn_add_event(turn, out,
                                  SC_TURN_EVENT_MEMORY_LOADED,
                                  sc_str_from_cstr("memory"),
                                  sc_str_from_cstr("loaded"),
                                  SC_OK);
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(turn)) {
        status = agent_emit_observer(agent, sc_str_from_cstr("runtime.memory.read"), sc_str_from_cstr("ok"));
    }
    if (sc_status_is_ok(status)) {
        status = turn_check_continue(turn, deadline_ns);
    }
    if (sc_status_is_ok(status)) {
        status = prune_history_for_prompt_budget(agent, history, turn, sc_string_as_str(&memory_context), out);
    }
    if (sc_status_is_ok(status)) {
        status = build_prompt(agent, turn, history, sc_string_as_str(&memory_context), scratch_alloc, &prompt);
    }
    if (sc_status_is_ok(status)) {
        status = turn_add_event(turn, out, SC_TURN_EVENT_PROMPT_BUILT, sc_str_from_cstr("prompt"), sc_str_from_cstr("built"), SC_OK);
    }
    if (sc_status_is_ok(status)) {
        status = agent_emit_observer(agent, sc_str_from_cstr("runtime.prompt.built"), sc_str_from_cstr("ok"));
    }
    while (sc_status_is_ok(status)) {
        status = turn_check_continue(turn, deadline_ns);
        if (!sc_status_is_ok(status)) {
            break;
        }
        status = run_provider_loop(agent, turn, sc_string_as_str(&prompt), deadline_ns, scratch_alloc, alloc, out);
        if (sc_status_is_ok(status) || !is_context_overflow_status(status) || retried_context_overflow ||
            history == nullptr || history->messages.len == 0) {
            break;
        }
        sc_status_clear(&status);
        retried_context_overflow = true;
        /* Give one provider context-overflow retry a much smaller history window before failing the turn. */
        status = compact_history_for_overflow_retry(agent, history, turn, sc_string_as_str(&memory_context), out);
        if (!sc_status_is_ok(status)) {
            break;
        }
        sc_string_clear(&prompt);
        status = build_prompt(agent, turn, history, sc_string_as_str(&memory_context), scratch_alloc, &prompt);
        if (sc_status_is_ok(status)) {
            status = turn_add_event(turn, out,
                                      SC_TURN_EVENT_PROMPT_BUILT,
                                      sc_str_from_cstr("prompt.context_trimmed"),
                                      sc_str_from_cstr("built"),
                                      SC_OK);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_string_clear(&prompt);
        sc_string_clear(&memory_context);
        sc_arena_clear(&scratch);
    }
    if (sc_status_is_ok(status)) {
        status = append_receipt_block_to_output(agent, alloc, out);
    }
    if (sc_status_is_ok(status)) {
        status = strip_receipt_tokens_from_output(alloc, out);
    }
    if (sc_status_is_ok(status)) {
        status = turn_check_continue(turn, deadline_ns);
    }
    if (sc_status_is_ok(status)) {
        status = sc_history_append(history, sc_str_from_cstr("user"), turn->input);
    }
    if (sc_status_is_ok(status)) {
        status = sc_history_append(history, sc_str_from_cstr("assistant"), sc_string_as_str(&out->output));
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(turn)) {
        status = persist_turn(agent, turn, out);
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(turn)) {
        status = turn_add_event(turn, out, SC_TURN_EVENT_TURN_PERSISTED, sc_str_from_cstr("turn"), sc_str_from_cstr("persisted"), SC_OK);
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(turn)) {
        status = agent_emit_observer(agent, sc_str_from_cstr("runtime.memory.write"), sc_str_from_cstr("ok"));
    }
    if (sc_status_is_ok(status)) {
        status = turn_add_event(turn, out, SC_TURN_EVENT_COMPLETED, sc_str_from_cstr("turn"), sc_str_from_cstr("completed"), SC_OK);
    }
    if (sc_status_is_ok(status)) {
        status = agent_emit_observer(agent, sc_str_from_cstr("runtime.turn.completed"), sc_str_from_cstr("ok"));
    }

    if (!sc_status_is_ok(status)) {
        if (status.code == SC_ERR_CANCELLED) {
            out->cancelled = true;
        } else if (status.code == SC_ERR_TIMEOUT) {
            out->timed_out = true;
        }
        (void)turn_add_event(turn, out,
                               status.code == SC_ERR_CANCELLED ? SC_TURN_EVENT_CANCELLED :
                                   (status.code == SC_ERR_TIMEOUT ? SC_TURN_EVENT_TIMEOUT : SC_TURN_EVENT_ERROR),
                               sc_str_from_cstr("turn"),
                               sc_str_from_cstr(status.error_key == nullptr ? "error" : status.error_key),
                               status.code);
        (void)agent_emit_observer(agent,
                                  status.code == SC_ERR_CANCELLED ? sc_str_from_cstr("runtime.turn.cancelled") :
                                      (status.code == SC_ERR_TIMEOUT ? sc_str_from_cstr("runtime.turn.timed_out") :
                                                                       sc_str_from_cstr("runtime.turn.failed")),
                                  sc_str_from_cstr(status.error_key == nullptr ? "error" : status.error_key));
    }

    sc_string_clear(&prompt);
    sc_string_clear(&memory_context);
    sc_arena_clear(&scratch);
    return status;
}

sc_status sc_agent_process_message_async(sc_agent *agent,
                                         sc_async_context *context,
                                         const sc_turn *turn,
                                         sc_allocator *alloc,
                                         sc_agent_turn_complete_fn complete,
                                         void *complete_user_data,
                                         sc_async_op **out)
{
    agent_async_turn *state = nullptr;
    sc_status status = sc_status_ok();

    if (agent == nullptr || context == nullptr || turn == nullptr || turn->input.ptr == nullptr || turn->input.len == 0 ||
        complete == nullptr) {
        return sc_status_invalid_argument("sc.agent.async_invalid_argument");
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(agent_async_turn));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (agent_async_turn){
        .alloc = alloc,
        .agent = agent,
        .context = context,
        .turn = turn,
        .complete = complete,
        .complete_user_data = complete_user_data,
    };
    status = agent_async_start(state);
    if (!sc_status_is_ok(status)) {
        agent_async_complete(state, status);
    }
    return sc_status_ok();
}

size_t sc_agent_history_len(const sc_agent *agent)
{
    size_t total = 0;

    if (agent == nullptr) {
        return 0;
    }
    total = agent->history.messages.len;
    for (size_t i = 0; i < agent->session_histories.len; i += 1) {
        const agent_session_history *session = sc_vec_at_const(&agent->session_histories, i);
        if (session != nullptr) {
            size_add_saturating(&total, session->history.messages.len);
        }
    }
    return total;
}

void sc_agent_destroy(sc_agent *agent)
{
    if (agent == nullptr) {
        return;
    }
    sc_vec_clear(&agent->tools);
    sc_history_clear(&agent->history);
    for (size_t i = 0; i < agent->session_histories.len; i += 1) {
        agent_session_history *session = sc_vec_at(&agent->session_histories, i);
        agent_session_history_clear(session);
    }
    sc_vec_clear(&agent->session_histories);
    sc_string_clear(&agent->model);
    sc_string_clear(&agent->identity);
    sc_string_clear(&agent->workspace);
    sc_string_clear(&agent->runtime_environment);
    sc_string_clear(&agent->memory_namespace);
    sc_string_clear(&agent->turn_namespace);
    sc_string_clear(&agent->cached_tool_specs_json);
    sc_free(agent->alloc, agent, sizeof(*agent), _Alignof(sc_agent));
}

void sc_agent_turn_result_init(sc_agent_turn_result *result, sc_allocator *alloc)
{
    if (result == nullptr) {
        return;
    }
    *result = (sc_agent_turn_result){.struct_size = sizeof(*result)};
    sc_receipt_chain_init(&result->receipts, alloc == nullptr ? sc_allocator_heap() : alloc);
    sc_vec_init(&result->events, alloc == nullptr ? sc_allocator_heap() : alloc, sizeof(sc_turn_event));
}

void sc_agent_turn_result_clear(sc_agent_turn_result *result)
{
    if (result == nullptr) {
        return;
    }
    sc_string_clear(&result->output);
    sc_string_clear(&result->active_model);
    sc_string_clear(&result->attachment_content_type);
    sc_string_clear(&result->attachment_filename);
    sc_bytes_clear(&result->attachment_bytes);
    sc_receipt_chain_clear(&result->receipts);
    for (size_t i = 0; i < result->events.len; i += 1) {
        sc_turn_event *event = sc_vec_at(&result->events, i);
        sc_turn_event_clear(event);
    }
    sc_vec_clear(&result->events);
    *result = (sc_agent_turn_result){0};
}

void sc_turn_event_clear(sc_turn_event *event)
{
    if (event == nullptr) {
        return;
    }
    sc_string_clear(&event->name);
    sc_string_clear(&event->message);
    *event = (sc_turn_event){0};
}

void sc_prompt_builder_init(sc_prompt_builder *builder, sc_allocator *alloc, size_t max_bytes)
{
    if (builder == nullptr) {
        return;
    }
    *builder = (sc_prompt_builder){.alloc = alloc == nullptr ? sc_allocator_heap() : alloc, .max_bytes = max_bytes};
    sc_string_builder_init(&builder->builder, builder->alloc);
}

sc_status sc_prompt_builder_append_section(sc_prompt_builder *builder, sc_str title, sc_str body)
{
    size_t used = 0;
    sc_str bounded_body = empty_if_null(body);
    sc_status status = sc_status_ok();

    if (builder == nullptr || title.ptr == nullptr || title.len == 0) {
        return sc_status_invalid_argument("sc.prompt_builder.invalid_argument");
    }
    used = builder->builder.bytes.len;
    if (builder->max_bytes > 0) {
        if (used >= builder->max_bytes) {
            return sc_status_ok();
        }
        size_t room = builder->max_bytes - used;
        if (bounded_body.len + title.len + 8 > room) {
            bounded_body.len = room > title.len + 8 ? room - title.len - 8 : 0;
        }
    }

    status = sc_string_builder_append_cstr(&builder->builder, "## ");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder->builder, title);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder->builder, "\n");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder->builder, bounded_body);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder->builder, "\n\n");
    }
    return status;
}

sc_status sc_prompt_builder_finish(sc_prompt_builder *builder, sc_string *out)
{
    if (builder == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.prompt_builder.invalid_argument");
    }
    return sc_string_builder_finish(&builder->builder, out);
}

void sc_prompt_builder_clear(sc_prompt_builder *builder)
{
    if (builder == nullptr) {
        return;
    }
    sc_string_builder_clear(&builder->builder);
    *builder = (sc_prompt_builder){0};
}

void sc_history_init(sc_history *history, sc_allocator *alloc, size_t max_messages)
{
    if (history == nullptr) {
        return;
    }
    history->alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_vec_init(&history->messages, history->alloc, sizeof(sc_history_message));
    history->max_messages = max_messages;
}

sc_status sc_history_append(sc_history *history, sc_str role, sc_str content)
{
    sc_history_message message = {.struct_size = sizeof(message)};
    sc_status status = sc_status_ok();

    if (history == nullptr || role.ptr == nullptr || role.len == 0) {
        return sc_status_invalid_argument("sc.history.invalid_argument");
    }
    status = sc_string_from_str(history->alloc, role, &message.role);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(history->alloc, empty_if_null(content), &message.content);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&history->messages, &message);
    }
    if (!sc_status_is_ok(status)) {
        history_message_clear(&message);
        return status;
    }
    sc_history_trim(history);
    return sc_status_ok();
}

sc_status sc_history_append_to_prompt(const sc_history *history, sc_prompt_builder *builder)
{
    sc_string_builder body = {0};
    sc_status status = sc_status_ok();
    bool previous_allows_tool_result = false;

    if (history == nullptr || builder == nullptr) {
        return sc_status_invalid_argument("sc.history.invalid_argument");
    }
    sc_string_builder_init(&body, history->alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < history->messages.len; i += 1) {
        const sc_history_message *message = sc_vec_at_const(&history->messages, i);
        if (message != nullptr) {
            sc_str role = sc_string_as_str(&message->role);
            if (history_role_is_tool_result(role) && !previous_allows_tool_result) {
                /* Tool results without the assistant request that caused them confuse provider transcripts. */
                continue;
            }
            status = append_line(&body, role, sc_string_as_str(&message->content));
            previous_allows_tool_result = history_role_is(role, "assistant") || history_role_is_tool_result(role);
        } else {
            previous_allows_tool_result = false;
        }
    }
    if (sc_status_is_ok(status)) {
        sc_string text = {0};
        status = sc_string_builder_finish(&body, &text);
        if (sc_status_is_ok(status)) {
            status = sc_prompt_builder_append_section(builder, sc_str_from_cstr("Recent history"), sc_string_as_str(&text));
        }
        sc_string_clear(&text);
    }
    sc_string_builder_clear(&body);
    return status;
}

size_t sc_history_estimated_chars(const sc_history *history)
{
    size_t total = 0;
    if (history == nullptr) {
        return 0;
    }
    for (size_t i = 0; i < history->messages.len; i += 1) {
        const sc_history_message *message = sc_vec_at_const(&history->messages, i);
        if (message != nullptr) {
            size_add_saturating(&total, history_message_estimated_chars(message));
        }
    }
    return total;
}

void sc_history_prune_orphans(sc_history *history)
{
    bool previous_allows_tool_result = false;

    if (history == nullptr) {
        return;
    }
    for (size_t i = 0; i < history->messages.len;) {
        sc_history_message *message = sc_vec_at(&history->messages, i);
        sc_str role = message == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&message->role);
        if (history_role_is_tool_result(role) && !previous_allows_tool_result) {
            history_remove_at(history, i);
            continue;
        }
        previous_allows_tool_result = history_role_is(role, "assistant") || history_role_is_tool_result(role);
        i += 1;
    }
}

sc_status sc_history_compress_to_recent(sc_history *history, size_t recent_messages, size_t max_summary_chars)
{
    sc_string_builder summary = {0};
    sc_string summary_text = {0};
    sc_history_message compressed = {.struct_size = sizeof(compressed)};
    size_t compress_count = 0;
    sc_status status = sc_status_ok();

    if (history == nullptr) {
        return sc_status_invalid_argument("sc.history.compress.invalid_argument");
    }
    sc_history_prune_orphans(history);
    if (recent_messages == 0) {
        recent_messages = 1;
    }
    if (history->messages.len <= recent_messages) {
        return sc_status_ok();
    }

    compress_count = history->messages.len - recent_messages;
    sc_string_builder_init(&summary, history->alloc);
    status = sc_string_builder_append_cstr(&summary, "Compressed earlier context. Keep these facts as background, but prefer newer messages:\n");
    for (size_t i = 0; sc_status_is_ok(status) && i < compress_count; i += 1) {
        const sc_history_message *message = sc_vec_at_const(&history->messages, i);
        sc_str role = message == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&message->role);
        sc_str content = message == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&message->content);
        size_t used = summary.bytes.len;
        size_t room = max_summary_chars > used ? max_summary_chars - used : 0;

        if (max_summary_chars > 0 && room <= 8) {
            break;
        }
        if (content.len > 180) {
            content.len = 180;
        }
        if (max_summary_chars > 0 && content.len + role.len + 8 > room) {
            content.len = room > role.len + 8 ? room - role.len - 8 : 0;
        }
        status = sc_string_builder_append_cstr(&summary, "- ");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&summary, role);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&summary, ": ");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&summary, content);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&summary, "\n");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&summary, &summary_text);
    } else {
        sc_string_builder_clear(&summary);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(history->alloc, "system", &compressed.role);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(history->alloc, sc_string_as_str(&summary_text), &compressed.content);
    }
    if (sc_status_is_ok(status)) {
        for (size_t i = 0; i < compress_count; i += 1) {
            history_remove_at(history, 0);
        }
        status = history_push_message(history, &compressed);
    }
    if (sc_status_is_ok(status)) {
        if (history->messages.len > 1) {
            const sc_history_message *last = sc_vec_at(&history->messages, history->messages.len - 1);
            if (last != nullptr) {
                /* Keep the synthetic summary first so later pruning treats it as background context. */
                sc_history_message saved = *last;
                (void)memmove((unsigned char *)history->messages.ptr + history->messages.item_size,
                              history->messages.ptr,
                              (history->messages.len - 1) * history->messages.item_size);
                *(sc_history_message *)history->messages.ptr = saved;
            }
        }
        compressed = (sc_history_message){0};
        sc_history_prune_orphans(history);
    }
    history_message_clear(&compressed);
    sc_string_clear(&summary_text);
    return status;
}

sc_status sc_history_prune_to_budget(sc_history *history, size_t max_chars)
{
    sc_status status;

    if (history == nullptr) {
        return sc_status_invalid_argument("sc.history.prune_budget.invalid_argument");
    }
    sc_history_prune_orphans(history);
    if (sc_history_estimated_chars(history) <= max_chars) {
        return sc_status_ok();
    }
    if (max_chars == 0) {
        while (history->messages.len > 0) {
            history_remove_at(history, 0);
        }
        return sc_status_ok();
    }
    if (history->messages.len > 1) {
        size_t recent_budget = max_chars - (max_chars / 4);
        size_t recent_messages = history_recent_messages_for_budget(history, recent_budget);
        size_t summary_budget = max_chars / 4;

        if (recent_messages >= history->messages.len) {
            recent_messages = history->messages.len - 1;
        }
        if (summary_budget < 128 && max_chars >= 128) {
            summary_budget = 128;
        }
        status = sc_history_compress_to_recent(history, recent_messages, summary_budget);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    while (sc_history_estimated_chars(history) > max_chars && history->messages.len > 0) {
        history_remove_at(history, 0);
    }
    sc_history_prune_orphans(history);
    return sc_status_ok();
}

void sc_history_trim(sc_history *history)
{
    if (history == nullptr || history->max_messages == 0) {
        return;
    }
    sc_history_prune_orphans(history);
    while (history->messages.len > history->max_messages) {
        history_remove_at(history, 0);
    }
}

void sc_history_clear(sc_history *history)
{
    if (history == nullptr) {
        return;
    }
    for (size_t i = 0; i < history->messages.len; i += 1) {
        sc_history_message *message = sc_vec_at(&history->messages, i);
        history_message_clear(message);
    }
    sc_vec_clear(&history->messages);
    *history = (sc_history){0};
}

static sc_status copy_option_string(sc_allocator *alloc, sc_str value, const char *fallback, sc_string *out)
{
    if (value.ptr == nullptr || value.len == 0) {
        value = sc_str_from_cstr(fallback);
    }
    return sc_string_from_str(alloc, value, out);
}

static sc_status agent_copy_tools(sc_agent *agent, sc_tool **tools, size_t tool_count)
{
    if (agent == nullptr || (tool_count > 0 && tools == nullptr)) {
        return sc_status_invalid_argument("sc.agent.tools.invalid_argument");
    }
    for (size_t i = 0; i < tool_count; i += 1) {
        if (tools[i] == nullptr) {
            return sc_status_invalid_argument("sc.agent.tools.null_tool");
        }
        sc_status status = sc_vec_push(&agent->tools, &tools[i]);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    return sc_status_ok();
}

static sc_status agent_history_for_turn(sc_agent *agent, const sc_turn *turn, sc_history **out)
{
    sc_str session_id = turn == nullptr ? sc_str_from_cstr("") : empty_if_null(turn->session_id);

    if (agent == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.agent.history.invalid_argument");
    }
    if (session_id.len == 0) {
        *out = &agent->history;
        return sc_status_ok();
    }
    return agent_history_for_session(agent, session_id, out);
}

static sc_status agent_history_for_session(sc_agent *agent, sc_str session_id, sc_history **out)
{
    agent_session_history session = {0};
    sc_status status = sc_status_ok();

    if (agent == nullptr || out == nullptr || session_id.ptr == nullptr || session_id.len == 0) {
        return sc_status_invalid_argument("sc.agent.history.invalid_argument");
    }
    for (size_t i = 0; i < agent->session_histories.len; i += 1) {
        agent_session_history *existing = sc_vec_at(&agent->session_histories, i);
        if (existing != nullptr && sc_str_equal(sc_string_as_str(&existing->session_id), session_id)) {
            *out = &existing->history;
            return sc_status_ok();
        }
    }

    status = sc_string_from_str(agent->alloc, session_id, &session.session_id);
    if (sc_status_is_ok(status)) {
        sc_history_init(&session.history, agent->alloc, agent->history.max_messages);
        status = sc_vec_push(&agent->session_histories, &session);
    }
    if (!sc_status_is_ok(status)) {
        agent_session_history_clear(&session);
        return status;
    }
    agent_session_history *stored = sc_vec_at(&agent->session_histories, agent->session_histories.len - 1);
    if (stored == nullptr) {
        return sc_status_invalid_argument("sc.agent.history.invalid_argument");
    }
    *out = &stored->history;
    return sc_status_ok();
}

static void agent_session_history_clear(agent_session_history *session)
{
    if (session == nullptr) {
        return;
    }
    sc_string_clear(&session->session_id);
    sc_history_clear(&session->history);
    *session = (agent_session_history){0};
}

static void history_message_clear(sc_history_message *message)
{
    if (message == nullptr) {
        return;
    }
    sc_string_clear(&message->role);
    sc_string_clear(&message->content);
    *message = (sc_history_message){0};
}

static sc_status result_add_event(sc_agent_turn_result *result,
                                  sc_turn_event_type type,
                                  sc_str name,
                                  sc_str message,
                                  sc_status_code status_code)
{
    sc_turn_event event = {.struct_size = sizeof(event), .type = type, .status_code = status_code};
    sc_status status = sc_status_ok();

    if (result == nullptr || result->events.item_size != sizeof(sc_turn_event)) {
        return sc_status_invalid_argument("sc.turn_event.invalid_argument");
    }
    status = sc_string_from_str(result->events.alloc, empty_if_null(name), &event.name);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(result->events.alloc, empty_if_null(message), &event.message);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&result->events, &event);
    }
    if (!sc_status_is_ok(status)) {
        sc_turn_event_clear(&event);
    }
    return status;
}

static sc_status turn_add_event(const sc_turn *turn,
                                sc_agent_turn_result *result,
                                sc_turn_event_type type,
                                sc_str name,
                                sc_str message,
                                sc_status_code status_code)
{
    size_t index = 0;
    sc_status status = sc_status_ok();

    if (result == nullptr) {
        return sc_status_invalid_argument("sc.turn_event.invalid_argument");
    }
    index = result->events.len;
    status = result_add_event(result, type, name, message, status_code);
    if (sc_status_is_ok(status) && turn != nullptr && turn->event_callback != nullptr && index < result->events.len) {
        const sc_turn_event *event = sc_vec_at_const(&result->events, index);
        if (event != nullptr) {
            turn->event_callback(turn->event_callback_user_data, event);
        }
    }
    return status;
}

static sc_status agent_emit_observer(sc_agent *agent, sc_str event_name, sc_str status_text)
{
    sc_log_field fields[] = {
        {.key = "status", .value = status_text, .secret = false},
    };
    sc_observer_event event = {
        .struct_size = sizeof(event),
        .target = sc_str_from_cstr("sc.agent"),
        .name = event_name,
        .fields = fields,
        .field_count = 1,
    };
    if (agent == nullptr || agent->observer == nullptr) {
        return sc_status_ok();
    }
    return sc_observer_emit_safe(agent->observer, &event, nullptr);
}

static sc_status load_memory_context(sc_agent *agent, const sc_turn *turn, sc_allocator *alloc, sc_string *out)
{
    sc_memory_query query = {0};
    sc_memory_result result = {0};
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (agent == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.agent.memory.invalid_argument");
    }
    if (agent->memory == nullptr || turn_memory_disabled(turn)) {
        return sc_string_from_cstr(alloc, "", out);
    }
    query = (sc_memory_query){
        .struct_size = sizeof(query),
        .namespace_name = sc_string_as_str(&agent->memory_namespace),
        .session_id = turn == nullptr ? sc_str_from_cstr("") : empty_if_null(turn->session_id),
        .query = turn == nullptr ? sc_str_from_cstr("") : empty_if_null(turn->input),
        .limit = agent->max_memory_entries,
        .include_superseded = false,
    };
    status = sc_memory_search(agent->memory, &query, alloc, &result);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < result.entries.len; i += 1) {
        const sc_memory_entry *entry = sc_vec_at_const(&result.entries, i);
        if (entry != nullptr && !sc_memory_key_is_reserved(sc_string_as_str(&entry->key))) {
            status = append_line(&builder, sc_string_as_str(&entry->key), sc_string_as_str(&entry->content));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    sc_string_builder_clear(&builder);
    sc_memory_result_clear(&result);
    return status;
}

static sc_status apply_model_switch(sc_agent *agent,
                                    const sc_turn *turn,
                                    const sc_model_switch_request *request,
                                    sc_allocator *alloc,
                                    sc_agent_turn_result *out)
{
    sc_string next_model = {0};
    sc_status status = sc_status_ok();

    if (agent == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.agent.model_switch.invalid_argument");
    }
    if (request == nullptr || request->model.ptr == nullptr || request->model.len == 0) {
        return sc_status_ok();
    }
    status = sc_string_from_str(agent->alloc, request->model, &next_model);
    if (sc_status_is_ok(status)) {
        sc_string_clear(&agent->model);
        agent->model = next_model;
        next_model = (sc_string){0};
        out->model_switched = true;
        status = sc_string_from_str(alloc, sc_string_as_str(&agent->model), &out->active_model);
    }
    if (sc_status_is_ok(status)) {
        sc_string_builder message = {0};
        sc_string text = {0};
        sc_string_builder_init(&message, alloc);
        status = sc_string_builder_append(&message, sc_string_as_str(&agent->model));
        if (sc_status_is_ok(status) && request->provider_name.len > 0) {
            status = sc_string_builder_append_cstr(&message, " provider=");
        }
        if (sc_status_is_ok(status) && request->provider_name.len > 0) {
            status = sc_string_builder_append(&message, request->provider_name);
        }
        if (sc_status_is_ok(status) && request->reason.len > 0) {
            status = sc_string_builder_append_cstr(&message, " reason=");
        }
        if (sc_status_is_ok(status) && request->reason.len > 0) {
            status = sc_string_builder_append(&message, request->reason);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&message, &text);
        } else {
            sc_string_builder_clear(&message);
        }
        if (sc_status_is_ok(status)) {
            status = turn_add_event(turn, out,
                                      SC_TURN_EVENT_MODEL_SWITCHED,
                                      sc_str_from_cstr("model"),
                                      sc_string_as_str(&text),
                                      SC_OK);
        }
        sc_string_clear(&text);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&next_model);
    }
    return status;
}

static sc_status build_prompt(sc_agent *agent,
                              const sc_turn *turn,
                              const sc_history *history,
                              sc_str memory_context,
                              sc_allocator *alloc,
                              sc_string *out)
{
    sc_prompt_builder builder = {0};
    sc_status status = sc_status_ok();
    sc_str workspace = {0};

    if (agent == nullptr || turn == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.agent.prompt.invalid_argument");
    }
    sc_prompt_builder_init(&builder, alloc, agent->max_prompt_bytes);
    workspace = agent->workspace.len > 0 ? sc_string_as_str(&agent->workspace) :
        (agent->policy == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&agent->policy->workspace_root));

    status = sc_prompt_builder_append_section(&builder, sc_str_from_cstr("Identity"), sc_string_as_str(&agent->identity));
    if (sc_status_is_ok(status)) {
        status = sc_prompt_builder_append_section(&builder,
                                                  sc_str_from_cstr("Safety and autonomy"),
                                                  sc_str_from_cstr("Follow security policy, approvals, and emergency stop state."));
    }
    if (sc_status_is_ok(status)) {
        status = sc_prompt_builder_append_section(&builder,
                                                  sc_str_from_cstr("Tool honesty"),
                                                  sc_str_from_cstr("Only claim tool results that are present in the turn context."));
    }
    if (sc_status_is_ok(status) && agent->policy != nullptr &&
        agent->policy->receipts_enabled && agent->policy->receipts_inject_system_prompt) {
        status = sc_prompt_builder_append_section(
            &builder,
            sc_str_from_cstr("Tool receipts"),
            sc_str_from_cstr("Use tool results, but do not include receipt tokens in user-visible responses."));
    }
    if (sc_status_is_ok(status)) {
        status = sc_prompt_builder_append_section(&builder, sc_str_from_cstr("Workspace"), workspace);
    }
    if (sc_status_is_ok(status)) {
        status = sc_prompt_builder_append_section(&builder,
                                                  sc_str_from_cstr("Runtime environment"),
                                                  sc_string_as_str(&agent->runtime_environment));
    }
    if (sc_status_is_ok(status)) {
        status = sc_prompt_builder_append_section(&builder, sc_str_from_cstr("Memory context"), memory_context);
    }
    if (sc_status_is_ok(status)) {
        status = sc_prompt_builder_append_section(&builder, sc_str_from_cstr("Skills"), sc_str_from_cstr("No skills loaded."));
    }
    if (sc_status_is_ok(status)) {
        status = sc_prompt_builder_append_section(&builder,
                                                  sc_str_from_cstr("Channel and media"),
                                                  sc_str_from_cstr("Text channel only."));
    }
    if (sc_status_is_ok(status) && agent->include_wall_time) {
        status = append_date_time_section(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = append_tool_specs_section(agent, turn, &builder);
    }
    if (sc_status_is_ok(status) && history != nullptr) {
        status = sc_history_append_to_prompt(history, &builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_prompt_builder_append_section(&builder, sc_str_from_cstr("User message"), turn->input);
    }
    if (sc_status_is_ok(status)) {
        status = sc_prompt_builder_finish(&builder, out);
    }
    sc_prompt_builder_clear(&builder);
    return status;
}

static sc_status prune_history_for_prompt_budget(const sc_agent *agent,
                                                 sc_history *history,
                                                 const sc_turn *turn,
                                                 sc_str memory_context,
                                                 sc_agent_turn_result *out)
{
    size_t before_len = history == nullptr ? 0 : history->messages.len;
    size_t before_chars = sc_history_estimated_chars(history);
    size_t budget = agent_history_prompt_budget(agent, turn, memory_context);
    sc_status status = sc_status_ok();

    if (history == nullptr) {
        return sc_status_ok();
    }
    status = sc_history_prune_to_budget(history, budget);
    if (sc_status_is_ok(status) &&
        (history->messages.len != before_len || sc_history_estimated_chars(history) != before_chars)) {
        status = turn_add_event(turn,
                                out,
                                SC_TURN_EVENT_PROMPT_BUILT,
                                sc_str_from_cstr("prompt.context_pruned"),
                                sc_str_from_cstr("budget"),
                                SC_OK);
    }
    return status;
}

static sc_status compact_history_for_overflow_retry(const sc_agent *agent,
                                                    sc_history *history,
                                                    const sc_turn *turn,
                                                    sc_str memory_context,
                                                    sc_agent_turn_result *out)
{
    size_t budget = agent_history_prompt_budget(agent, turn, memory_context);
    size_t overflow_budget = budget / 2;
    sc_status status = sc_status_ok();

    if (history == nullptr || history->messages.len == 0) {
        return sc_status_ok();
    }
    if (overflow_budget == 0 && budget > 0) {
        overflow_budget = 1;
    }
    if (history->messages.len > 1) {
        size_t recent_budget = overflow_budget == 0 ? 0 : overflow_budget / 2;
        size_t recent_messages = history_recent_messages_for_budget(history, recent_budget);
        size_t summary_budget = overflow_budget == 0 ? 0 : overflow_budget / 4;

        if (recent_messages >= history->messages.len) {
            recent_messages = history->messages.len - 1;
        }
        status = sc_history_compress_to_recent(history, recent_messages, summary_budget);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    status = sc_history_prune_to_budget(history, overflow_budget);
    if (sc_status_is_ok(status)) {
        status = turn_add_event(turn,
                                out,
                                SC_TURN_EVENT_PROMPT_BUILT,
                                sc_str_from_cstr("prompt.context_compacted"),
                                sc_str_from_cstr("overflow_retry"),
                                SC_OK);
    }
    return status;
}

static size_t agent_history_prompt_budget(const sc_agent *agent, const sc_turn *turn, sc_str memory_context)
{
    size_t max_prompt = agent == nullptr ? 0 : agent->max_prompt_bytes;
    size_t reserved = 1'024;
    size_t budget = 0;
    size_t max_history = 0;

    if (max_prompt == 0) {
        return 0;
    }
    if (agent != nullptr) {
        size_t tool_budget = 0;
        size_add_saturating(&reserved, agent->identity.len);
        size_add_saturating(&reserved, agent->workspace.len);
        size_add_saturating(&reserved, agent->runtime_environment.len);
        tool_budget = agent->cached_tool_specs_json.len;
        if (tool_budget == 0 && sc_size_mul_overflow(agent->tools.len, 384, &tool_budget)) {
            tool_budget = SIZE_MAX;
        }
        size_add_saturating(&reserved, tool_budget);
    }
    if (turn != nullptr) {
        size_add_saturating(&reserved, empty_if_null(turn->input).len);
        size_add_saturating(&reserved, empty_if_null(turn->media_context).len);
    }
    size_add_saturating(&reserved, empty_if_null(memory_context).len);
    if (reserved >= max_prompt) {
        return 0;
    }
    budget = max_prompt - reserved;
    max_history = max_prompt / 2;
    return budget > max_history ? max_history : budget;
}

static size_t history_recent_messages_for_budget(const sc_history *history, size_t max_chars)
{
    size_t count = 0;
    size_t used = 0;

    if (history == nullptr || history->messages.len == 0) {
        return 0;
    }
    for (size_t i = history->messages.len; i > 0; i -= 1) {
        const sc_history_message *message = sc_vec_at_const(&history->messages, i - 1);
        size_t cost = history_message_estimated_chars(message);
        size_t next = used;

        size_add_saturating(&next, cost);
        if (count > 0 && max_chars > 0 && next > max_chars) {
            break;
        }
        used = next;
        count += 1;
        if (max_chars == 0) {
            break;
        }
    }
    return count == 0 ? 1 : count;
}

static size_t history_message_estimated_chars(const sc_history_message *message)
{
    size_t total = 0;

    if (message == nullptr) {
        return 0;
    }
    size_add_saturating(&total, message->role.len);
    size_add_saturating(&total, message->content.len);
    size_add_saturating(&total, 2);
    return total;
}

static void size_add_saturating(size_t *target, size_t value)
{
    size_t next = 0;

    if (target == nullptr) {
        return;
    }
    if (sc_size_add_overflow(*target, value, &next)) {
        *target = SIZE_MAX;
        return;
    }
    *target = next;
}

static sc_status append_date_time_section(sc_prompt_builder *builder)
{
    sc_wall_time now = {0};
    sc_string formatted = {0};
    sc_status status = sc_clock_wall(&now);
    if (sc_status_is_ok(status)) {
        status = sc_time_format_rfc3339(builder == nullptr ? sc_allocator_heap() : builder->alloc, now, &formatted);
    }
    if (sc_status_is_ok(status)) {
        status = sc_prompt_builder_append_section(builder, sc_str_from_cstr("Date and time"), sc_string_as_str(&formatted));
    }
    sc_string_clear(&formatted);
    return status;
}

static sc_status append_tool_specs_section(sc_agent *agent, const sc_turn *turn, sc_prompt_builder *builder)
{
    sc_string serialized = {0};
    sc_str specs_json = {0};
    sc_status status = sc_status_ok();

    if (agent == nullptr || builder == nullptr) {
        return sc_status_invalid_argument("sc.agent.tool_specs.invalid_argument");
    }
    status = tool_specs_json_for_turn(agent, turn, builder->alloc, &serialized, &specs_json);
    if (sc_status_is_ok(status)) {
        status = sc_prompt_builder_append_section(builder, sc_str_from_cstr("Tools"), specs_json);
    }
    sc_string_clear(&serialized);
    return status;
}

static sc_status tool_specs_json_for_turn(sc_agent *agent,
                                          const sc_turn *turn,
                                          sc_allocator *alloc,
                                          sc_string *owned,
                                          sc_str *out)
{
    sc_json_value *specs = nullptr;
    sc_status status = sc_status_ok();

    if (agent == nullptr || owned == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.agent.tool_specs.invalid_argument");
    }
    *out = sc_str_from_cstr("");
    if (turn_uses_default_tool_specs(turn) &&
        agent->cached_tool_specs_json.ptr != nullptr &&
        agent->cached_tool_specs_tool_count == agent->tools.len) {
        *out = sc_string_as_str(&agent->cached_tool_specs_json);
        return sc_status_ok();
    }

    if (turn_uses_default_tool_specs(turn)) {
        status = tool_specs_for_turn(agent, turn, agent->alloc, &specs);
        if (sc_status_is_ok(status)) {
            sc_string_clear(&agent->cached_tool_specs_json);
            status = sc_json_serialize(specs, agent->alloc, &agent->cached_tool_specs_json);
        }
        if (sc_status_is_ok(status)) {
            agent->cached_tool_specs_tool_count = agent->tools.len;
            *out = sc_string_as_str(&agent->cached_tool_specs_json);
        }
        sc_json_destroy(specs);
        return status;
    }

    status = tool_specs_for_turn(agent, turn, alloc, &specs);
    if (sc_status_is_ok(status)) {
        status = sc_json_serialize(specs, alloc, owned);
    }
    if (sc_status_is_ok(status)) {
        *out = sc_string_as_str(owned);
    }
    sc_json_destroy(specs);
    return status;
}

static bool turn_uses_default_tool_specs(const sc_turn *turn)
{
    return turn == nullptr ||
        (!turn_memory_disabled(turn) &&
         (turn->allowed_tools == nullptr || turn->allowed_tool_count == 0) &&
         (turn->denied_tools == nullptr || turn->denied_tool_count == 0) &&
         !turn->autonomy_override_set);
}

static bool turn_memory_disabled(const sc_turn *turn)
{
    return turn != nullptr && turn->disable_memory;
}

static bool turn_cancel_requested(const sc_turn *turn)
{
    return turn != nullptr && (turn->cancel_requested || (turn->cancel_token != nullptr && turn->cancel_token->cancel_requested));
}

static bool turn_deadline_expired(int64_t deadline_ns)
{
    sc_instant now = {0};
    if (deadline_ns <= 0 || !sc_status_is_ok(sc_clock_monotonic(&now))) {
        return false;
    }
    return now.ns >= deadline_ns;
}

static sc_status turn_check_continue(const sc_turn *turn, int64_t deadline_ns)
{
    if (turn_cancel_requested(turn)) {
        return sc_status_cancelled("sc.agent.turn.cancelled");
    }
    if (turn_deadline_expired(deadline_ns)) {
        return sc_status_timeout("sc.agent.turn.timeout");
    }
    return sc_status_ok();
}

static sc_status run_provider_loop(sc_agent *agent,
                                   const sc_turn *turn,
                                   sc_str initial_prompt,
                                   int64_t deadline_ns,
                                   sc_allocator *scratch_alloc,
                                   sc_allocator *result_alloc,
                                   sc_agent_turn_result *out)
{
    sc_string_builder output = {0};
    sc_string_builder prompt = {0};
    sc_string owned_tool_specs_json = {0};
    sc_vec approval_denials = {0};
    sc_vec approval_grants = {0};
    sc_vec successful_tools = {0};
    sc_vec terminal_failures = {0};
    sc_str tool_specs_json = {0};
    sc_status status = sc_status_ok();
    size_t iterations = 0;

    scratch_alloc = scratch_alloc == nullptr ? sc_allocator_heap() : scratch_alloc;
    result_alloc = result_alloc == nullptr ? sc_allocator_heap() : result_alloc;
    /*
     * These caches are scoped to one provider loop. They prevent repeated
     * approval prompts, repeated terminal failures, and repeated successful
     * attachment-producing tool calls from cycling indefinitely.
     */
    sc_vec_init(&approval_denials, scratch_alloc, sizeof(tool_approval_denial));
    sc_vec_init(&approval_grants, scratch_alloc, sizeof(tool_approval_grant));
    sc_vec_init(&successful_tools, scratch_alloc, sizeof(tool_success_cache));
    sc_vec_init(&terminal_failures, scratch_alloc, sizeof(tool_terminal_failure));
    sc_string_builder_init(&output, result_alloc);
    sc_string_builder_init(&prompt, scratch_alloc);
    status = sc_string_builder_append(&prompt, initial_prompt);
    if (sc_status_is_ok(status)) {
        status = tool_specs_json_for_turn(agent, turn, scratch_alloc, &owned_tool_specs_json, &tool_specs_json);
    }

    while (sc_status_is_ok(status)) {
        sc_provider_request request = {
            .struct_size = sizeof(request),
            .model = sc_string_as_str(&agent->model),
            .prompt = sc_str_from_parts((const char *)prompt.bytes.ptr, prompt.bytes.len),
            .system_instruction = sc_string_as_str(&agent->identity),
            .tool_specs_json = tool_specs_json,
            .media = turn == nullptr ? nullptr : turn->media,
            .media_count = turn == nullptr ? 0 : turn->media_count,
            .media_context = turn == nullptr ? sc_str_from_cstr("") : turn->media_context,
            .cancel_requested = turn_cancel_requested(turn),
            .route_hint = turn == nullptr ? sc_str_from_cstr("") : turn->route_hint,
        };
        sc_provider_response response = {0};
        sc_string_builder feedback = {0};
        bool repeated_success_stop = false;

        if (iterations >= agent->max_tool_iterations) {
            status = sc_status_cancelled("sc.agent.tool_loop.max_iterations");
            break;
        }
        status = turn_check_continue(turn, deadline_ns);
        if (!sc_status_is_ok(status)) {
            break;
        }
        iterations += 1;
        out->provider_call_count += 1;
        status = turn_add_event(turn, out,
                                  SC_TURN_EVENT_PROVIDER_STARTED,
                                  sc_str_from_cstr("provider"),
                                  sc_str_from_cstr("started"),
                                  SC_OK);
        if (sc_status_is_ok(status)) {
            status = agent_emit_observer(agent, sc_str_from_cstr("runtime.provider.request.started"), sc_str_from_cstr("ok"));
        }
        if (sc_status_is_ok(status)) {
            status = provider_request_once(agent, turn, &request, scratch_alloc, out, &response);
        }
        if (sc_status_is_ok(status)) {
            status = recover_text_tool_calls(&response, scratch_alloc);
        }
        if (sc_status_is_ok(status)) {
            result_add_usage(out, &response);
            status = turn_add_event(turn, out,
                                      SC_TURN_EVENT_PROVIDER_CALLED,
                                      sc_str_from_cstr("provider"),
                                      sc_str_from_cstr("completed"),
                                      SC_OK);
        }
        if (sc_status_is_ok(status)) {
            status = agent_emit_observer(agent, sc_str_from_cstr("runtime.provider.completed"), sc_str_from_cstr("ok"));
        }
        if (sc_status_is_ok(status) && response.malformed_tool_call) {
            status = sc_status_parse("sc.agent.provider.malformed_tool_call");
        }
        if (sc_status_is_ok(status) && result_exceeds_budget(turn, out)) {
            out->budget_exceeded = true;
            status = sc_status_cancelled("sc.agent.budget_exceeded");
        }
        if (sc_status_is_ok(status) && response.text.len > 0) {
            status = sc_string_builder_append(&output, sc_string_as_str(&response.text));
            if (sc_status_is_ok(status)) {
                status = turn_add_event(turn, out,
                                          SC_TURN_EVENT_TEXT_DELTA,
                                          sc_str_from_cstr("assistant"),
                                          sc_string_as_str(&response.text),
                                          SC_OK);
            }
        }
        if (!sc_status_is_ok(status) || response.tool_calls.len == 0) {
            sc_provider_response_clear(&response);
            break;
        }

        sc_string_builder_init(&feedback, scratch_alloc);
        status = execute_tool_calls(agent,
                                    turn,
                                    &response,
                                    deadline_ns,
                                    scratch_alloc,
                                    result_alloc,
                                    out,
                                    &feedback,
                                    &approval_denials,
                                    &approval_grants,
                                    &successful_tools,
                                    &repeated_success_stop,
                                    &terminal_failures);
        if (sc_status_is_ok(status) && repeated_success_stop) {
            sc_str fallback = out->attachment_bytes.len > 0 ?
                sc_str_from_cstr("The requested tool result is attached.") :
                sc_str_from_cstr("The requested tool action completed.");
            if (output.bytes.len == 0) {
                status = sc_string_builder_append(&output, fallback);
                if (sc_status_is_ok(status)) {
                    status = turn_add_event(turn,
                                            out,
                                            SC_TURN_EVENT_TEXT_DELTA,
                                            sc_str_from_cstr("assistant"),
                                            fallback,
                                            SC_OK);
                }
            }
            sc_string_builder_clear(&feedback);
            sc_provider_response_clear(&response);
            break;
        }
        if (sc_status_is_ok(status)) {
            sc_string feedback_text = {0};
            status = sc_string_builder_finish(&feedback, &feedback_text);
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&prompt, "\n## Tool results\n");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&prompt, sc_string_as_str(&feedback_text));
            }
            sc_string_clear(&feedback_text);
        }
        sc_string_builder_clear(&feedback);
        sc_provider_response_clear(&response);
    }

    approval_denials_clear(&approval_denials);
    approval_grants_clear(&approval_grants);
    tool_success_cache_clear(&successful_tools);
    tool_terminal_failures_clear(&terminal_failures);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&output, &out->output);
    }
    sc_string_clear(&owned_tool_specs_json);
    sc_string_builder_clear(&output);
    sc_string_builder_clear(&prompt);
    return status;
}

static sc_status execute_tool_calls(sc_agent *agent,
                                    const sc_turn *turn,
                                    const sc_provider_response *response,
                                    int64_t deadline_ns,
                                    sc_allocator *alloc,
                                    sc_allocator *result_alloc,
                                    sc_agent_turn_result *out,
                                    sc_string_builder *feedback,
                                    sc_vec *approval_denials,
                                    sc_vec *approval_grants,
                                    sc_vec *successful_tools,
                                    bool *out_repeated_success_stop,
                                    sc_vec *terminal_failures)
{
    sc_status status = sc_status_ok();

    if (agent == nullptr || response == nullptr || alloc == nullptr || result_alloc == nullptr ||
        out == nullptr || feedback == nullptr ||
        approval_denials == nullptr || approval_grants == nullptr || successful_tools == nullptr ||
        out_repeated_success_stop == nullptr || terminal_failures == nullptr) {
        return sc_status_invalid_argument("sc.agent.tool_loop.invalid_argument");
    }
    *out_repeated_success_stop = false;

    for (size_t i = 0;
         sc_status_is_ok(status) && !*out_repeated_success_stop && i < response->tool_calls.len;
         i += 1) {
        const sc_provider_tool_call *provider_call = sc_vec_at_const(&response->tool_calls, i);
        sc_tool *tool = provider_call == nullptr || !turn_allows_tool(turn, sc_string_as_str(&provider_call->name)) ?
            nullptr :
            find_tool_by_name(agent, sc_string_as_str(&provider_call->name));
        sc_tool_call call = {0};
        sc_tool_result tool_result = {0};
        sc_tool_spec spec = {0};
        sc_string args_summary = {0};
        sc_string output_summary = {0};
        sc_status tool_status;
        bool used_success_cache = false;

        tool_status = turn_check_continue(turn, deadline_ns);
        if (!sc_status_is_ok(tool_status)) {
            sc_status_code code = tool_status.code;
            sc_status_clear(&tool_status);
            return code == SC_ERR_TIMEOUT ? sc_status_timeout("sc.agent.tool_loop.timeout") :
                                            sc_status_cancelled("sc.agent.tool_loop.cancelled");
        }
        if (provider_call != nullptr && !turn_allows_tool(turn, sc_string_as_str(&provider_call->name))) {
            tool_status = sc_status_security_denied("sc.agent.tool_loop.filtered_tool");
        } else if (tool == nullptr || provider_call == nullptr) {
            tool_status = sc_status_invalid_argument("sc.agent.tool_loop.unknown_tool");
        } else if (tool_terminal_failures_contains(terminal_failures, provider_call)) {
            tool_status = sc_status_unsupported("sc.agent.tool_loop.repeated_terminal_failure");
        } else {
            tool_success_cache *cached_success = tool_success_cache_find(successful_tools, provider_call);
            if (cached_success == nullptr) {
                /*
                 * Some providers ask for the same attachment by name after a
                 * successful tool call; replay once, then stop the loop.
                 */
                cached_success = tool_success_cache_find_attachment_by_name(successful_tools, provider_call);
                if (cached_success != nullptr) {
                    *out_repeated_success_stop = true;
                }
            }
            if (cached_success != nullptr) {
                used_success_cache = true;
                if (cached_success->replay_count > 0) {
                    *out_repeated_success_stop = true;
                }
                tool_status = tool_result_from_success_cache(alloc, cached_success, &tool_result);
                if (sc_status_is_ok(tool_status)) {
                    cached_success->replay_count += 1;
                }
            } else {
                tool_status = sc_tool_spec_get(tool, &spec);
                if (sc_status_is_ok(tool_status) && !turn_autonomy_allows_spec(turn, &spec)) {
                    tool_status = sc_status_security_denied("sc.agent.tool_loop.autonomy_denied");
                }
                if (sc_status_is_ok(tool_status)) {
                    tool_status = sc_security_validate_prompt_injection(sc_string_as_str(&provider_call->arguments_json));
                }
                if (sc_status_is_ok(tool_status)) {
                    tool_status = validate_tool_call_args(&spec, provider_call);
                }
                if (sc_status_is_ok(tool_status) && approval_denials_contains(approval_denials, provider_call)) {
                    tool_status = sc_status_security_denied("sc.tool.approval_declined");
                }
                if (sc_status_is_ok(tool_status)) {
                    tool_status = sc_provider_tool_call_as_tool_call(provider_call, &call);
                }
                if (sc_status_is_ok(tool_status)) {
                    if (approval_grants_contains(approval_grants, provider_call)) {
                        /* The exact call was already approved during this turn, so bypass a second prompt. */
                        sc_tool_approval_override_set(sc_string_as_str(&provider_call->name));
                        tool_status = sc_tool_invoke(tool, &call, alloc, &tool_result);
                        sc_tool_approval_override_clear();
                    } else {
                        tool_status = sc_tool_invoke(tool, &call, alloc, &tool_result);
                    }
                    if (tool_status_is_approval_required(tool_status)) {
                        bool approved = false;
                        sc_status approval_status;
                        sc_status_clear(&tool_status);
                        sc_tool_result_clear(&tool_result);
                        approval_status = request_turn_tool_approval(turn, provider_call, alloc, &approved);
                        if (!sc_status_is_ok(approval_status)) {
                            tool_status = approval_status;
                        } else if (!approved) {
                            tool_status = approval_denials_add(approval_denials, alloc, provider_call);
                            if (sc_status_is_ok(tool_status)) {
                                tool_status = sc_status_security_denied("sc.tool.approval_declined");
                            }
                        } else {
                            tool_status = approval_grants_add(approval_grants, alloc, provider_call);
                            if (sc_status_is_ok(tool_status)) {
                                sc_tool_approval_override_set(sc_string_as_str(&provider_call->name));
                                tool_status = sc_tool_invoke(tool, &call, alloc, &tool_result);
                                sc_tool_approval_override_clear();
                            }
                        }
                    }
                }
            }
        }
        if (sc_status_is_ok(status) && tool_status_is_turn_terminal(tool_status)) {
            status = tool_terminal_failures_add(terminal_failures, alloc, provider_call, tool_status);
        }
        if (sc_status_is_ok(status) && !used_success_cache && sc_status_is_ok(tool_status) && tool_result.success) {
            status = tool_success_cache_add(successful_tools, alloc, provider_call, &tool_result);
        }

        out->tool_call_count += 1;
        if (sc_status_is_ok(status)) {
            status = turn_add_event(turn, out,
                                    SC_TURN_EVENT_TOOL_CALLED,
                                    provider_call == nullptr ? sc_str_from_cstr("unknown") : sc_string_as_str(&provider_call->name),
                                    sc_status_is_ok(tool_status) ? sc_str_from_cstr("requested") : sc_str_from_cstr("denied_or_failed"),
                                    tool_status.code);
        }
        if (sc_status_is_ok(status) && !sc_status_is_ok(tool_status) &&
            (tool_status.code == SC_ERR_SECURITY_DENIED || tool_status.code == SC_ERR_INVALID_ARGUMENT ||
             (tool_status.code == SC_ERR_CANCELLED && tool_status.error_key != nullptr &&
              strcmp(tool_status.error_key, "sc.tool.approval_required") == 0))) {
            status = turn_add_event(turn, out,
                                      SC_TURN_EVENT_TOOL_DENIED,
                                      provider_call == nullptr ? sc_str_from_cstr("unknown") : sc_string_as_str(&provider_call->name),
                                      sc_str_from_cstr(tool_status.error_key == nullptr ? "tool_denied" : tool_status.error_key),
                                      tool_status.code);
        }
        if (sc_status_is_ok(status)) {
            status = turn_add_event(turn, out,
                                      SC_TURN_EVENT_TOOL_RESULT,
                                      provider_call == nullptr ? sc_str_from_cstr("unknown") : sc_string_as_str(&provider_call->name),
                                      sc_status_is_ok(tool_status) ? sc_str_from_cstr("completed") :
                                          sc_str_from_cstr(tool_status.error_key == nullptr ? "tool_error" : tool_status.error_key),
                                      tool_status.code);
        }
        if (sc_status_is_ok(status)) {
            status = agent_emit_observer(agent,
                                         sc_status_is_ok(tool_status) ? sc_str_from_cstr("runtime.tool.completed") :
                                                                       sc_str_from_cstr("runtime.tool.denied"),
                                         sc_status_is_ok(tool_status) ? sc_str_from_cstr("ok") :
                                                                       sc_str_from_cstr(tool_status.error_key == nullptr ? "error" :
                                                                                                        tool_status.error_key));
        }
        if (sc_status_is_ok(status)) {
            status = receipt_summary_for_turn(alloc,
                                              provider_call == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&provider_call->arguments_json),
                                              &args_summary);
        }
        if (sc_status_is_ok(status)) {
            status = receipt_summary_for_turn(alloc,
                                              sc_status_is_ok(tool_status) ? sc_string_as_str(&tool_result.output) :
                                                  sc_str_from_cstr(tool_status.error_key == nullptr ? "tool_error" : tool_status.error_key),
                                              &output_summary);
        }
        if (sc_status_is_ok(status) && agent_receipts_enabled(agent)) {
            status = sc_receipt_chain_append_ex(&out->receipts,
                                                provider_call == nullptr ? sc_str_from_cstr("unknown") :
                                                                           sc_string_as_str(&provider_call->name),
                                                sc_string_as_str(&args_summary),
                                                sc_string_as_str(&output_summary),
                                                sc_status_is_ok(tool_status) && tool_result.success,
                                                tool_policy_decision(tool_status),
                                                tool_failure_reason(tool_status),
                                                tool_outcome(tool_status, sc_status_is_ok(tool_status) && tool_result.success));
        }
        if (sc_status_is_ok(status) && sc_status_is_ok(tool_status) && tool_result.success) {
            status = result_copy_tool_attachment(result_alloc, out, &tool_result);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(feedback, "tool=");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(feedback,
                                              provider_call == nullptr ? sc_str_from_cstr("unknown") : sc_string_as_str(&provider_call->name));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(feedback, " success=");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(feedback, sc_status_is_ok(tool_status) && tool_result.success ? "true" : "false");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(feedback, " output=");
        }
        if (sc_status_is_ok(status)) {
            status = append_bounded(feedback,
                                    sc_status_is_ok(tool_status) ? sc_string_as_str(&tool_result.output) :
                                        sc_str_from_cstr(tool_status.error_key == nullptr ? "tool_error" : tool_status.error_key),
                                    agent->max_tool_output_bytes);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(feedback, "\n");
        }
        if (!sc_status_is_ok(tool_status) && tool_status.code == SC_ERR_CANCELLED) {
            sc_status_clear(&tool_status);
            sc_tool_result_clear(&tool_result);
            sc_string_clear(&args_summary);
            sc_string_clear(&output_summary);
            return sc_status_cancelled("sc.agent.tool_loop.cancelled");
        }
        sc_status_clear(&tool_status);
        sc_tool_result_clear(&tool_result);
        sc_string_clear(&args_summary);
        sc_string_clear(&output_summary);
    }
    return status;
}

static sc_status provider_request_once(sc_agent *agent,
                                       const sc_turn *turn,
                                       const sc_provider_request *request,
                                       sc_allocator *alloc,
                                       sc_agent_turn_result *out,
                                       sc_provider_response *response)
{
    bool use_streaming = (turn != nullptr && turn->use_streaming) || (agent != nullptr && agent->use_streaming);
    runtime_stream_collect collect = {0};
    sc_status status = sc_status_ok();

    if (agent == nullptr || request == nullptr || response == nullptr) {
        return sc_status_invalid_argument("sc.agent.provider.invalid_argument");
    }
    if (!use_streaming) {
        return sc_provider_generate(agent->provider, request, alloc, response);
    }
    sc_provider_response_init(response, alloc);
    collect = (runtime_stream_collect){
        .alloc = alloc,
        .agent = agent,
        .turn = turn,
        .result = out,
        .response = response,
    };
    status = sc_provider_stream(agent->provider, request, alloc, runtime_stream_callback, &collect);
    if (sc_status_is_ok(status) && !response->streaming_complete) {
        status = sc_status_parse("sc.agent.provider_stream.incomplete");
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_response_clear(response);
    }
    return status;
}

static sc_status runtime_stream_callback(void *user_data, const sc_provider_stream_event *event)
{
    runtime_stream_collect *collect = user_data;
    sc_status status = sc_status_ok();

    if (collect == nullptr || collect->response == nullptr || event == nullptr) {
        return sc_status_invalid_argument("sc.agent.provider_stream.invalid_argument");
    }
    if (turn_cancel_requested(collect->turn)) {
        return sc_status_cancelled("sc.agent.provider_stream.cancelled");
    }
    switch (event->type) {
    case SC_PROVIDER_STREAM_DELTA:
    {
        sc_string_builder builder = {0};
        sc_string next = {0};
        sc_string_builder_init(&builder, collect->alloc);
        status = sc_string_builder_append(&builder, sc_string_as_str(&collect->response->text));
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&event->text));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &next);
            if (sc_status_is_ok(status)) {
                sc_string_clear(&collect->response->text);
                collect->response->text = next;
            }
        } else {
            sc_string_builder_clear(&builder);
        }
        if (sc_status_is_ok(status) && collect->result != nullptr &&
            ((collect->turn != nullptr && collect->turn->emit_stream_deltas) ||
             (collect->agent != nullptr && collect->agent->emit_stream_deltas))) {
            status = turn_add_event(collect->turn,
                                    collect->result,
                                    SC_TURN_EVENT_TEXT_DELTA,
                                    sc_str_from_cstr("assistant"),
                                    sc_str_from_cstr("delta"),
                                    SC_OK);
        }
        break;
    }
    case SC_PROVIDER_STREAM_REASONING_DELTA:
        if (collect->turn != nullptr && collect->turn->show_reasoning) {
            sc_string_builder builder = {0};
            sc_string next = {0};
            sc_string_builder_init(&builder, collect->alloc);
            status = sc_string_builder_append(&builder, sc_string_as_str(&collect->response->text));
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, sc_string_as_str(&event->text));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_finish(&builder, &next);
                if (sc_status_is_ok(status)) {
                    sc_string_clear(&collect->response->text);
                    collect->response->text = next;
                }
            } else {
                sc_string_builder_clear(&builder);
            }
        } else {
            sc_string_builder builder = {0};
            sc_string next = {0};
            sc_string_builder_init(&builder, collect->alloc);
            status = sc_string_builder_append(&builder, sc_string_as_str(&collect->response->reasoning_text));
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&builder, sc_string_as_str(&event->text));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_finish(&builder, &next);
                if (sc_status_is_ok(status)) {
                    sc_string_clear(&collect->response->reasoning_text);
                    collect->response->reasoning_text = next;
                }
            } else {
                sc_string_builder_clear(&builder);
            }
        }
        break;
    case SC_PROVIDER_STREAM_TOOL_CALL:
        status = sc_provider_response_add_tool_call(collect->response, &event->tool_call);
        break;
    case SC_PROVIDER_STREAM_PRE_EXECUTED_TOOL_CALL:
        sc_provider_tool_call_clear(&collect->response->pre_executed_tool_call);
        status = sc_string_from_str(collect->alloc,
                                    sc_string_as_str(&event->tool_call.call_id),
                                    &collect->response->pre_executed_tool_call.call_id);
        if (sc_status_is_ok(status)) {
            status = sc_string_from_str(collect->alloc,
                                        sc_string_as_str(&event->tool_call.name),
                                        &collect->response->pre_executed_tool_call.name);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_from_str(collect->alloc,
                                        sc_string_as_str(&event->tool_call.arguments_json),
                                        &collect->response->pre_executed_tool_call.arguments_json);
        }
        if (sc_status_is_ok(status) && event->tool_call.arguments != nullptr) {
            status = sc_json_clone(event->tool_call.arguments,
                                   collect->alloc,
                                   &collect->response->pre_executed_tool_call.arguments);
        }
        collect->response->pre_executed_tool_call.struct_size = sizeof(collect->response->pre_executed_tool_call);
        break;
    case SC_PROVIDER_STREAM_PRE_EXECUTED_TOOL_RESULT:
        sc_string_clear(&collect->response->pre_executed_tool_result);
        status = sc_string_from_str(collect->alloc,
                                    sc_string_as_str(&event->text),
                                    &collect->response->pre_executed_tool_result);
        break;
    case SC_PROVIDER_STREAM_FINAL_USAGE:
        collect->response->input_tokens = event->input_tokens;
        collect->response->output_tokens = event->output_tokens;
        collect->response->total_tokens = event->total_tokens;
        collect->response->cost_usd = event->cost_usd;
        break;
    case SC_PROVIDER_STREAM_DONE:
        collect->response->streaming_complete = true;
        break;
    case SC_PROVIDER_STREAM_ERROR:
        status = sc_status_parse("sc.agent.provider_stream.error");
        break;
    default:
        status = sc_status_parse("sc.agent.provider_stream.unknown_event");
        break;
    }
    return status;
}

static sc_status agent_async_start(agent_async_turn *state)
{
    sc_status status = sc_status_ok();
    sc_instant started = {0};

    if (state == nullptr || state->agent == nullptr || state->turn == nullptr) {
        return sc_status_invalid_argument("sc.agent.async.invalid_argument");
    }
    sc_arena_init(&state->scratch,
                  state->alloc,
                  state->agent->max_prompt_bytes == 0 ? 16'384 : state->agent->max_prompt_bytes);
    state->scratch_alloc = sc_arena_allocator(&state->scratch);
    sc_vec_init(&state->terminal_failures, state->scratch_alloc, sizeof(tool_terminal_failure));
    sc_agent_turn_result_init(&state->result, state->alloc);
    state->result.turn_id = state->turn->turn_id;
    status = agent_history_for_turn(state->agent, state->turn, &state->history);
    if ((state->turn->timeout_ms > 0 || state->agent->default_timeout_ms > 0) && sc_status_is_ok(sc_clock_monotonic(&started))) {
        uint32_t timeout_ms = state->turn->timeout_ms > 0 ? state->turn->timeout_ms : state->agent->default_timeout_ms;
        state->deadline_ns = started.ns + (int64_t)timeout_ms * 1'000'000;
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (turn_cancel_requested(state->turn)) {
        state->result.cancelled = true;
        (void)turn_add_event(state->turn, &state->result,
                               SC_TURN_EVENT_CANCELLED,
                               sc_str_from_cstr("cancelled"),
                               sc_str_from_cstr("input"),
                               SC_ERR_CANCELLED);
        return sc_status_cancelled("sc.agent.cancelled");
    }

    status = turn_add_event(state->turn, &state->result, SC_TURN_EVENT_STARTED, sc_str_from_cstr("turn"), sc_str_from_cstr("started"), SC_OK);
    if (sc_status_is_ok(status)) {
        status = agent_emit_observer(state->agent, sc_str_from_cstr("runtime.turn.started"), sc_str_from_cstr("ok"));
    }
    if (sc_status_is_ok(status)) {
        status = apply_model_switch(state->agent, state->turn, state->turn->model_switch, state->alloc, &state->result);
    }
    if (sc_status_is_ok(status)) {
        status = turn_add_event(state->turn, &state->result, SC_TURN_EVENT_INPUT_VALIDATED, sc_str_from_cstr("input"), state->turn->input, SC_OK);
    }
    if (sc_status_is_ok(status)) {
        status = agent_emit_observer(state->agent, sc_str_from_cstr("turn.input_validated"), sc_str_from_cstr("ok"));
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(state->turn)) {
        status = load_memory_context(state->agent, state->turn, state->scratch_alloc, &state->memory_context);
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(state->turn)) {
        status = turn_add_event(state->turn, &state->result,
                                  SC_TURN_EVENT_MEMORY_LOADED,
                                  sc_str_from_cstr("memory"),
                                  sc_str_from_cstr("loaded"),
                                  SC_OK);
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(state->turn)) {
        status = agent_emit_observer(state->agent, sc_str_from_cstr("runtime.memory.read"), sc_str_from_cstr("ok"));
    }
    if (sc_status_is_ok(status)) {
        status = turn_check_continue(state->turn, state->deadline_ns);
    }
    if (sc_status_is_ok(status)) {
        status = prune_history_for_prompt_budget(state->agent,
                                                 state->history,
                                                 state->turn,
                                                 sc_string_as_str(&state->memory_context),
                                                 &state->result);
    }
    if (sc_status_is_ok(status)) {
        status = build_prompt(state->agent,
                              state->turn,
                              state->history,
                              sc_string_as_str(&state->memory_context),
                              state->scratch_alloc,
                              &state->prompt_base);
    }
    if (sc_status_is_ok(status)) {
        status = turn_add_event(state->turn, &state->result, SC_TURN_EVENT_PROMPT_BUILT, sc_str_from_cstr("prompt"), sc_str_from_cstr("built"), SC_OK);
    }
    if (sc_status_is_ok(status)) {
        status = agent_emit_observer(state->agent, sc_str_from_cstr("runtime.prompt.built"), sc_str_from_cstr("ok"));
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }

    sc_string_builder_init(&state->output, state->alloc);
    sc_string_builder_init(&state->prompt, state->scratch_alloc);
    status = sc_string_builder_append(&state->prompt, sc_string_as_str(&state->prompt_base));
    if (sc_status_is_ok(status)) {
        status = tool_specs_for_turn(state->agent, state->turn, state->scratch_alloc, &state->tool_specs);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_serialize(state->tool_specs, state->scratch_alloc, &state->tool_specs_json);
    }
    if (sc_status_is_ok(status)) {
        agent_async_provider_next(state);
    }
    return status;
}

static void agent_async_provider_next(agent_async_turn *state)
{
    sc_provider_request request = {0};
    sc_status status = sc_status_ok();
    bool use_streaming = false;

    if (state == nullptr) {
        return;
    }
    if (state->iterations >= state->agent->max_tool_iterations) {
        agent_async_complete(state, sc_status_cancelled("sc.agent.tool_loop.max_iterations"));
        return;
    }
    status = turn_check_continue(state->turn, state->deadline_ns);
    if (!sc_status_is_ok(status)) {
        agent_async_complete(state, status);
        return;
    }
    state->iterations += 1;
    state->result.provider_call_count += 1;
    status = turn_add_event(state->turn, &state->result,
                              SC_TURN_EVENT_PROVIDER_STARTED,
                              sc_str_from_cstr("provider"),
                              sc_str_from_cstr("started"),
                              SC_OK);
    if (sc_status_is_ok(status)) {
        status = agent_emit_observer(state->agent, sc_str_from_cstr("runtime.provider.request.started"), sc_str_from_cstr("ok"));
    }
    if (!sc_status_is_ok(status)) {
        agent_async_complete(state, status);
        return;
    }

    request = (sc_provider_request){
        .struct_size = sizeof(request),
        .model = sc_string_as_str(&state->agent->model),
        .prompt = sc_str_from_parts((const char *)state->prompt.bytes.ptr, state->prompt.bytes.len),
        .system_instruction = sc_string_as_str(&state->agent->identity),
        .tool_specs_json = sc_string_as_str(&state->tool_specs_json),
        .media = state->turn->media,
        .media_count = state->turn->media_count,
        .media_context = state->turn->media_context,
        .cancel_requested = turn_cancel_requested(state->turn),
        .route_hint = state->turn->route_hint,
    };
    use_streaming = state->turn->use_streaming || state->agent->use_streaming;
    if (!use_streaming) {
        status = sc_provider_generate_async(state->agent->provider,
                                            state->context,
                                            &request,
                                            state->alloc,
                                            agent_async_provider_generate_done,
                                            state,
                                            nullptr);
    } else {
        sc_provider_response_clear(&state->response);
        sc_provider_response_init(&state->response, state->scratch_alloc);
        state->stream_collect = (runtime_stream_collect){
            .alloc = state->scratch_alloc,
            .agent = state->agent,
            .turn = state->turn,
            .result = &state->result,
            .response = &state->response,
        };
        status = sc_provider_stream_async(state->agent->provider,
                                          state->context,
                                          &request,
                                          state->scratch_alloc,
                                          runtime_stream_callback,
                                          &state->stream_collect,
                                          agent_async_provider_stream_done,
                                          state,
                                          nullptr);
    }
    if (!sc_status_is_ok(status)) {
        agent_async_complete(state, status);
    }
}

static void agent_async_provider_generate_done(void *user_data, const sc_provider_response *response, sc_status status)
{
    agent_async_turn *state = user_data;

    if (state == nullptr) {
        sc_status_clear(&status);
        return;
    }
    sc_provider_response_clear(&state->response);
    if (sc_status_is_ok(status)) {
        status = provider_response_clone(state->scratch_alloc, response, &state->response);
    }
    agent_async_provider_ready(state, status);
}

static void agent_async_provider_stream_done(void *user_data, sc_status status)
{
    agent_async_turn *state = user_data;

    if (state == nullptr) {
        sc_status_clear(&status);
        return;
    }
    if (sc_status_is_ok(status) && !state->response.streaming_complete) {
        status = sc_status_parse("sc.agent.provider_stream.incomplete");
    }
    agent_async_provider_ready(state, status);
}

static void agent_async_provider_ready(agent_async_turn *state, sc_status status)
{
    if (state == nullptr) {
        sc_status_clear(&status);
        return;
    }
    if (sc_status_is_ok(status)) {
        status = recover_text_tool_calls(&state->response, state->scratch_alloc);
    }
    if (sc_status_is_ok(status)) {
        result_add_usage(&state->result, &state->response);
        status = turn_add_event(state->turn, &state->result,
                                  SC_TURN_EVENT_PROVIDER_CALLED,
                                  sc_str_from_cstr("provider"),
                                  sc_str_from_cstr("completed"),
                                  SC_OK);
    }
    if (sc_status_is_ok(status)) {
        status = agent_emit_observer(state->agent, sc_str_from_cstr("runtime.provider.completed"), sc_str_from_cstr("ok"));
    }
    if (sc_status_is_ok(status) && state->response.malformed_tool_call) {
        status = sc_status_parse("sc.agent.provider.malformed_tool_call");
    }
    if (sc_status_is_ok(status) && result_exceeds_budget(state->turn, &state->result)) {
        state->result.budget_exceeded = true;
        status = sc_status_cancelled("sc.agent.budget_exceeded");
    }
    if (sc_status_is_ok(status) && state->response.text.len > 0) {
        status = sc_string_builder_append(&state->output, sc_string_as_str(&state->response.text));
        if (sc_status_is_ok(status)) {
            status = turn_add_event(state->turn, &state->result,
                                      SC_TURN_EVENT_TEXT_DELTA,
                                      sc_str_from_cstr("assistant"),
                                      sc_string_as_str(&state->response.text),
                                      SC_OK);
        }
    }
    if (!sc_status_is_ok(status)) {
        if (agent_async_retry_context_overflow(state, status)) {
            sc_status_clear(&status);
            return;
        }
        agent_async_complete(state, status);
        return;
    }
    if (state->response.tool_calls.len == 0) {
        status = agent_async_finalize_success(state);
        agent_async_complete(state, status);
        return;
    }
    sc_string_builder_init(&state->feedback, state->scratch_alloc);
    state->feedback_initialized = true;
    state->tool_index = 0;
    agent_async_tool_next(state);
}

static bool agent_async_retry_context_overflow(agent_async_turn *state, sc_status status)
{
    sc_status retry_status = sc_status_ok();

    if (state == nullptr || !is_context_overflow_status(status) || state->retried_context_overflow ||
        state->history == nullptr || state->history->messages.len == 0) {
        return false;
    }
    state->retried_context_overflow = true;
    retry_status = compact_history_for_overflow_retry(state->agent,
                                                      state->history,
                                                      state->turn,
                                                      sc_string_as_str(&state->memory_context),
                                                      &state->result);
    if (sc_status_is_ok(retry_status)) {
        retry_status = agent_async_rebuild_prompt(state, sc_str_from_cstr("prompt.context_trimmed"));
    }
    if (!sc_status_is_ok(retry_status)) {
        agent_async_complete(state, retry_status);
        return true;
    }
    agent_async_provider_next(state);
    return true;
}

static sc_status agent_async_rebuild_prompt(agent_async_turn *state, sc_str event_name)
{
    sc_status status = sc_status_ok();

    if (state == nullptr) {
        return sc_status_invalid_argument("sc.agent.async.invalid_argument");
    }
    if (state->feedback_initialized) {
        sc_string_builder_clear(&state->feedback);
        state->feedback_initialized = false;
    }
    sc_provider_response_clear(&state->response);
    sc_string_builder_clear(&state->prompt);
    sc_string_builder_clear(&state->output);
    sc_string_clear(&state->prompt_base);
    state->iterations = 0;
    state->tool_index = 0;
    sc_string_builder_init(&state->output, state->alloc);
    sc_string_builder_init(&state->prompt, state->scratch_alloc);
    status = build_prompt(state->agent,
                          state->turn,
                          state->history,
                          sc_string_as_str(&state->memory_context),
                          state->scratch_alloc,
                          &state->prompt_base);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&state->prompt, sc_string_as_str(&state->prompt_base));
    }
    if (sc_status_is_ok(status)) {
        status = turn_add_event(state->turn,
                                &state->result,
                                SC_TURN_EVENT_PROMPT_BUILT,
                                event_name,
                                sc_str_from_cstr("built"),
                                SC_OK);
    }
    return status;
}

static void agent_async_tool_next(agent_async_turn *state)
{
    const sc_provider_tool_call *provider_call = nullptr;
    sc_tool *tool = nullptr;
    sc_tool_call call = {0};
    sc_tool_spec spec = {0};
    sc_status tool_status;

    if (state == nullptr) {
        return;
    }
    if (state->tool_index >= state->response.tool_calls.len) {
        sc_string feedback_text = {0};
        sc_status status = sc_string_builder_finish(&state->feedback, &feedback_text);
        state->feedback_initialized = false;
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&state->prompt, "\n## Tool results\n");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&state->prompt, sc_string_as_str(&feedback_text));
        }
        sc_string_clear(&feedback_text);
        sc_string_builder_clear(&state->feedback);
        sc_provider_response_clear(&state->response);
        if (!sc_status_is_ok(status)) {
            agent_async_complete(state, status);
            return;
        }
        agent_async_provider_next(state);
        return;
    }

    provider_call = sc_vec_at_const(&state->response.tool_calls, state->tool_index);
    tool = provider_call == nullptr || !turn_allows_tool(state->turn, sc_string_as_str(&provider_call->name)) ?
        nullptr :
        find_tool_by_name(state->agent, sc_string_as_str(&provider_call->name));
    tool_status = turn_check_continue(state->turn, state->deadline_ns);
    if (!sc_status_is_ok(tool_status)) {
        sc_status_code code = tool_status.code;
        sc_status_clear(&tool_status);
        agent_async_complete(state,
                             code == SC_ERR_TIMEOUT ? sc_status_timeout("sc.agent.tool_loop.timeout") :
                                                       sc_status_cancelled("sc.agent.tool_loop.cancelled"));
        return;
    }
    if (provider_call != nullptr && !turn_allows_tool(state->turn, sc_string_as_str(&provider_call->name))) {
        tool_status = sc_status_security_denied("sc.agent.tool_loop.filtered_tool");
    } else if (tool == nullptr || provider_call == nullptr) {
        tool_status = sc_status_invalid_argument("sc.agent.tool_loop.unknown_tool");
    } else if (tool_terminal_failures_contains(&state->terminal_failures, provider_call)) {
        tool_status = sc_status_unsupported("sc.agent.tool_loop.repeated_terminal_failure");
    } else {
        tool_status = sc_tool_spec_get(tool, &spec);
        if (sc_status_is_ok(tool_status) && !turn_autonomy_allows_spec(state->turn, &spec)) {
            tool_status = sc_status_security_denied("sc.agent.tool_loop.autonomy_denied");
        }
        if (sc_status_is_ok(tool_status)) {
            tool_status = sc_security_validate_prompt_injection(sc_string_as_str(&provider_call->arguments_json));
        }
        if (sc_status_is_ok(tool_status)) {
            tool_status = validate_tool_call_args(&spec, provider_call);
        }
        if (sc_status_is_ok(tool_status)) {
            tool_status = sc_provider_tool_call_as_tool_call(provider_call, &call);
            call.cancel_token = state->turn->cancel_token;
        }
        if (sc_status_is_ok(tool_status)) {
            sc_status invoke_status = sc_tool_invoke_async(tool,
                                                           state->context,
                                                           &call,
                                                           state->alloc,
                                                           agent_async_tool_done,
                                                           state,
                                                           nullptr);
            if (!sc_status_is_ok(invoke_status)) {
                tool_status = invoke_status;
            } else {
                return;
            }
        }
    }
    sc_status finish_status = agent_async_tool_finish(state, provider_call, tool_status, nullptr);
    if (sc_status_is_ok(finish_status)) {
        state->tool_index += 1;
        agent_async_tool_next(state);
    } else {
        agent_async_complete(state, finish_status);
    }
}

static void agent_async_tool_done(void *user_data, const sc_tool_result *result, sc_status status)
{
    agent_async_turn *state = user_data;
    const sc_provider_tool_call *provider_call = nullptr;
    sc_status next_status;
    sc_status_code tool_code = status.code;

    if (state == nullptr) {
        sc_status_clear(&status);
        return;
    }
    provider_call = sc_vec_at_const(&state->response.tool_calls, state->tool_index);
    next_status = agent_async_tool_finish(state, provider_call, status, result);
    if (!sc_status_is_ok(next_status)) {
        agent_async_complete(state, next_status);
        return;
    }
    if (tool_code == SC_ERR_CANCELLED) {
        agent_async_complete(state, sc_status_cancelled("sc.agent.tool_loop.cancelled"));
        return;
    }
    state->tool_index += 1;
    agent_async_tool_next(state);
}

static sc_status agent_async_tool_finish(agent_async_turn *state,
                                         const sc_provider_tool_call *provider_call,
                                         sc_status tool_status,
                                         const sc_tool_result *tool_result)
{
    sc_status status = sc_status_ok();
    sc_string args_summary = {0};
    sc_string output_summary = {0};
    bool tool_success = sc_status_is_ok(tool_status) && tool_result != nullptr && tool_result->success;
    sc_str output = tool_success ? sc_string_as_str(&tool_result->output) :
                                   sc_str_from_cstr(tool_status.error_key == nullptr ? "tool_error" : tool_status.error_key);

    state->result.tool_call_count += 1;
    status = turn_add_event(state->turn, &state->result,
                              SC_TURN_EVENT_TOOL_CALLED,
                              provider_call == nullptr ? sc_str_from_cstr("unknown") : sc_string_as_str(&provider_call->name),
                              sc_status_is_ok(tool_status) ? sc_str_from_cstr("requested") : sc_str_from_cstr("denied_or_failed"),
                              tool_status.code);
    if (sc_status_is_ok(status) && !sc_status_is_ok(tool_status) &&
        (tool_status.code == SC_ERR_SECURITY_DENIED || tool_status.code == SC_ERR_INVALID_ARGUMENT ||
         (tool_status.code == SC_ERR_CANCELLED && tool_status.error_key != nullptr &&
          strcmp(tool_status.error_key, "sc.tool.approval_required") == 0))) {
        status = turn_add_event(state->turn, &state->result,
                                  SC_TURN_EVENT_TOOL_DENIED,
                                  provider_call == nullptr ? sc_str_from_cstr("unknown") : sc_string_as_str(&provider_call->name),
                                  sc_str_from_cstr(tool_status.error_key == nullptr ? "tool_denied" : tool_status.error_key),
                                  tool_status.code);
    }
    if (sc_status_is_ok(status)) {
        status = turn_add_event(state->turn, &state->result,
                                  SC_TURN_EVENT_TOOL_RESULT,
                                  provider_call == nullptr ? sc_str_from_cstr("unknown") : sc_string_as_str(&provider_call->name),
                                  sc_status_is_ok(tool_status) ? sc_str_from_cstr("completed") :
                                      sc_str_from_cstr(tool_status.error_key == nullptr ? "tool_error" : tool_status.error_key),
                                  tool_status.code);
    }
    if (sc_status_is_ok(status)) {
        status = agent_emit_observer(state->agent,
                                     sc_status_is_ok(tool_status) ? sc_str_from_cstr("runtime.tool.completed") :
                                                                   sc_str_from_cstr("runtime.tool.denied"),
                                     sc_status_is_ok(tool_status) ? sc_str_from_cstr("ok") :
                                                                   sc_str_from_cstr(tool_status.error_key == nullptr ? "error" :
                                                                                                    tool_status.error_key));
    }
    if (sc_status_is_ok(status)) {
        status = receipt_summary_for_turn(state->scratch_alloc,
                                          provider_call == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&provider_call->arguments_json),
                                          &args_summary);
    }
    if (sc_status_is_ok(status)) {
        status = receipt_summary_for_turn(state->scratch_alloc, output, &output_summary);
    }
    if (sc_status_is_ok(status) && agent_receipts_enabled(state->agent)) {
        status = sc_receipt_chain_append_ex(&state->result.receipts,
                                            provider_call == nullptr ? sc_str_from_cstr("unknown") :
                                                                       sc_string_as_str(&provider_call->name),
                                            sc_string_as_str(&args_summary),
                                            sc_string_as_str(&output_summary),
                                            tool_success,
                                            tool_policy_decision(tool_status),
                                            tool_failure_reason(tool_status),
                                            tool_outcome(tool_status, tool_success));
    }
    if (sc_status_is_ok(status) && tool_status_is_turn_terminal(tool_status)) {
        status = tool_terminal_failures_add(&state->terminal_failures, state->scratch_alloc, provider_call, tool_status);
    }
    if (sc_status_is_ok(status) && tool_success) {
        status = result_copy_tool_attachment(state->alloc, &state->result, tool_result);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&state->feedback, "tool=");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&state->feedback,
                                          provider_call == nullptr ? sc_str_from_cstr("unknown") : sc_string_as_str(&provider_call->name));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&state->feedback, " success=");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&state->feedback, tool_success ? "true" : "false");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&state->feedback, " output=");
    }
    if (sc_status_is_ok(status)) {
        status = append_bounded(&state->feedback, output, state->agent->max_tool_output_bytes);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&state->feedback, "\n");
    }
    sc_status_clear(&tool_status);
    sc_string_clear(&args_summary);
    sc_string_clear(&output_summary);
    return status;
}

static sc_status agent_async_finalize_success(agent_async_turn *state)
{
    sc_status status = sc_status_ok();

    if (state == nullptr) {
        return sc_status_invalid_argument("sc.agent.async.invalid_argument");
    }
    status = sc_string_builder_finish(&state->output, &state->result.output);
    if (sc_status_is_ok(status)) {
        status = append_receipt_block_to_output(state->agent, state->alloc, &state->result);
    }
    if (sc_status_is_ok(status)) {
        status = strip_receipt_tokens_from_output(state->alloc, &state->result);
    }
    if (sc_status_is_ok(status)) {
        status = turn_check_continue(state->turn, state->deadline_ns);
    }
    if (sc_status_is_ok(status)) {
        status = sc_history_append(state->history, sc_str_from_cstr("user"), state->turn->input);
    }
    if (sc_status_is_ok(status)) {
        status = sc_history_append(state->history, sc_str_from_cstr("assistant"), sc_string_as_str(&state->result.output));
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(state->turn)) {
        status = persist_turn(state->agent, state->turn, &state->result);
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(state->turn)) {
        status = turn_add_event(state->turn, &state->result, SC_TURN_EVENT_TURN_PERSISTED, sc_str_from_cstr("turn"), sc_str_from_cstr("persisted"), SC_OK);
    }
    if (sc_status_is_ok(status) && !turn_memory_disabled(state->turn)) {
        status = agent_emit_observer(state->agent, sc_str_from_cstr("runtime.memory.write"), sc_str_from_cstr("ok"));
    }
    if (sc_status_is_ok(status)) {
        status = turn_add_event(state->turn, &state->result, SC_TURN_EVENT_COMPLETED, sc_str_from_cstr("turn"), sc_str_from_cstr("completed"), SC_OK);
    }
    if (sc_status_is_ok(status)) {
        status = agent_emit_observer(state->agent, sc_str_from_cstr("runtime.turn.completed"), sc_str_from_cstr("ok"));
    }
    return status;
}

static void agent_async_complete(agent_async_turn *state, sc_status status)
{
    if (state == nullptr) {
        sc_status_clear(&status);
        return;
    }
    if (!sc_status_is_ok(status)) {
        if (status.code == SC_ERR_CANCELLED) {
            state->result.cancelled = true;
        } else if (status.code == SC_ERR_TIMEOUT) {
            state->result.timed_out = true;
        }
        (void)turn_add_event(state->turn, &state->result,
                               status.code == SC_ERR_CANCELLED ? SC_TURN_EVENT_CANCELLED :
                                   (status.code == SC_ERR_TIMEOUT ? SC_TURN_EVENT_TIMEOUT : SC_TURN_EVENT_ERROR),
                               sc_str_from_cstr("turn"),
                               sc_str_from_cstr(status.error_key == nullptr ? "error" : status.error_key),
                               status.code);
        (void)agent_emit_observer(state->agent,
                                  status.code == SC_ERR_CANCELLED ? sc_str_from_cstr("runtime.turn.cancelled") :
                                      (status.code == SC_ERR_TIMEOUT ? sc_str_from_cstr("runtime.turn.timed_out") :
                                                                       sc_str_from_cstr("runtime.turn.failed")),
                                  sc_str_from_cstr(status.error_key == nullptr ? "error" : status.error_key));
    }
    state->complete(state->complete_user_data, sc_status_is_ok(status) ? &state->result : nullptr, status);
    agent_async_destroy(state);
}

static void agent_async_destroy(agent_async_turn *state)
{
    if (state == nullptr) {
        return;
    }
    if (state->feedback_initialized) {
        sc_string_builder_clear(&state->feedback);
    }
    tool_terminal_failures_clear(&state->terminal_failures);
    sc_provider_response_clear(&state->response);
    sc_string_clear(&state->tool_specs_json);
    sc_json_destroy(state->tool_specs);
    sc_string_builder_clear(&state->output);
    sc_string_builder_clear(&state->prompt);
    sc_string_clear(&state->prompt_base);
    sc_string_clear(&state->memory_context);
    sc_agent_turn_result_clear(&state->result);
    sc_arena_clear(&state->scratch);
    sc_free(state->alloc, state, sizeof(*state), _Alignof(agent_async_turn));
}

static sc_status provider_response_clone(sc_allocator *alloc, const sc_provider_response *source, sc_provider_response *out)
{
    sc_status status = sc_status_ok();

    if (source == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.agent.provider_response.clone_invalid_argument");
    }
    sc_provider_response_init(out, alloc);
    status = copy_optional_string(alloc, &source->text, &out->text);
    if (sc_status_is_ok(status)) {
        status = copy_optional_string(alloc, &source->reasoning_text, &out->reasoning_text);
    }
    if (sc_status_is_ok(status)) {
        status = copy_optional_string(alloc, &source->finish_reason, &out->finish_reason);
    }
    if (sc_status_is_ok(status)) {
        status = copy_optional_string(alloc, &source->model, &out->model);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < source->tool_calls.len; i += 1) {
        const sc_provider_tool_call *call = sc_vec_at_const(&source->tool_calls, i);
        status = sc_provider_response_add_tool_call(out, call);
    }
    if (sc_status_is_ok(status)) {
        out->input_tokens = source->input_tokens;
        out->output_tokens = source->output_tokens;
        out->total_tokens = source->total_tokens;
        out->cost_usd = source->cost_usd;
        out->malformed_tool_call = source->malformed_tool_call;
        out->streaming_complete = source->streaming_complete;
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_response_clear(out);
    }
    return status;
}

static sc_status copy_optional_string(sc_allocator *alloc, const sc_string *source, sc_string *out)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.agent.provider_response.clone_invalid_argument");
    }
    if (source == nullptr || source->len == 0) {
        *out = (sc_string){0};
        return sc_status_ok();
    }
    return sc_string_from_str(alloc, sc_string_as_str(source), out);
}

static sc_status result_copy_tool_attachment(sc_allocator *alloc, sc_agent_turn_result *out, const sc_tool_result *tool_result)
{
    sc_string content_type = {0};
    sc_string filename = {0};
    sc_bytes bytes = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr || tool_result == nullptr) {
        return sc_status_invalid_argument("sc.agent.tool_attachment.invalid_argument");
    }
    if (tool_result->attachment_bytes.len == 0) {
        return sc_status_ok();
    }
    status = sc_string_from_str(alloc, sc_string_as_str(&tool_result->attachment_content_type), &content_type);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&tool_result->attachment_filename), &filename);
    }
    if (sc_status_is_ok(status)) {
        status = sc_bytes_from_buf(alloc,
                                   sc_buf_from_parts(tool_result->attachment_bytes.ptr,
                                                     tool_result->attachment_bytes.len),
                                   &bytes);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&content_type);
        sc_string_clear(&filename);
        sc_bytes_clear(&bytes);
        return status;
    }
    sc_string_clear(&out->attachment_content_type);
    sc_string_clear(&out->attachment_filename);
    sc_bytes_clear(&out->attachment_bytes);
    out->attachment_content_type = content_type;
    out->attachment_filename = filename;
    out->attachment_bytes = bytes;
    out->attachment_delivery = tool_result->attachment_delivery;
    return sc_status_ok();
}

static sc_tool *find_tool_by_name(sc_agent *agent, sc_str name)
{
    if (agent == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < agent->tools.len; i += 1) {
        sc_tool **slot = sc_vec_at(&agent->tools, i);
        sc_tool_spec spec = {0};
        if (slot != nullptr && *slot != nullptr && sc_status_is_ok(sc_tool_spec_get(*slot, &spec)) && sc_str_equal(spec.name, name)) {
            return *slot;
        }
    }
    return nullptr;
}

static bool turn_allows_tool(const sc_turn *turn, sc_str name)
{
    if (turn_memory_disabled(turn) && tool_name_is_memory(name)) {
        return false;
    }
    if (turn != nullptr && turn->denied_tools != nullptr) {
        for (size_t i = 0; i < turn->denied_tool_count; i += 1) {
            if (sc_str_equal(turn->denied_tools[i], name)) {
                return false;
            }
        }
    }
    if (turn == nullptr || turn->allowed_tools == nullptr || turn->allowed_tool_count == 0) {
        return true;
    }
    for (size_t i = 0; i < turn->allowed_tool_count; i += 1) {
        if (sc_str_equal(turn->allowed_tools[i], name)) {
            return true;
        }
    }
    return false;
}

static bool tool_name_is_memory(sc_str name)
{
    return sc_str_equal(name, sc_str_from_cstr("memory_store")) ||
           sc_str_equal(name, sc_str_from_cstr("memory_recall")) ||
           sc_str_equal(name, sc_str_from_cstr("memory_search")) ||
           sc_str_equal(name, sc_str_from_cstr("memory_pin")) ||
           sc_str_equal(name, sc_str_from_cstr("memory_forget")) ||
           sc_str_equal(name, sc_str_from_cstr("memory_export")) ||
           sc_str_equal(name, sc_str_from_cstr("memory_purge"));
}

static bool turn_autonomy_allows_risk(const sc_turn *turn, sc_tool_risk risk)
{
    if (turn == nullptr || !turn->autonomy_override_set) {
        return true;
    }
    if (turn->autonomy_override == SC_AUTONOMY_FULL) {
        return true;
    }
    if (turn->autonomy_override == SC_AUTONOMY_READ_ONLY) {
        return risk == SC_TOOL_RISK_READONLY;
    }
    return risk != SC_TOOL_RISK_DESTRUCTIVE && risk != SC_TOOL_RISK_SHELL;
}

static bool turn_autonomy_allows_spec(const sc_turn *turn, const sc_tool_spec *spec)
{
    return spec != nullptr && turn_autonomy_allows_risk(turn, spec->risk);
}

static sc_status validate_tool_call_args(const sc_tool_spec *spec, const sc_provider_tool_call *provider_call)
{
    const sc_json_value *schema = spec == nullptr ? nullptr : spec->input_schema;
    sc_json_value *required = schema == nullptr ? nullptr : sc_json_object_get(schema, sc_str_from_cstr("required"));

    if (provider_call == nullptr || provider_call->arguments == nullptr) {
        return sc_status_invalid_argument("sc.agent.tool_schema.missing_args");
    }
    if (schema == nullptr) {
        return sc_status_ok();
    }
    if (sc_json_type_of(provider_call->arguments) != SC_JSON_OBJECT) {
        return sc_status_invalid_argument("sc.agent.tool_schema.args_not_object");
    }
    for (size_t i = 0; i < sc_json_array_len(required); i += 1) {
        sc_str name = {0};
        sc_json_value *required_name = sc_json_array_get(required, i);
        sc_json_value *value = nullptr;
        if (!sc_json_as_str(required_name, &name)) {
            return sc_status_invalid_argument("sc.agent.tool_schema.invalid_required");
        }
        value = sc_json_object_get(provider_call->arguments, name);
        if (value == nullptr) {
            return sc_status_invalid_argument("sc.agent.tool_schema.required_missing");
        }
        if (sc_json_type_of(value) != SC_JSON_STRING) {
            return sc_status_invalid_argument("sc.agent.tool_schema.type_mismatch");
        }
    }
    return sc_status_ok();
}

static sc_status append_bounded(sc_string_builder *builder, sc_str value, size_t max_bytes)
{
    if (builder == nullptr) {
        return sc_status_invalid_argument("sc.agent.tool_feedback.invalid_argument");
    }
    if (max_bytes > 0 && value.len > max_bytes) {
        return sc_string_builder_append(builder, sc_str_from_parts(value.ptr, max_bytes));
    }
    return sc_string_builder_append(builder, value);
}

static sc_status tool_specs_for_turn(sc_agent *agent, const sc_turn *turn, sc_allocator *alloc, sc_json_value **out)
{
    sc_tool **selected = nullptr;
    size_t selected_count = 0;
    sc_status status = sc_status_ok();

    if (agent == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.agent.tool_specs.invalid_argument");
    }
    if (turn_uses_default_tool_specs(turn)) {
        return sc_tool_registry_model_specs_from_tools(agent->tools.ptr, agent->tools.len, alloc, out);
    }
    if (agent->tools.len > 0) {
        selected = sc_alloc(alloc, agent->tools.len * sizeof(*selected), _Alignof(sc_tool *));
        if (selected == nullptr) {
            return sc_status_no_memory();
        }
    }
    for (size_t i = 0; i < agent->tools.len; i += 1) {
        sc_tool **slot = sc_vec_at(&agent->tools, i);
        sc_tool_spec spec = {0};
        if (slot != nullptr && *slot != nullptr && sc_status_is_ok(sc_tool_spec_get(*slot, &spec)) &&
            turn_allows_tool(turn, spec.name) && turn_autonomy_allows_spec(turn, &spec)) {
            selected[selected_count++] = *slot;
        }
    }
    status = sc_tool_registry_model_specs_from_tools(selected, selected_count, alloc, out);
    sc_free(alloc, selected, agent->tools.len * sizeof(*selected), _Alignof(sc_tool *));
    return status;
}

static sc_status recover_text_tool_calls(sc_provider_response *response, sc_allocator *alloc)
{
    sc_str source = {0};
    sc_string_builder visible = {0};
    size_t pos = 0;
    size_t recovered = 0;
    bool changed = false;
    sc_status status = sc_status_ok();

    if (response == nullptr || response->text.ptr == nullptr || response->text.len == 0) {
        return sc_status_ok();
    }
    /*
     * Compatibility path for providers that emit XML-ish tool-call markers in
     * assistant text instead of structured tool_calls. Parsed tool calls are
     * removed from visible text so they are not shown to the user as prose.
     */
    source = sc_string_as_str(&response->text);
    sc_string_builder_init(&visible, alloc);
    while (sc_status_is_ok(status) && pos < source.len) {
        size_t tool_call_start = find_literal(source, pos, "<tool_call>");
        size_t tool_start = find_literal(source, pos, "<tool ");
        bool use_tool_call = tool_call_start != SIZE_MAX && (tool_start == SIZE_MAX || tool_call_start < tool_start);
        size_t start = use_tool_call ? tool_call_start : tool_start;

        if (start == SIZE_MAX) {
            status = sc_string_builder_append(&visible, sc_str_from_parts(source.ptr + pos, source.len - pos));
            break;
        }
        status = sc_string_builder_append(&visible, sc_str_from_parts(source.ptr + pos, start - pos));
        if (!sc_status_is_ok(status)) {
            break;
        }
        if (use_tool_call) {
            size_t body_start = start + strlen("<tool_call>");
            size_t end = find_literal(source, body_start, "</tool_call>");
            if (end == SIZE_MAX) {
                status = sc_string_builder_append(&visible, sc_str_from_parts(source.ptr + start, source.len - start));
                break;
            }
            sc_str payload = trim_ascii(sc_str_from_parts(source.ptr + body_start, end - body_start));
            sc_status parsed = add_recovered_json_tool_call(response, alloc, payload, recovered + 1);
            if (sc_status_is_ok(parsed)) {
                recovered += 1;
                changed = true;
            } else {
                sc_status_clear(&parsed);
                status = sc_string_builder_append(&visible, sc_str_from_parts(source.ptr + start, end + strlen("</tool_call>") - start));
            }
            pos = end + strlen("</tool_call>");
        } else {
            size_t name_start = find_literal(source, start, "name=\"");
            size_t tag_end = find_byte(source, start, '>');
            size_t end = tag_end == SIZE_MAX ? SIZE_MAX : find_literal(source, tag_end + 1, "</tool>");
            if (name_start == SIZE_MAX || tag_end == SIZE_MAX || end == SIZE_MAX || name_start > tag_end) {
                status = sc_string_builder_append(&visible, sc_str_from_parts(source.ptr + start, source.len - start));
                break;
            }
            name_start += strlen("name=\"");
            size_t name_end = find_byte(source, name_start, '"');
            if (name_end == SIZE_MAX || name_end > tag_end) {
                status = sc_string_builder_append(&visible, sc_str_from_parts(source.ptr + start, end + strlen("</tool>") - start));
                pos = end + strlen("</tool>");
                continue;
            }
            sc_str name = sc_str_from_parts(source.ptr + name_start, name_end - name_start);
            sc_str args = trim_ascii(sc_str_from_parts(source.ptr + tag_end + 1, end - tag_end - 1));
            sc_status parsed = add_recovered_named_tool_call(response, alloc, name, args, recovered + 1);
            if (sc_status_is_ok(parsed)) {
                recovered += 1;
                changed = true;
            } else {
                sc_status_clear(&parsed);
                status = sc_string_builder_append(&visible, sc_str_from_parts(source.ptr + start, end + strlen("</tool>") - start));
            }
            pos = end + strlen("</tool>");
        }
    }
    if (sc_status_is_ok(status) && changed) {
        sc_string text = {0};
        status = sc_string_builder_finish(&visible, &text);
        if (sc_status_is_ok(status)) {
            sc_string_clear(&response->text);
            response->text = text;
        }
    } else {
        sc_string_builder_clear(&visible);
    }
    return status;
}

static size_t find_literal(sc_str haystack, size_t start, const char *needle)
{
    size_t needle_len = needle == nullptr ? 0 : strlen(needle);
    if (needle_len == 0 || haystack.ptr == nullptr || start > haystack.len || needle_len > haystack.len) {
        return SIZE_MAX;
    }
    for (size_t i = start; i + needle_len <= haystack.len; i += 1) {
        if (memcmp(haystack.ptr + i, needle, needle_len) == 0) {
            return i;
        }
    }
    return SIZE_MAX;
}

static size_t find_byte(sc_str haystack, size_t start, char needle)
{
    if (haystack.ptr == nullptr || start > haystack.len) {
        return SIZE_MAX;
    }
    for (size_t i = start; i < haystack.len; i += 1) {
        if (haystack.ptr[i] == needle) {
            return i;
        }
    }
    return SIZE_MAX;
}

static sc_status add_recovered_json_tool_call(sc_provider_response *response, sc_allocator *alloc, sc_str payload, size_t index)
{
    sc_json_value *root = nullptr;
    sc_json_value *args = nullptr;
    sc_json_parse_error error = {0};
    sc_str name = {0};
    sc_status status = sc_json_parse(alloc, payload, &root, &error);

    if (sc_status_is_ok(status) && !sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("name")), &name)) {
        (void)sc_json_as_str(sc_json_object_get(root, sc_str_from_cstr("tool")), &name);
    }
    if (sc_status_is_ok(status) && (name.ptr == nullptr || name.len == 0)) {
        status = sc_status_parse("sc.agent.text_tool_call.missing_name");
    }
    if (sc_status_is_ok(status)) {
        args = sc_json_object_get(root, sc_str_from_cstr("arguments"));
        if (args == nullptr) {
            args = sc_json_object_get(root, sc_str_from_cstr("args"));
        }
        if (args == nullptr) {
            status = sc_status_parse("sc.agent.text_tool_call.missing_arguments");
        }
    }
    if (sc_status_is_ok(status)) {
        sc_string args_json = {0};
        status = sc_json_serialize(args, alloc, &args_json);
        if (sc_status_is_ok(status)) {
            status = add_recovered_named_tool_call(response, alloc, name, sc_string_as_str(&args_json), index);
        }
        sc_string_clear(&args_json);
    }
    sc_json_destroy(root);
    return status;
}

static sc_status add_recovered_named_tool_call(sc_provider_response *response,
                                               sc_allocator *alloc,
                                               sc_str name,
                                               sc_str args_json,
                                               size_t index)
{
    char call_id[32] = {0};
    sc_provider_tool_call call = {.struct_size = sizeof(call)};
    sc_json_parse_error error = {0};
    int written = snprintf(call_id, sizeof(call_id), "text-call-%zu", index);
    sc_status status = sc_status_ok();

    if (response == nullptr || name.ptr == nullptr || name.len == 0 || args_json.ptr == nullptr || written <= 0 || (size_t)written >= sizeof(call_id)) {
        return sc_status_invalid_argument("sc.agent.text_tool_call.invalid_argument");
    }
    status = sc_string_from_cstr(response->tool_calls.alloc, call_id, &call.call_id);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(response->tool_calls.alloc, name, &call.name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(response->tool_calls.alloc, args_json, &call.arguments_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_parse(alloc, args_json, &call.arguments, &error);
    }
    if (sc_status_is_ok(status)) {
        status = sc_provider_response_add_tool_call(response, &call);
    }
    sc_provider_tool_call_clear(&call);
    return status;
}

static sc_str trim_ascii(sc_str value)
{
    while (value.len > 0 && (value.ptr[0] == ' ' || value.ptr[0] == '\n' || value.ptr[0] == '\r' || value.ptr[0] == '\t')) {
        value.ptr += 1;
        value.len -= 1;
    }
    while (value.len > 0 &&
           (value.ptr[value.len - 1] == ' ' || value.ptr[value.len - 1] == '\n' || value.ptr[value.len - 1] == '\r' ||
            value.ptr[value.len - 1] == '\t')) {
        value.len -= 1;
    }
    return value;
}

static bool is_context_overflow_status(sc_status status)
{
    const char *key = status.error_key;
    return (status.code == SC_ERR_HTTP || status.code == SC_ERR_INVALID_ARGUMENT || status.code == SC_ERR_PARSE) && key != nullptr &&
           (strstr(key, "context") != nullptr || strstr(key, "token") != nullptr || strstr(key, "too_large") != nullptr);
}

static bool history_role_is(sc_str role, const char *expected)
{
    size_t expected_len = expected == nullptr ? 0U : strlen(expected);
    return role.ptr != nullptr && expected != nullptr && role.len == expected_len && memcmp(role.ptr, expected, expected_len) == 0;
}

static bool history_role_is_tool_result(sc_str role)
{
    return history_role_is(role, "tool") || history_role_is(role, "tool_result");
}

static void history_remove_at(sc_history *history, size_t index)
{
    sc_history_message *message = nullptr;

    if (history == nullptr || index >= history->messages.len) {
        return;
    }
    message = sc_vec_at(&history->messages, index);
    history_message_clear(message);
    if (index + 1 < history->messages.len) {
        (void)memmove((unsigned char *)history->messages.ptr + index * history->messages.item_size,
                      (unsigned char *)history->messages.ptr + (index + 1) * history->messages.item_size,
                      (history->messages.len - index - 1) * history->messages.item_size);
    }
    history->messages.len -= 1;
}

static sc_status history_push_message(sc_history *history, sc_history_message *message)
{
    sc_status status = sc_status_ok();

    if (history == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.history.push.invalid_argument");
    }
    status = sc_vec_push(&history->messages, message);
    if (sc_status_is_ok(status)) {
        *message = (sc_history_message){0};
    }
    return status;
}

static void result_add_usage(sc_agent_turn_result *out, const sc_provider_response *response)
{
    if (out == nullptr || response == nullptr) {
        return;
    }
    out->input_tokens += response->input_tokens;
    out->output_tokens += response->output_tokens;
    out->total_tokens += response->total_tokens;
    out->cost_usd += response->cost_usd;
}

static bool result_exceeds_budget(const sc_turn *turn, const sc_agent_turn_result *out)
{
    if (turn == nullptr || out == nullptr) {
        return false;
    }
    if (turn->max_total_tokens > 0 && out->total_tokens > turn->max_total_tokens) {
        return true;
    }
    return turn->max_cost_usd > 0.0 && out->cost_usd > turn->max_cost_usd;
}

static sc_status persist_turn(sc_agent *agent, const sc_turn *turn, const sc_agent_turn_result *result)
{
    char key[64] = {0};
    int written = 0;
    sc_string_builder body = {0};
    sc_string content = {0};
    sc_status status = sc_status_ok();
    sc_memory_record record = {0};

    if (agent == nullptr || turn == nullptr || result == nullptr || agent->memory == nullptr ||
        turn_memory_disabled(turn)) {
        return sc_status_ok();
    }
    written = snprintf(key, sizeof(key), "turn-%llu", (unsigned long long)++agent->turn_counter);
    if (written <= 0 || (size_t)written >= sizeof(key)) {
        return sc_status_io("sc.agent.persist.key_failed");
    }
    sc_string_builder_init(&body, agent->alloc);
    status = append_line(&body, sc_str_from_cstr("user"), turn->input);
    if (sc_status_is_ok(status)) {
        status = append_line(&body, sc_str_from_cstr("assistant"), sc_string_as_str(&result->output));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&body, &content);
    }
    if (sc_status_is_ok(status)) {
        record = (sc_memory_record){
            .struct_size = sizeof(record),
            .namespace_name = sc_string_as_str(&agent->turn_namespace),
            .session_id = empty_if_null(turn->session_id),
            .category = sc_str_from_cstr("turn"),
            .key = sc_str_from_cstr(key),
            .value = sc_string_as_str(&content),
            .score = 1.0,
            .importance = 1,
        };
        status = sc_memory_put(agent->memory, &record);
    }
    sc_string_clear(&content);
    sc_string_builder_clear(&body);
    return status;
}

static sc_str empty_if_null(sc_str value)
{
    return value.ptr == nullptr ? sc_str_from_cstr("") : value;
}

static sc_status append_line(sc_string_builder *builder, sc_str left, sc_str right)
{
    sc_status status = sc_string_builder_append(builder, empty_if_null(left));
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, ": ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(builder, empty_if_null(right));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\n");
    }
    return status;
}

static sc_status receipt_summary_for_turn(sc_allocator *alloc, sc_str value, sc_string *out)
{
    char text[64] = {0};
    int written = snprintf(text,
                           sizeof(text),
                           "hash=%016llx len=%zu",
                           (unsigned long long)receipt_hash_for_turn(value),
                           value.len);
    if (written < 0 || (size_t)written >= sizeof(text)) {
        return sc_status_no_memory();
    }
    return sc_string_from_cstr(alloc, text, out);
}

static uint64_t receipt_hash_for_turn(sc_str value)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < value.len; i += 1) {
        hash ^= (unsigned char)value.ptr[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static bool agent_receipts_enabled(const sc_agent *agent)
{
    return agent == nullptr || agent->policy == nullptr || agent->policy->receipts_enabled;
}

static sc_status append_receipt_block_to_output(const sc_agent *agent,
                                                sc_allocator *alloc,
                                                sc_agent_turn_result *out)
{
    sc_string_builder builder = {0};
    sc_string next = {0};
    sc_status status = sc_status_ok();

    if (agent == nullptr || out == nullptr || agent->policy == nullptr ||
        !agent->policy->receipts_enabled || !agent->policy->receipts_show_in_response ||
        out->receipts.receipts.len == 0) {
        return sc_status_ok();
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, sc_string_as_str(&out->output));
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\n\nTool receipts:\n");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < out->receipts.receipts.len; i += 1) {
        const sc_tool_receipt *receipt = sc_vec_at_const(&out->receipts.receipts, i);
        if (receipt == nullptr) {
            status = sc_status_invalid_argument("sc.agent.receipt_block.invalid_receipt");
            break;
        }
        status = sc_string_builder_append_cstr(&builder, "[receipt: ");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&receipt->token));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "] ");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&receipt->tool_name));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " ");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&receipt->outcome));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\n");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &next);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        sc_string_clear(&out->output);
        out->output = next;
    }
    return status;
}

static sc_status strip_receipt_tokens_from_output(sc_allocator *alloc, sc_agent_turn_result *out)
{
    sc_string_builder builder = {0};
    sc_string next = {0};
    sc_str input = {0};
    sc_status status = sc_status_ok();
    size_t start = 0;
    bool changed = false;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.agent.receipt_strip.invalid_argument");
    }

    input = sc_string_as_str(&out->output);
    if (input.len == 0 || input.ptr == nullptr) {
        return sc_status_ok();
    }

    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < input.len;) {
        size_t skip_start = input.len;
        size_t skip_end = input.len;

        if (i + strlen("[receipt: sc-receipt-") <= input.len &&
            memcmp(input.ptr + i, "[receipt: sc-receipt-", strlen("[receipt: sc-receipt-")) == 0) {
            size_t close = find_byte(input, i, ']');
            if (close != SIZE_MAX) {
                skip_start = i;
                skip_end = close + 1;
            }
        } else if (i + strlen("sc-receipt-") <= input.len &&
                   memcmp(input.ptr + i, "sc-receipt-", strlen("sc-receipt-")) == 0) {
            skip_start = i;
            skip_end = i + strlen("sc-receipt-");
            while (skip_end < input.len &&
                   input.ptr[skip_end] != ' ' &&
                   input.ptr[skip_end] != '\t' &&
                   input.ptr[skip_end] != '\r' &&
                   input.ptr[skip_end] != '\n' &&
                   input.ptr[skip_end] != ']' &&
                   input.ptr[skip_end] != ')' &&
                   input.ptr[skip_end] != ',' &&
                   input.ptr[skip_end] != '.') {
                skip_end += 1;
            }
        }

        if (skip_start != input.len) {
            if (skip_start > start) {
                status = sc_string_builder_append(&builder, sc_str_from_parts(input.ptr + start, skip_start - start));
            }
            changed = true;
            i = skip_end;
            start = skip_end;
            continue;
        }
        i += 1;
    }

    if (sc_status_is_ok(status) && !changed) {
        sc_string_builder_clear(&builder);
        return sc_status_ok();
    }
    if (sc_status_is_ok(status) && start < input.len) {
        status = sc_string_builder_append(&builder, sc_str_from_parts(input.ptr + start, input.len - start));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &next);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        sc_string_clear(&out->output);
        out->output = next;
    }
    return status;
}

static sc_str tool_policy_decision(sc_status tool_status)
{
    if (sc_status_is_ok(tool_status)) {
        return sc_str_from_cstr("allowed");
    }
    if (tool_status.code == SC_ERR_SECURITY_DENIED) {
        return sc_str_from_cstr("denied");
    }
    if (tool_status.code == SC_ERR_CANCELLED && tool_status.error_key != nullptr &&
        strcmp(tool_status.error_key, "sc.tool.approval_required") == 0) {
        return sc_str_from_cstr("approval-required");
    }
    return sc_str_from_cstr("allowed");
}

static bool tool_status_is_approval_required(sc_status tool_status)
{
    return tool_status.code == SC_ERR_CANCELLED && tool_status.error_key != nullptr &&
           strcmp(tool_status.error_key, "sc.tool.approval_required") == 0;
}

static bool tool_status_is_turn_terminal(sc_status tool_status)
{
    if (sc_status_is_ok(tool_status) || tool_status.error_key == nullptr ||
        strcmp(tool_status.error_key, "sc.agent.tool_loop.repeated_terminal_failure") == 0) {
        return false;
    }
    if (tool_status.code == SC_ERR_UNSUPPORTED) {
        return true;
    }
    return strcmp(tool_status.error_key, "sc.browser_tool.cdp_connect_failed") == 0 ||
           strcmp(tool_status.error_key, "sc.browser_tool.websocket_client_unavailable") == 0;
}

static bool approval_denials_contains(const sc_vec *denials, const sc_provider_tool_call *provider_call)
{
    sc_str tool_name = provider_call == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&provider_call->name);

    if (denials == nullptr || provider_call == nullptr) {
        return false;
    }
    for (size_t i = 0; i < denials->len; i += 1) {
        const tool_approval_denial *denial = sc_vec_at_const(denials, i);
        if (denial != nullptr &&
            sc_str_equal(sc_string_as_str(&denial->tool_name), tool_name)) {
            return true;
        }
    }
    return false;
}

static sc_status approval_denials_add(sc_vec *denials, sc_allocator *alloc, const sc_provider_tool_call *provider_call)
{
    tool_approval_denial denial = {0};
    sc_status status = sc_status_ok();

    if (denials == nullptr || provider_call == nullptr) {
        return sc_status_invalid_argument("sc.agent.approval_denial.invalid_argument");
    }
    if (approval_denials_contains(denials, provider_call)) {
        return sc_status_ok();
    }

    status = sc_string_from_str(alloc, sc_string_as_str(&provider_call->name), &denial.tool_name);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(denials, &denial);
    }
    if (!sc_status_is_ok(status)) {
        tool_approval_denial_clear(&denial);
    }
    return status;
}

static void approval_denials_clear(sc_vec *denials)
{
    if (denials == nullptr) {
        return;
    }
    for (size_t i = 0; i < denials->len; i += 1) {
        tool_approval_denial *denial = sc_vec_at(denials, i);
        tool_approval_denial_clear(denial);
    }
    sc_vec_clear(denials);
}

static void tool_approval_denial_clear(tool_approval_denial *denial)
{
    if (denial == nullptr) {
        return;
    }
    sc_string_clear(&denial->tool_name);
    *denial = (tool_approval_denial){0};
}

static bool approval_grants_contains(const sc_vec *grants, const sc_provider_tool_call *provider_call)
{
    sc_str tool_name = provider_call == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&provider_call->name);
    sc_str arguments_json = provider_call == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&provider_call->arguments_json);

    if (grants == nullptr || provider_call == nullptr) {
        return false;
    }
    for (size_t i = 0; i < grants->len; i += 1) {
        const tool_approval_grant *grant = sc_vec_at_const(grants, i);
        if (grant != nullptr &&
            sc_str_equal(sc_string_as_str(&grant->tool_name), tool_name) &&
            sc_str_equal(sc_string_as_str(&grant->arguments_json), arguments_json)) {
            return true;
        }
    }
    return false;
}

static sc_status approval_grants_add(sc_vec *grants, sc_allocator *alloc, const sc_provider_tool_call *provider_call)
{
    tool_approval_grant grant = {0};
    sc_status status = sc_status_ok();

    if (grants == nullptr || provider_call == nullptr) {
        return sc_status_invalid_argument("sc.agent.approval_grant.invalid_argument");
    }
    if (approval_grants_contains(grants, provider_call)) {
        return sc_status_ok();
    }

    status = sc_string_from_str(alloc, sc_string_as_str(&provider_call->name), &grant.tool_name);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&provider_call->arguments_json), &grant.arguments_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(grants, &grant);
    }
    if (!sc_status_is_ok(status)) {
        tool_approval_grant_clear(&grant);
    }
    return status;
}

static void approval_grants_clear(sc_vec *grants)
{
    if (grants == nullptr) {
        return;
    }
    for (size_t i = 0; i < grants->len; i += 1) {
        tool_approval_grant *grant = sc_vec_at(grants, i);
        tool_approval_grant_clear(grant);
    }
    sc_vec_clear(grants);
}

static void tool_approval_grant_clear(tool_approval_grant *grant)
{
    if (grant == nullptr) {
        return;
    }
    sc_string_clear(&grant->tool_name);
    sc_string_clear(&grant->arguments_json);
    *grant = (tool_approval_grant){0};
}

static tool_success_cache *tool_success_cache_find(sc_vec *successes, const sc_provider_tool_call *provider_call)
{
    sc_str tool_name = provider_call == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&provider_call->name);
    sc_str arguments_json = provider_call == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&provider_call->arguments_json);

    if (successes == nullptr || provider_call == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < successes->len; i += 1) {
        tool_success_cache *entry = sc_vec_at(successes, i);
        if (entry != nullptr &&
            sc_str_equal(sc_string_as_str(&entry->tool_name), tool_name) &&
            sc_str_equal(sc_string_as_str(&entry->arguments_json), arguments_json)) {
            return entry;
        }
    }
    return nullptr;
}

static tool_success_cache *tool_success_cache_find_attachment_by_name(sc_vec *successes,
                                                                      const sc_provider_tool_call *provider_call)
{
    sc_str tool_name = provider_call == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&provider_call->name);

    if (successes == nullptr || provider_call == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < successes->len; i += 1) {
        tool_success_cache *entry = sc_vec_at(successes, i);
        if (entry != nullptr &&
            entry->attachment_bytes.len > 0 &&
            sc_str_equal(sc_string_as_str(&entry->tool_name), tool_name)) {
            return entry;
        }
    }
    return nullptr;
}

static sc_status tool_success_cache_add(sc_vec *successes,
                                        sc_allocator *alloc,
                                        const sc_provider_tool_call *provider_call,
                                        const sc_tool_result *tool_result)
{
    tool_success_cache entry = {0};
    sc_status status = sc_status_ok();

    if (successes == nullptr || provider_call == nullptr || tool_result == nullptr) {
        return sc_status_invalid_argument("sc.agent.success_cache.invalid_argument");
    }
    if (tool_success_cache_find(successes, provider_call) != nullptr) {
        return sc_status_ok();
    }

    status = sc_string_from_str(alloc, sc_string_as_str(&provider_call->name), &entry.tool_name);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&provider_call->arguments_json), &entry.arguments_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&tool_result->output), &entry.output);
    }
    if (sc_status_is_ok(status) && tool_result->attachment_bytes.len > 0) {
        status = sc_string_from_str(alloc,
                                    sc_string_as_str(&tool_result->attachment_content_type),
                                    &entry.attachment_content_type);
    }
    if (sc_status_is_ok(status) && tool_result->attachment_bytes.len > 0) {
        status = sc_string_from_str(alloc,
                                    sc_string_as_str(&tool_result->attachment_filename),
                                    &entry.attachment_filename);
    }
    if (sc_status_is_ok(status) && tool_result->attachment_bytes.len > 0) {
        status = sc_bytes_from_buf(alloc,
                                   sc_buf_from_parts(tool_result->attachment_bytes.ptr,
                                                     tool_result->attachment_bytes.len),
                                   &entry.attachment_bytes);
    }
    if (sc_status_is_ok(status) && tool_result->attachment_bytes.len > 0) {
        entry.attachment_delivery = tool_result->attachment_delivery;
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(successes, &entry);
    }
    if (!sc_status_is_ok(status)) {
        tool_success_cache_entry_clear(&entry);
    }
    return status;
}

static sc_status tool_result_from_success_cache(sc_allocator *alloc,
                                                const tool_success_cache *cached,
                                                sc_tool_result *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (cached == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.agent.success_cache.invalid_argument");
    }

    *out = (sc_tool_result){.struct_size = sizeof(*out), .success = true};
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "duplicate tool call suppressed; previous result reused");
    if (sc_status_is_ok(status) && cached->output.len > 0) {
        status = sc_string_builder_append_cstr(&builder, ": ");
    }
    if (sc_status_is_ok(status) && cached->output.len > 0) {
        status = sc_string_builder_append(&builder, sc_string_as_str(&cached->output));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &out->output);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status) && cached->attachment_bytes.len > 0) {
        status = sc_string_from_str(alloc,
                                    sc_string_as_str(&cached->attachment_content_type),
                                    &out->attachment_content_type);
    }
    if (sc_status_is_ok(status) && cached->attachment_bytes.len > 0) {
        status = sc_string_from_str(alloc,
                                    sc_string_as_str(&cached->attachment_filename),
                                    &out->attachment_filename);
    }
    if (sc_status_is_ok(status) && cached->attachment_bytes.len > 0) {
        status = sc_bytes_from_buf(alloc,
                                   sc_buf_from_parts(cached->attachment_bytes.ptr,
                                                     cached->attachment_bytes.len),
                                   &out->attachment_bytes);
    }
    if (sc_status_is_ok(status) && cached->attachment_bytes.len > 0) {
        out->attachment_delivery = cached->attachment_delivery;
    }
    if (!sc_status_is_ok(status)) {
        sc_tool_result_clear(out);
    }
    return status;
}

static void tool_success_cache_clear(sc_vec *successes)
{
    if (successes == nullptr) {
        return;
    }
    for (size_t i = 0; i < successes->len; i += 1) {
        tool_success_cache *entry = sc_vec_at(successes, i);
        tool_success_cache_entry_clear(entry);
    }
    sc_vec_clear(successes);
}

static void tool_success_cache_entry_clear(tool_success_cache *entry)
{
    if (entry == nullptr) {
        return;
    }
    sc_string_clear(&entry->tool_name);
    sc_string_clear(&entry->arguments_json);
    sc_string_clear(&entry->output);
    sc_string_clear(&entry->attachment_content_type);
    sc_string_clear(&entry->attachment_filename);
    sc_bytes_clear(&entry->attachment_bytes);
    *entry = (tool_success_cache){0};
}

static bool tool_terminal_failures_contains(const sc_vec *failures, const sc_provider_tool_call *provider_call)
{
    sc_str tool_name = provider_call == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&provider_call->name);

    if (failures == nullptr || provider_call == nullptr) {
        return false;
    }
    for (size_t i = 0; i < failures->len; i += 1) {
        const tool_terminal_failure *failure = sc_vec_at_const(failures, i);
        if (failure != nullptr && sc_str_equal(sc_string_as_str(&failure->tool_name), tool_name)) {
            return true;
        }
    }
    return false;
}

static sc_status tool_terminal_failures_add(sc_vec *failures,
                                            sc_allocator *alloc,
                                            const sc_provider_tool_call *provider_call,
                                            sc_status tool_status)
{
    tool_terminal_failure failure = {0};
    sc_status status = sc_status_ok();

    if (failures == nullptr || provider_call == nullptr || tool_status.error_key == nullptr) {
        return sc_status_invalid_argument("sc.agent.terminal_failure.invalid_argument");
    }
    if (tool_terminal_failures_contains(failures, provider_call)) {
        return sc_status_ok();
    }

    status = sc_string_from_str(alloc, sc_string_as_str(&provider_call->name), &failure.tool_name);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(alloc, tool_status.error_key, &failure.error_key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(failures, &failure);
    }
    if (!sc_status_is_ok(status)) {
        tool_terminal_failure_clear(&failure);
    }
    return status;
}

static void tool_terminal_failures_clear(sc_vec *failures)
{
    if (failures == nullptr) {
        return;
    }
    for (size_t i = 0; i < failures->len; i += 1) {
        tool_terminal_failure *failure = sc_vec_at(failures, i);
        tool_terminal_failure_clear(failure);
    }
    sc_vec_clear(failures);
}

static void tool_terminal_failure_clear(tool_terminal_failure *failure)
{
    if (failure == nullptr) {
        return;
    }
    sc_string_clear(&failure->tool_name);
    sc_string_clear(&failure->error_key);
    *failure = (tool_terminal_failure){0};
}

static sc_status request_turn_tool_approval(const sc_turn *turn,
                                            const sc_provider_tool_call *provider_call,
                                            sc_allocator *alloc,
                                            bool *out_approved)
{
    if (out_approved == nullptr) {
        return sc_status_invalid_argument("sc.agent.approval_invalid_argument");
    }
    *out_approved = false;
    if (turn == nullptr || turn->request_tool_approval == nullptr || provider_call == nullptr) {
        return sc_status_cancelled("sc.tool.approval_required");
    }
    return turn->request_tool_approval(turn->request_tool_approval_user_data,
                                       sc_string_as_str(&provider_call->name),
                                       sc_string_as_str(&provider_call->arguments_json),
                                       alloc,
                                       out_approved);
}

static sc_str tool_outcome(sc_status tool_status, bool success)
{
    if (success) {
        return sc_str_from_cstr("ok");
    }
    if (tool_status.code == SC_ERR_SECURITY_DENIED) {
        return sc_str_from_cstr("denied");
    }
    if (tool_status.code == SC_ERR_CANCELLED && tool_status.error_key != nullptr &&
        strcmp(tool_status.error_key, "sc.tool.approval_required") == 0) {
        return sc_str_from_cstr("approval_required");
    }
    return sc_str_from_cstr("error");
}

static sc_str tool_failure_reason(sc_status tool_status)
{
    if (sc_status_is_ok(tool_status)) {
        return sc_str_from_parts(nullptr, 0);
    }
    return sc_str_from_cstr(tool_status.error_key == nullptr ? "sc.tool.failed" : tool_status.error_key);
}
