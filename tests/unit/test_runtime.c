#define _XOPEN_SOURCE 700

#include "sc/provider.h"
#include "sc/runtime.h"
#include "test_helpers.h"

#include "runtime/loop_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct scripted_step {
    const char *text;
    const char *tool_name[2];
    const char *tool_args[2];
    const char *required_prompt_substring;
    const char *forbidden_prompt_substring;
    const char *required_model;
    sc_status_code error_code;
    const char *error_key;
    int64_t input_tokens;
    int64_t output_tokens;
    double cost_usd;
} scripted_step;

typedef struct scripted_provider {
    sc_allocator *alloc;
    const scripted_step *steps;
    size_t step_count;
    size_t calls;
} scripted_provider;

typedef struct echo_tool {
    sc_allocator *alloc;
    sc_string name;
    sc_json_value *schema;
    const char *failure_key;
    int *invoke_count;
    bool attach;
    sc_attachment_delivery attachment_delivery;
    bool fail;
} echo_tool;

typedef struct record_observer {
    int calls;
} record_observer;

typedef struct loop_counter {
    int calls;
    bool saw_cancel;
} loop_counter;

typedef struct queue_record {
    int calls;
    int cancelled;
    sc_status_code last_status;
} queue_record;

typedef struct async_turn_record {
    int calls;
    sc_status_code status;
    sc_string output;
    size_t provider_calls;
    size_t tool_calls;
} async_turn_record;

typedef struct turn_event_record {
    size_t tool_called;
    size_t tool_denied;
    size_t tool_failed;
    char last_tool[64];
} turn_event_record;

typedef struct shutdown_record {
    int drain;
    int flush;
    int close;
    int destroy;
} shutdown_record;

typedef struct stream_provider {
    sc_allocator *alloc;
    bool omit_done;
} stream_provider;

typedef struct pending_provider {
    sc_allocator *alloc;
    sc_provider_generate_complete_fn complete;
    void *complete_user_data;
    sc_string last_prompt;
    size_t calls;
    bool pending;
} pending_provider;

static sc_status scripted_provider_new(sc_allocator *alloc,
                                       const scripted_step *steps,
                                       size_t step_count,
                                       sc_provider **out);
static sc_status scripted_generate(void *impl,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_provider_response *out);
static void scripted_destroy(void *impl);
static sc_status stream_provider_new(sc_allocator *alloc, bool omit_done, sc_provider **out);
static sc_status stream_generate(void *impl,
                                 const sc_provider_request *request,
                                 sc_allocator *alloc,
                                 sc_provider_response *out);
static sc_status stream_run(void *impl,
                            const sc_provider_request *request,
                            sc_allocator *alloc,
                            sc_provider_stream_callback callback,
                            void *callback_user_data);
static void stream_destroy(void *impl);
static sc_status pending_provider_new(sc_allocator *alloc, pending_provider **state_out, sc_provider **out);
static sc_status pending_generate(void *impl,
                                  const sc_provider_request *request,
                                  sc_allocator *alloc,
                                  sc_provider_response *out);
static sc_status pending_generate_async(void *impl,
                                        sc_async_context *context,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_generate_complete_fn complete,
                                        void *complete_user_data,
                                        sc_async_op **out);
static sc_status pending_provider_complete(pending_provider *provider, sc_str text);
static void pending_destroy(void *impl);
static sc_status add_tool_call(sc_provider_response *response,
                               sc_str call_id,
                               sc_str name,
                               sc_str args_json);
static sc_status echo_tool_new(sc_allocator *alloc, const char *name, bool fail, sc_tool **out);
static sc_status echo_tool_new_with_failure_key(sc_allocator *alloc,
                                                const char *name,
                                                const char *failure_key,
                                                sc_tool **out);
static sc_status echo_tool_new_with_counter(sc_allocator *alloc,
                                            const char *name,
                                            int *invoke_count,
                                            sc_tool **out);
static sc_status echo_tool_new_with_counter_and_attachment(sc_allocator *alloc,
                                                           const char *name,
                                                           int *invoke_count,
                                                           sc_tool **out);
static sc_status echo_tool_new_counting(sc_allocator *alloc,
                                        const char *name,
                                        int *invoke_count,
                                        bool attach,
                                        sc_tool **out);
static sc_status echo_spec(void *impl, sc_tool_spec *out);
static sc_status echo_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void echo_destroy(void *impl);
static sc_status record_emit(void *impl, const sc_observer_event *event);
static void record_destroy(void *impl);
static sc_status failing_observer_emit(void *impl, const sc_observer_event *event);
static void failing_observer_destroy(void *impl);
static sc_status loop_count_task(void *impl, const sc_cancel_token *cancel, sc_allocator *alloc);
static void queue_done(void *user_data, const sc_agent_turn_result *result, sc_status status);
static void async_turn_done(void *user_data, const sc_agent_turn_result *result, sc_status status);
static void async_turn_record_clear(async_turn_record *record);
static void record_turn_event(void *user_data, const sc_turn_event *event);
static sc_status approve_tool(void *user_data, sc_str tool_name, sc_str arguments_json, sc_allocator *alloc, bool *out_approved);
static sc_status decline_tool(void *user_data, sc_str tool_name, sc_str arguments_json, sc_allocator *alloc, bool *out_approved);
static sc_status shutdown_drain(void *user_data, sc_allocator *alloc);
static sc_status shutdown_flush(void *user_data, sc_allocator *alloc);
static sc_status shutdown_close(void *user_data, sc_allocator *alloc);
static sc_status shutdown_destroy(void *user_data, sc_allocator *alloc);
static sc_status make_temp_path(const char *suffix, sc_string *out);
static int write_file(sc_str path, const char *text);
static sc_status parse_json_object(sc_str json, sc_json_value **out);
static int test_no_tool_turn(void);
static int test_observer_failure_does_not_corrupt_turn(void);
static int test_single_and_multi_tool_turns(void);
static int test_receipt_presentation_controls(void);
static int test_denied_approval_and_failure_paths(void);
static int test_provider_fallback_memory_history_and_cancellation(void);
static int test_text_tool_recovery_filter_budget_and_context_trim(void);
static int test_history_orphan_pruning_and_compression(void);
static int test_runtime_loop_and_model_switch(void);
static int test_runtime_turn_queue_consecutive_messages(void);
static int test_agent_process_message_async(void);
static int test_runtime_facade_schema_and_streaming(void);
static int test_direct_tool_rules(void);

static const sc_provider_vtab scripted_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "scripted",
    .display_name = "Scripted provider",
    .feature_flag = "SC_PROVIDER_SCRIPTED_TEST",
    .capabilities = SC_CONTRACT_CAP_TOOLS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = scripted_generate,
    .destroy = scripted_destroy,
};

static const sc_provider_vtab stream_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "stream-test",
    .display_name = "Stream test provider",
    .feature_flag = "SC_PROVIDER_STREAM_TEST",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = stream_generate,
    .stream = stream_run,
    .destroy = stream_destroy,
    .provider_modes = SC_PROVIDER_MODE_CHAT | SC_PROVIDER_MODE_STREAM,
};

static const sc_provider_vtab pending_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "pending-test",
    .display_name = "Pending test provider",
    .feature_flag = "SC_PROVIDER_PENDING_TEST",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = pending_generate,
    .generate_async = pending_generate_async,
    .destroy = pending_destroy,
};

static const sc_tool_vtab echo_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock_tool",
    .display_name = "Mock tool",
    .feature_flag = "SC_TOOL_MOCK_TEST",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = echo_spec,
    .invoke = echo_invoke,
    .destroy = echo_destroy,
};

static const sc_observer_vtab record_observer_vtab = {
    .struct_size = sizeof(sc_observer_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "record-observer",
    .display_name = "Record observer",
    .feature_flag = "SC_OBSERVER_RECORD_TEST",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .emit = record_emit,
    .flush = nullptr,
    .destroy = record_destroy,
};

static const sc_observer_vtab failing_observer_vtab = {
    .struct_size = sizeof(sc_observer_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "failing-observer",
    .display_name = "Failing observer",
    .feature_flag = "SC_OBSERVER_FAIL_TEST",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .emit = failing_observer_emit,
    .flush = nullptr,
    .destroy = failing_observer_destroy,
};

int main(void)
{
    int failures = 0;

    failures += test_no_tool_turn();
    failures += test_observer_failure_does_not_corrupt_turn();
    failures += test_single_and_multi_tool_turns();
    failures += test_receipt_presentation_controls();
    failures += test_denied_approval_and_failure_paths();
    failures += test_provider_fallback_memory_history_and_cancellation();
    failures += test_text_tool_recovery_filter_budget_and_context_trim();
    failures += test_history_orphan_pruning_and_compression();
    failures += test_runtime_loop_and_model_switch();
    failures += test_runtime_turn_queue_consecutive_messages();
    failures += test_agent_process_message_async();
    failures += test_runtime_facade_schema_and_streaming();
    failures += test_direct_tool_rules();

    return failures == 0 ? 0 : 1;
}

static int test_no_tool_turn(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_agent_turn_result result = {0};
    record_observer *observer_impl = nullptr;
    sc_observer *observer = nullptr;
    sc_turn turn = {.struct_size = sizeof(turn), .input = sc_str_from_cstr("hello"), .session_id = sc_str_from_cstr("s")};

    observer_impl = sc_alloc(sc_allocator_heap(), sizeof(*observer_impl), _Alignof(record_observer));
    if (observer_impl != nullptr) {
        *observer_impl = (record_observer){0};
    }
    failures += sc_test_expect_true("observer impl", observer_impl != nullptr);
    failures += sc_test_expect_status("observer new", sc_observer_new(sc_allocator_heap(), &record_observer_vtab, observer_impl, &observer), SC_OK);
    failures += sc_test_expect_status("mock provider",
                              sc_provider_mock_new(sc_allocator_heap(), SC_PROVIDER_MOCK_TEXT, sc_str_from_cstr("assistant"), &provider),
                              SC_OK);
    failures += sc_test_expect_status("agent new",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .observer = observer,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("agent process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("no-tool output", strcmp(result.output.ptr, "assistant") == 0);
    failures += sc_test_expect_true("provider called once", result.provider_call_count == 1);
    failures += sc_test_expect_true("no tool calls", result.tool_call_count == 0);
    failures += sc_test_expect_true("events emitted", result.events.len >= 5);
    failures += sc_test_expect_true("observer called", observer_impl->calls >= 2);
    failures += sc_test_expect_true("history stored", sc_agent_history_len(agent) == 2);

    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_observer_destroy(observer);
    return failures;
}

static int test_observer_failure_does_not_corrupt_turn(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_observer *observer = nullptr;
    sc_agent_turn_result result = {0};
    sc_turn turn = {.struct_size = sizeof(turn), .input = sc_str_from_cstr("hello"), .session_id = sc_str_from_cstr("s")};

    failures += sc_test_expect_status("failing observer new",
                              sc_observer_new(sc_allocator_heap(), &failing_observer_vtab, nullptr, &observer),
                              SC_OK);
    failures += sc_test_expect_status("mock provider",
                              sc_provider_mock_new(sc_allocator_heap(), SC_PROVIDER_MOCK_TEXT, sc_str_from_cstr("assistant"), &provider),
                              SC_OK);
    failures += sc_test_expect_status("agent new",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .observer = observer,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("agent process ignores observer failure",
                              sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result),
                              SC_OK);
    failures += sc_test_expect_true("observer failure output", strcmp(result.output.ptr, "assistant") == 0);

    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_observer_destroy(observer);
    return failures;
}

static int test_single_and_multi_tool_turns(void)
{
    int failures = 0;
    const scripted_step single_steps[] = {
        {.text = "need-tool", .tool_name = {"mock_tool", nullptr}, .tool_args = {"{\"value\":\"one\"}", nullptr}},
        {.text = "done", .required_prompt_substring = "output=one"},
    };
    const scripted_step multi_steps[] = {
        {.text = "multi", .tool_name = {"mock_tool", "mock_tool"}, .tool_args = {"{\"value\":\"one\"}", "{\"value\":\"two\"}"}},
        {.text = "done", .required_prompt_substring = "output=two"},
    };
    sc_provider *provider = nullptr;
    sc_tool *tool = nullptr;
    sc_agent *agent = nullptr;
    sc_agent_turn_result result = {0};
    sc_tool *tools[1] = {nullptr};
    turn_event_record event_record = {0};
    sc_turn turn = {.struct_size = sizeof(turn), .input = sc_str_from_cstr("run"), .session_id = sc_str_from_cstr("s")};
    turn.event_callback = record_turn_event;
    turn.event_callback_user_data = &event_record;

    failures += sc_test_expect_status("echo tool", echo_tool_new(sc_allocator_heap(), "mock_tool", false, &tool), SC_OK);
    tools[0] = tool;
    failures += sc_test_expect_status("single provider", scripted_provider_new(sc_allocator_heap(), single_steps, 2, &provider), SC_OK);
    failures += sc_test_expect_status("single agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("single process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("single output", strstr(result.output.ptr, "done") != nullptr);
    failures += sc_test_expect_true("single tool count", result.tool_call_count == 1);
    failures += sc_test_expect_true("single receipt", result.receipts.receipts.len == 1 && sc_receipt_chain_verify(&result.receipts));
    failures += sc_test_expect_true("single tool event callback", event_record.tool_called == 1);
    failures += sc_test_expect_true("single tool event name", strcmp(event_record.last_tool, "mock_tool") == 0);
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    agent = nullptr;
    provider = nullptr;
    event_record = (turn_event_record){0};

    failures += sc_test_expect_status("multi provider", scripted_provider_new(sc_allocator_heap(), multi_steps, 2, &provider), SC_OK);
    failures += sc_test_expect_status("multi agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("multi process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("multi tool count", result.tool_call_count == 2);
    failures += sc_test_expect_true("multi provider calls", result.provider_call_count == 2);
    failures += sc_test_expect_true("multi receipts", result.receipts.receipts.len == 2 && sc_receipt_chain_verify(&result.receipts));
    failures += sc_test_expect_true("multi tool event callbacks", event_record.tool_called == 2);

    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_tool_destroy(tool);
    return failures;
}

static int test_receipt_presentation_controls(void)
{
    int failures = 0;
    const scripted_step shown_steps[] = {
        {.text = "need",
         .tool_name = {"mock_tool", nullptr},
         .tool_args = {"{\"value\":\"visible\"}", nullptr},
         .required_prompt_substring = "do not include receipt tokens"},
        {.text = "done [receipt: sc-receipt-fake]"},
    };
    const scripted_step hidden_steps[] = {
        {.text = "need", .tool_name = {"mock_tool", nullptr}, .tool_args = {"{\"value\":\"hidden\"}", nullptr}},
        {.text = "done"},
    };
    sc_security_policy policy = {0};
    sc_provider *provider = nullptr;
    sc_tool *tool = nullptr;
    sc_tool *tools[1] = {nullptr};
    sc_agent *agent = nullptr;
    sc_agent_turn_result result = {0};
    sc_turn turn = {.struct_size = sizeof(turn), .input = sc_str_from_cstr("run"), .session_id = sc_str_from_cstr("s")};

    failures += sc_test_expect_status("receipt policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    policy.receipts_show_in_response = true;
    failures += sc_test_expect_status("receipt tool", echo_tool_new(sc_allocator_heap(), "mock_tool", false, &tool), SC_OK);
    tools[0] = tool;
    failures += sc_test_expect_status("receipt provider", scripted_provider_new(sc_allocator_heap(), shown_steps, 2, &provider), SC_OK);
    failures += sc_test_expect_status("receipt agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                               .policy = &policy,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("receipt process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("receipt output hides tokens",
                            strstr(result.output.ptr, "Tool receipts:") != nullptr &&
                                strstr(result.output.ptr, "sc-receipt-") == nullptr);
    failures += sc_test_expect_true("receipt chain visible", result.receipts.receipts.len == 1 && sc_receipt_chain_verify(&result.receipts));
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    agent = nullptr;
    provider = nullptr;

    policy.receipts_enabled = false;
    failures += sc_test_expect_status("receipt hidden provider", scripted_provider_new(sc_allocator_heap(), hidden_steps, 2, &provider), SC_OK);
    failures += sc_test_expect_status("receipt hidden agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                               .policy = &policy,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("receipt hidden process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("receipt hidden output",
                            strstr(result.output.ptr, "Tool receipts:") == nullptr &&
                                strstr(result.output.ptr, "sc-receipt-") == nullptr);
    failures += sc_test_expect_true("receipt hidden chain", result.receipts.receipts.len == 0);

    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_tool_destroy(tool);
    sc_security_policy_clear(&policy);
    return failures;
}

static int test_denied_approval_and_failure_paths(void)
{
    int failures = 0;
    const scripted_step denied_steps[] = {
        {.text = "try read", .tool_name = {"file_read", nullptr}, .tool_args = {"{\"path\":\"/tmp/sc-runtime-denied.txt\"}", nullptr}},
        {.text = "denied handled", .required_prompt_substring = "success=false"},
    };
    const scripted_step approval_steps[] = {
        {.text = "approval", .tool_name = {"approval_test", nullptr}, .tool_args = {"{\"message\":\"pending\"}", nullptr}},
    };
    const scripted_step approved_steps[] = {
        {.text = "approval", .tool_name = {"approval_test", nullptr}, .tool_args = {"{\"message\":\"accepted\"}", nullptr}},
        {.text = "approval repeat",
         .tool_name = {"approval_test", nullptr},
         .tool_args = {"{\"message\":\"accepted\"}", nullptr},
         .required_prompt_substring = "success=true"},
        {.text = "approved handled", .required_prompt_substring = "success=true"},
    };
    const scripted_step declined_steps[] = {
        {.text = "approval", .tool_name = {"approval_test", nullptr}, .tool_args = {"{\"message\":\"declined\"}", nullptr}},
        {.text = "approval again",
         .tool_name = {"approval_test", nullptr},
         .tool_args = {"{\"message\":\"changed\"}", nullptr},
         .required_prompt_substring = "sc.tool.approval_declined"},
        {.text = "declined handled", .required_prompt_substring = "sc.tool.approval_declined"},
    };
    const scripted_step failure_steps[] = {
        {.text = "fail", .tool_name = {"mock_tool", nullptr}, .tool_args = {"{\"value\":\"x\"}", nullptr}},
        {.text = "failure handled", .required_prompt_substring = "sc.test_tool.failed"},
    };
    const scripted_step repeated_terminal_steps[] = {
        {.text = "first terminal", .tool_name = {"mock_tool", nullptr}, .tool_args = {"{\"value\":\"x\"}", nullptr}},
        {.text = "repeat terminal",
         .tool_name = {"mock_tool", nullptr},
         .tool_args = {"{\"value\":\"x\"}", nullptr},
         .required_prompt_substring = "sc.browser_tool.cdp_connect_failed"},
        {.text = "terminal handled", .required_prompt_substring = "sc.agent.tool_loop.repeated_terminal_failure"},
    };
    const scripted_step repeated_success_steps[] = {
        {.text = "", .tool_name = {"mock_tool", nullptr}, .tool_args = {"{\"value\":\"same\"}", nullptr}},
        {.text = "",
         .tool_name = {"mock_tool", nullptr},
         .tool_args = {"{\"value\":\"same\"}", nullptr},
         .required_prompt_substring = "output=same"},
        {.text = "",
         .tool_name = {"mock_tool", nullptr},
         .tool_args = {"{\"value\":\"same\"}", nullptr},
         .required_prompt_substring = "duplicate tool call suppressed"},
        {.text = "unreachable"},
    };
    const scripted_step repeated_attachment_steps[] = {
        {.text = "", .tool_name = {"mock_tool", nullptr}, .tool_args = {"{\"value\":\"first\"}", nullptr}},
        {.text = "",
         .tool_name = {"mock_tool", nullptr},
         .tool_args = {"{\"value\":\"changed\"}", nullptr},
         .required_prompt_substring = "output=first"},
        {.text = "unreachable"},
    };
    sc_security_policy policy = {0};
    sc_tool_context context = {0};
    sc_provider *provider = nullptr;
    sc_tool *tool = nullptr;
    sc_agent *agent = nullptr;
    sc_agent_turn_result result = {0};
    sc_tool *tools[1] = {nullptr};
    int approval_calls = 0;
    int decline_calls = 0;
    int repeated_success_invocations = 0;
    int repeated_attachment_invocations = 0;
    sc_turn turn = {.struct_size = sizeof(turn), .input = sc_str_from_cstr("run"), .session_id = sc_str_from_cstr("s")};

    failures += sc_test_expect_true("write denied fixture", write_file(sc_str_from_cstr("/tmp/sc-runtime-denied.txt"), "secret") == 0);
    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("deny file_read", sc_security_policy_add_denied_tool(&policy, sc_str_from_cstr("file_read")), SC_OK);
    context = (sc_tool_context){.struct_size = sizeof(context), .policy = &policy};
    failures += sc_test_expect_status("file tool", sc_tool_file_read_new(sc_allocator_heap(), &context, &tool), SC_OK);
    tools[0] = tool;
    failures += sc_test_expect_status("denied provider", scripted_provider_new(sc_allocator_heap(), denied_steps, 2, &provider), SC_OK);
    failures += sc_test_expect_status("denied agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                               .policy = &policy,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("denied process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("denied receipt failed", result.receipts.receipts.len == 1);
    if (result.receipts.receipts.len == 1) {
        const sc_tool_receipt *receipt = sc_vec_at_const(&result.receipts.receipts, 0);
        failures += sc_test_expect_true("denied receipt policy",
                                receipt != nullptr && strcmp(receipt->policy_decision.ptr, "denied") == 0);
        failures += sc_test_expect_true("denied receipt reason",
                                receipt != nullptr && strcmp(receipt->failure_reason.ptr, "sc.security.tool_denied") == 0);
        failures += sc_test_expect_true("denied receipt outcome",
                                receipt != nullptr && strcmp(receipt->outcome.ptr, "denied") == 0);
    }
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    agent = nullptr;
    provider = nullptr;
    sc_tool_destroy(tool);
    sc_security_policy_clear(&policy);
    tool = nullptr;

    failures += sc_test_expect_status("policy defaults 2", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    context = (sc_tool_context){.struct_size = sizeof(context), .policy = &policy};
    failures += sc_test_expect_status("approval test tool", sc_tool_approval_test_new(sc_allocator_heap(), &context, &tool), SC_OK);
    tools[0] = tool;

    turn.request_tool_approval = decline_tool;
    turn.request_tool_approval_user_data = &decline_calls;
    failures += sc_test_expect_status("declined provider", scripted_provider_new(sc_allocator_heap(), declined_steps, 3, &provider), SC_OK);
    failures += sc_test_expect_status("declined agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                               .policy = &policy,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("declined process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("declined approval callback not repeated", decline_calls == 1);
    failures += sc_test_expect_true("declined repeated tool call denied", result.tool_call_count == 2);
    failures += sc_test_expect_true("declined output", strstr(result.output.ptr, "declined handled") != nullptr);
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    agent = nullptr;
    provider = nullptr;
    turn.request_tool_approval = nullptr;
    turn.request_tool_approval_user_data = nullptr;

    failures += sc_test_expect_status("approval provider", scripted_provider_new(sc_allocator_heap(), approval_steps, 1, &provider), SC_OK);
    failures += sc_test_expect_status("approval agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                               .policy = &policy,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("approval process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_ERR_CANCELLED);
    failures += sc_test_expect_true("approval cancelled flag", result.cancelled);
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    agent = nullptr;
    provider = nullptr;

    turn.request_tool_approval = approve_tool;
    turn.request_tool_approval_user_data = &approval_calls;
    failures += sc_test_expect_status("approved provider",
                              scripted_provider_new(sc_allocator_heap(), approved_steps, SC_ARRAY_LEN(approved_steps), &provider),
                              SC_OK);
    failures += sc_test_expect_status("approved agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                               .policy = &policy,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("approved process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("approval callback cached for repeated args", approval_calls == 1);
    failures += sc_test_expect_true("approved repeated tool call completed", result.tool_call_count == 2);
    failures += sc_test_expect_true("approved output", strstr(result.output.ptr, "approved handled") != nullptr);
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_tool_destroy(tool);
    sc_security_policy_clear(&policy);
    turn.request_tool_approval = nullptr;
    turn.request_tool_approval_user_data = nullptr;
    agent = nullptr;
    provider = nullptr;
    tool = nullptr;

    failures += sc_test_expect_status("failing tool", echo_tool_new(sc_allocator_heap(), "mock_tool", true, &tool), SC_OK);
    tools[0] = tool;
    failures += sc_test_expect_status("failure provider", scripted_provider_new(sc_allocator_heap(), failure_steps, 2, &provider), SC_OK);
    failures += sc_test_expect_status("failure agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("failure process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("failure reached provider", strstr(result.output.ptr, "failure handled") != nullptr);

    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_tool_destroy(tool);
    agent = nullptr;
    provider = nullptr;
    tool = nullptr;

    failures += sc_test_expect_status("terminal failing tool",
                              echo_tool_new_with_failure_key(sc_allocator_heap(),
                                                             "mock_tool",
                                                             "sc.browser_tool.cdp_connect_failed",
                                                             &tool),
                              SC_OK);
    tools[0] = tool;
    failures += sc_test_expect_status("terminal provider",
                              scripted_provider_new(sc_allocator_heap(),
                                                    repeated_terminal_steps,
                                                    SC_ARRAY_LEN(repeated_terminal_steps),
                                                    &provider),
                              SC_OK);
    failures += sc_test_expect_status("terminal agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("terminal process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("terminal repeated tool suppressed", result.tool_call_count == 2);
    failures += sc_test_expect_true("terminal reached provider", strstr(result.output.ptr, "terminal handled") != nullptr);

    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_tool_destroy(tool);
    agent = nullptr;
    provider = nullptr;
    tool = nullptr;

    failures += sc_test_expect_status("repeated success tool",
                              echo_tool_new_with_counter(sc_allocator_heap(),
                                                         "mock_tool",
                                                         &repeated_success_invocations,
                                                         &tool),
                              SC_OK);
    tools[0] = tool;
    failures += sc_test_expect_status("repeated success provider",
                              scripted_provider_new(sc_allocator_heap(),
                                                    repeated_success_steps,
                                                    SC_ARRAY_LEN(repeated_success_steps),
                                                    &provider),
                              SC_OK);
    failures += sc_test_expect_status("repeated success agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("repeated success process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("repeated success invoked once", repeated_success_invocations == 1);
    failures += sc_test_expect_true("repeated success counted provider calls", result.provider_call_count == 3);
    failures += sc_test_expect_true("repeated success counted tool calls", result.tool_call_count == 3);
    failures += sc_test_expect_true("repeated success completed from cache",
                            strstr(result.output.ptr, "The requested tool action completed.") != nullptr);

    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_tool_destroy(tool);
    agent = nullptr;
    provider = nullptr;
    tool = nullptr;

    failures += sc_test_expect_status("repeated attachment tool",
                              echo_tool_new_with_counter_and_attachment(sc_allocator_heap(),
                                                                        "mock_tool",
                                                                        &repeated_attachment_invocations,
                                                                        &tool),
                              SC_OK);
    tools[0] = tool;
    failures += sc_test_expect_status("repeated attachment provider",
                              scripted_provider_new(sc_allocator_heap(),
                                                    repeated_attachment_steps,
                                                    SC_ARRAY_LEN(repeated_attachment_steps),
                                                    &provider),
                              SC_OK);
    failures += sc_test_expect_status("repeated attachment agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("repeated attachment process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("repeated attachment invoked once", repeated_attachment_invocations == 1);
    failures += sc_test_expect_true("repeated attachment counted provider calls", result.provider_call_count == 2);
    failures += sc_test_expect_true("repeated attachment counted tool calls", result.tool_call_count == 2);
    failures += sc_test_expect_true("repeated attachment preserved bytes", result.attachment_bytes.len == 3);
    failures += sc_test_expect_true("repeated attachment preserved delivery",
                            result.attachment_delivery == SC_ATTACHMENT_DELIVERY_DOCUMENT);
    failures += sc_test_expect_true("repeated attachment completed from cache",
                            strstr(result.output.ptr, "The requested tool result is attached.") != nullptr);

    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_tool_destroy(tool);
    return failures;
}

static int test_provider_fallback_memory_history_and_cancellation(void)
{
    int failures = 0;
    const scripted_step memory_steps[] = {
        {.text = "memory ok", .required_prompt_substring = "hint: remembered"},
        {.text = "memory disabled ok", .forbidden_prompt_substring = "hint: remembered"},
    };
    sc_provider *error_provider = nullptr;
    sc_provider *text_provider = nullptr;
    sc_provider *fallback = nullptr;
    sc_provider *scripted = nullptr;
    sc_provider *providers[2] = {nullptr, nullptr};
    sc_agent *agent = nullptr;
    sc_agent_turn_result result = {0};
    sc_memory *memory = nullptr;
    sc_string memory_path = {0};
    sc_memory_record record = {0};
    sc_memory_query query = {0};
    sc_memory_result memory_result = {0};
    sc_turn turn = {.struct_size = sizeof(turn), .input = sc_str_from_cstr("hello"), .session_id = sc_str_from_cstr("session-a")};
    sc_turn memory_turn = {.struct_size = sizeof(memory_turn), .input = sc_str_from_cstr("remembered"), .session_id = sc_str_from_cstr("session-a")};
    sc_turn memory_disabled_turn = {
        .struct_size = sizeof(memory_disabled_turn),
        .input = sc_str_from_cstr("remembered"),
        .session_id = sc_str_from_cstr("session-a"),
        .disable_memory = true,
    };
    sc_turn cancelled = {.struct_size = sizeof(cancelled), .input = sc_str_from_cstr("stop"), .cancel_requested = true};

    failures += sc_test_expect_status("error provider",
                              sc_provider_mock_new(sc_allocator_heap(), SC_PROVIDER_MOCK_ERROR, sc_str_from_cstr("error"), &error_provider),
                              SC_OK);
    failures += sc_test_expect_status("text provider",
                              sc_provider_mock_new(sc_allocator_heap(), SC_PROVIDER_MOCK_TEXT, sc_str_from_cstr("fallback"), &text_provider),
                              SC_OK);
    providers[0] = error_provider;
    providers[1] = text_provider;
    failures += sc_test_expect_status("reliable provider", sc_provider_reliable_new(sc_allocator_heap(), providers, 2, &fallback), SC_OK);
    failures += sc_test_expect_status("fallback agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = fallback,
                                               .max_history_messages = 2,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("fallback process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("fallback output", strcmp(result.output.ptr, "fallback") == 0);
    sc_agent_turn_result_clear(&result);
    failures += sc_test_expect_status("history turn 2", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("history trimmed", sc_agent_history_len(agent) == 2);
    sc_agent_turn_result_clear(&result);
    failures += sc_test_expect_status("cancelled input", sc_agent_process_message(agent, &cancelled, sc_allocator_heap(), &result), SC_ERR_CANCELLED);
    failures += sc_test_expect_true("cancel event", result.cancelled && result.events.len == 1);
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(fallback);
    sc_provider_destroy(error_provider);
    sc_provider_destroy(text_provider);
    agent = nullptr;

    failures += sc_test_expect_status("memory path", make_temp_path("runtime-memory.md", &memory_path), SC_OK);
    failures += sc_test_expect_status("markdown memory", sc_memory_markdown_open(sc_allocator_heap(), sc_string_as_str(&memory_path), &memory), SC_OK);
    record = (sc_memory_record){
        .struct_size = sizeof(record),
        .namespace_name = sc_str_from_cstr("runtime.memory"),
        .session_id = sc_str_from_cstr("session-a"),
        .category = sc_str_from_cstr("note"),
        .key = sc_str_from_cstr("hint"),
        .value = sc_str_from_cstr("remembered"),
        .score = 1.0,
        .importance = 10,
    };
    failures += sc_test_expect_status("seed memory", sc_memory_put(memory, &record), SC_OK);
    failures += sc_test_expect_status("memory scripted", scripted_provider_new(sc_allocator_heap(), memory_steps, SC_ARRAY_LEN(memory_steps), &scripted), SC_OK);
    failures += sc_test_expect_status("memory agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = scripted,
                                               .memory = memory,
                                               .memory_namespace = sc_str_from_cstr("runtime.memory"),
                                               .turn_namespace = sc_str_from_cstr("runtime.turns"),
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("memory process", sc_agent_process_message(agent, &memory_turn, sc_allocator_heap(), &result), SC_OK);
    query = (sc_memory_query){
        .struct_size = sizeof(query),
        .namespace_name = sc_str_from_cstr("runtime.turns"),
        .session_id = sc_str_from_cstr("session-a"),
        .limit = 10,
    };
    failures += sc_test_expect_status("turn persisted search", sc_memory_search(memory, &query, sc_allocator_heap(), &memory_result), SC_OK);
    failures += sc_test_expect_true("turn persisted", memory_result.entries.len == 1);

    sc_memory_result_clear(&memory_result);
    sc_agent_turn_result_clear(&result);
    failures += sc_test_expect_status("memory disabled process",
                              sc_agent_process_message(agent, &memory_disabled_turn, sc_allocator_heap(), &result),
                              SC_OK);
    failures += sc_test_expect_true("memory disabled output", strcmp(result.output.ptr, "memory disabled ok") == 0);
    failures += sc_test_expect_status("memory disabled persisted search",
                              sc_memory_search(memory, &query, sc_allocator_heap(), &memory_result),
                              SC_OK);
    failures += sc_test_expect_true("memory disabled not persisted", memory_result.entries.len == 1);
    sc_memory_result_clear(&memory_result);
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(scripted);
    sc_memory_destroy(memory);
    sc_string_clear(&memory_path);
    return failures;
}

static int test_text_tool_recovery_filter_budget_and_context_trim(void)
{
    int failures = 0;
    const scripted_step text_tool_steps[] = {
        {.text = "before <tool_call>{\"name\":\"mock_tool\",\"arguments\":{\"value\":\"from-text\"}}</tool_call> after"},
        {.text = "done", .required_prompt_substring = "output=from-text"},
    };
    const scripted_step filter_steps[] = {
        {.text = "try hidden", .tool_name = {"hidden_tool", nullptr}, .tool_args = {"{\"value\":\"hidden\"}", nullptr}},
        {.text = "filtered handled", .required_prompt_substring = "sc.agent.tool_loop.filtered_tool"},
    };
    const scripted_step budget_steps[] = {
        {.text = "costly", .input_tokens = 10, .output_tokens = 5, .cost_usd = 0.25},
    };
    const scripted_step prune_steps[] = {
        {.text = "seed-budget"},
        {.text = "budget-pruned",
         .required_prompt_substring = "User message\nlatest",
         .forbidden_prompt_substring = "ancient-budget-context"},
    };
    const scripted_step context_steps[] = {
        {.text = "old-ok"},
        {.text = nullptr, .error_code = SC_ERR_HTTP, .error_key = "sc.provider.context_overflow"},
        {.text = "trimmed",
         .required_prompt_substring = "User message\nnew",
         .forbidden_prompt_substring = "ancient-overflow-context"},
    };
    const char *budget_old =
        "ancient-budget-context "
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    const char *overflow_old =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
        " ancient-overflow-context";
    sc_provider *provider = nullptr;
    sc_tool *tool = nullptr;
    sc_tool *hidden_tool = nullptr;
    sc_tool *tools[2] = {nullptr, nullptr};
    sc_agent *agent = nullptr;
    sc_agent_turn_result result = {0};
    sc_str allowed[1] = {sc_str_from_cstr("mock_tool")};
    sc_turn turn = {.struct_size = sizeof(turn), .input = sc_str_from_cstr("run"), .session_id = sc_str_from_cstr("s")};
    sc_turn filtered_turn = {
        .struct_size = sizeof(filtered_turn),
        .input = sc_str_from_cstr("run"),
        .session_id = sc_str_from_cstr("s"),
        .allowed_tools = allowed,
        .allowed_tool_count = 1,
    };
    sc_turn budget_turn = {
        .struct_size = sizeof(budget_turn),
        .input = sc_str_from_cstr("run"),
        .session_id = sc_str_from_cstr("s"),
        .max_total_tokens = 12,
        .max_cost_usd = 0.10,
    };
    sc_turn old_turn = {.struct_size = sizeof(old_turn), .input = sc_str_from_cstr(overflow_old), .session_id = sc_str_from_cstr("s")};
    sc_turn new_turn = {.struct_size = sizeof(new_turn), .input = sc_str_from_cstr("new"), .session_id = sc_str_from_cstr("s")};
    sc_turn prune_old_turn = {
        .struct_size = sizeof(prune_old_turn),
        .input = sc_str_from_cstr(budget_old),
        .session_id = sc_str_from_cstr("budget-session"),
    };
    sc_turn prune_new_turn = {
        .struct_size = sizeof(prune_new_turn),
        .input = sc_str_from_cstr("latest"),
        .session_id = sc_str_from_cstr("budget-session"),
    };

    failures += sc_test_expect_status("text tool", echo_tool_new(sc_allocator_heap(), "mock_tool", false, &tool), SC_OK);
    tools[0] = tool;
    failures += sc_test_expect_status("text provider", scripted_provider_new(sc_allocator_heap(), text_tool_steps, 2, &provider), SC_OK);
    failures += sc_test_expect_status("text agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 1,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("text process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("text recovered tool count", result.tool_call_count == 1);
    failures += sc_test_expect_true("text stripped tool markup", strstr(result.output.ptr, "tool_call") == nullptr);
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    agent = nullptr;
    provider = nullptr;

    failures += sc_test_expect_status("hidden tool", echo_tool_new(sc_allocator_heap(), "hidden_tool", false, &hidden_tool), SC_OK);
    tools[0] = tool;
    tools[1] = hidden_tool;
    failures += sc_test_expect_status("filter provider", scripted_provider_new(sc_allocator_heap(), filter_steps, 2, &provider), SC_OK);
    failures += sc_test_expect_status("filter agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = tools,
                                               .tool_count = 2,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("filter process", sc_agent_process_message(agent, &filtered_turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("filtered receipt", result.receipts.receipts.len == 1);
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    agent = nullptr;
    provider = nullptr;

    failures += sc_test_expect_status("budget provider", scripted_provider_new(sc_allocator_heap(), budget_steps, 1, &provider), SC_OK);
    failures += sc_test_expect_status("budget agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){.struct_size = sizeof(sc_agent_options), .provider = provider},
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("budget process", sc_agent_process_message(agent, &budget_turn, sc_allocator_heap(), &result), SC_ERR_CANCELLED);
    failures += sc_test_expect_true("budget fields", result.budget_exceeded && result.total_tokens == 15 && result.cost_usd > 0.20);
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    agent = nullptr;
    provider = nullptr;

    failures += sc_test_expect_status("prune provider", scripted_provider_new(sc_allocator_heap(), prune_steps, 2, &provider), SC_OK);
    failures += sc_test_expect_status("prune agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .max_prompt_bytes = 900,
                                               .max_history_messages = 8,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("prune seed", sc_agent_process_message(agent, &prune_old_turn, sc_allocator_heap(), &result), SC_OK);
    sc_agent_turn_result_clear(&result);
    failures += sc_test_expect_status("prune process", sc_agent_process_message(agent, &prune_new_turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("budget prune output", strcmp(result.output.ptr, "budget-pruned") == 0);
    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    agent = nullptr;
    provider = nullptr;

    failures += sc_test_expect_status("context provider", scripted_provider_new(sc_allocator_heap(), context_steps, 3, &provider), SC_OK);
    failures += sc_test_expect_status("context agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .max_prompt_bytes = 4096,
                                               .max_history_messages = 4,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("seed old history", sc_agent_process_message(agent, &old_turn, sc_allocator_heap(), &result), SC_OK);
    sc_agent_turn_result_clear(&result);
    failures += sc_test_expect_status("context retry process", sc_agent_process_message(agent, &new_turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("context retry output", strcmp(result.output.ptr, "trimmed") == 0);

    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_tool_destroy(hidden_tool);
    sc_tool_destroy(tool);
    return failures;
}

static int test_history_orphan_pruning_and_compression(void)
{
    int failures = 0;
    sc_history history = {0};
    sc_prompt_builder builder = {0};
    sc_string prompt = {0};

    sc_history_init(&history, sc_allocator_heap(), 8);
    failures += sc_test_expect_status("orphan tool append",
                              sc_history_append(&history, sc_str_from_cstr("tool"), sc_str_from_cstr("orphan output")),
                              SC_OK);
    failures += sc_test_expect_true("orphan pruned", history.messages.len == 0);

    failures += sc_test_expect_status("history user one",
                              sc_history_append(&history, sc_str_from_cstr("user"), sc_str_from_cstr("alpha request")),
                              SC_OK);
    failures += sc_test_expect_status("history assistant one",
                              sc_history_append(&history, sc_str_from_cstr("assistant"), sc_str_from_cstr("beta tool call")),
                              SC_OK);
    failures += sc_test_expect_status("history tool one",
                              sc_history_append(&history, sc_str_from_cstr("tool"), sc_str_from_cstr("gamma tool output")),
                              SC_OK);
    failures += sc_test_expect_status("history tool two",
                              sc_history_append(&history, sc_str_from_cstr("tool_result"), sc_str_from_cstr("delta second output")),
                              SC_OK);
    failures += sc_test_expect_status("history user two",
                              sc_history_append(&history, sc_str_from_cstr("user"), sc_str_from_cstr("epsilon request")),
                              SC_OK);
    failures += sc_test_expect_status("history assistant two",
                              sc_history_append(&history, sc_str_from_cstr("assistant"), sc_str_from_cstr("zeta response")),
                              SC_OK);
    failures += sc_test_expect_status("compress history", sc_history_compress_to_recent(&history, 2, 512), SC_OK);
    failures += sc_test_expect_true("compressed message count", history.messages.len == 3);

    sc_prompt_builder_init(&builder, sc_allocator_heap(), 2048);
    failures += sc_test_expect_status("compressed prompt", sc_history_append_to_prompt(&history, &builder), SC_OK);
    failures += sc_test_expect_status("compressed prompt finish", sc_prompt_builder_finish(&builder, &prompt), SC_OK);
    failures += sc_test_expect_true("compressed prompt has summary", strstr(prompt.ptr, "Compressed earlier context") != nullptr);
    failures += sc_test_expect_true("compressed prompt has old user", strstr(prompt.ptr, "alpha request") != nullptr);
    failures += sc_test_expect_true("compressed prompt has attached tool", strstr(prompt.ptr, "gamma tool output") != nullptr);
    failures += sc_test_expect_true("compressed prompt has recent user", strstr(prompt.ptr, "epsilon request") != nullptr);
    failures += sc_test_expect_true("compressed prompt drops orphan", strstr(prompt.ptr, "orphan output") == nullptr);

    sc_string_clear(&prompt);
    sc_prompt_builder_clear(&builder);
    sc_history_clear(&history);
    return failures;
}

static int test_runtime_loop_and_model_switch(void)
{
    int failures = 0;
    loop_counter counter = {0};
    queue_record queue = {0};
    shutdown_record shutdown = {0};
    sc_runtime_loop *loop = nullptr;
    sc_timer_handle *timer = nullptr;
    sc_runtime_turn_queue *turn_queue = nullptr;
    sc_cancel_token token = {0};
    const scripted_step model_steps[] = {
        {.text = "switched", .required_model = "model-b"},
    };
    const scripted_step queue_steps[] = {
        {.text = "cancelled"},
        {.text = "queued"},
    };
    sc_model_switch_request switch_request = {
        .struct_size = sizeof(switch_request),
        .model = sc_str_from_cstr("model-b"),
        .reason = sc_str_from_cstr("test"),
    };
    sc_turn turn = {
        .struct_size = sizeof(turn),
        .input = sc_str_from_cstr("run"),
        .model_switch = &switch_request,
    };
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_agent_turn_result result = {0};

    failures += sc_test_expect_status("loop new",
                              sc_runtime_loop_new(sc_allocator_heap(),
                                                  &(sc_runtime_loop_options){
                                                      .struct_size = sizeof(sc_runtime_loop_options),
                                                      .max_iterations = 3,
                                                  },
                                                  &loop),
                              SC_OK);
    failures += sc_test_expect_status("timer start",
                              sc_runtime_timer_start(loop,
                                                     &(sc_timer_options){
                                                         .struct_size = sizeof(sc_timer_options),
                                                         .name = sc_str_from_cstr("deadline"),
                                                         .run = loop_count_task,
                                                         .user_data = &counter,
                                                     },
                                                     &timer),
                              SC_OK);
    failures += sc_test_expect_true("timer active", sc_runtime_timer_active(timer) && sc_runtime_timer_deadline_ns(timer) > 0);
    failures += sc_test_expect_status("timer cancel", sc_runtime_timer_cancel(timer), SC_OK);
    failures += sc_test_expect_true("timer inactive", !sc_runtime_timer_active(timer));
    sc_runtime_timer_destroy(timer);
    timer = nullptr;
    failures += sc_test_expect_status("loop add task",
                              sc_runtime_loop_add_task(loop,
                                                       &(sc_runtime_loop_task_options){
                                                           .struct_size = sizeof(sc_runtime_loop_task_options),
                                                           .name = sc_str_from_cstr("count"),
                                                           .run = loop_count_task,
                                                           .user_data = &counter,
                                                           .repeat = true,
                                                           .run_immediately = true,
                                                       }),
                              SC_OK);
    failures += sc_test_expect_status("loop run", sc_runtime_loop_run(loop), SC_OK);
    failures += sc_test_expect_true("loop max iterations", counter.calls == 3);
    failures += sc_test_expect_status("shutdown phases",
                              sc_runtime_loop_shutdown(loop,
                                                       &(sc_runtime_shutdown_options){
                                                           .struct_size = sizeof(sc_runtime_shutdown_options),
                                                           .drain_receipts = shutdown_drain,
                                                           .flush_observers = shutdown_flush,
                                                           .close_transports = shutdown_close,
                                                           .destroy_subsystems = shutdown_destroy,
                                                           .user_data = &shutdown,
                                                       }),
                              SC_OK);
    failures += sc_test_expect_true("shutdown ordered phases",
                            shutdown.drain == 1 && shutdown.flush == 2 && shutdown.close == 3 && shutdown.destroy == 4);
    failures += sc_test_expect_status("shutdown idempotent",
                              sc_runtime_loop_shutdown(loop,
                                                       &(sc_runtime_shutdown_options){
                                                           .struct_size = sizeof(sc_runtime_shutdown_options),
                                                           .drain_receipts = shutdown_drain,
                                                           .flush_observers = shutdown_flush,
                                                           .close_transports = shutdown_close,
                                                           .destroy_subsystems = shutdown_destroy,
                                                           .user_data = &shutdown,
                                                       }),
                              SC_OK);
    failures += sc_test_expect_true("shutdown phases not repeated",
                            shutdown.drain == 1 && shutdown.flush == 2 && shutdown.close == 3 && shutdown.destroy == 4);
    sc_runtime_loop_destroy(loop);
    loop = nullptr;

    shutdown = (shutdown_record){0};
    failures += sc_test_expect_status("hard loop new",
                              sc_runtime_loop_new(sc_allocator_heap(),
                                                  &(sc_runtime_loop_options){
                                                      .struct_size = sizeof(sc_runtime_loop_options),
                                                      .hard_shutdown = true,
                                                  },
                                                  &loop),
                              SC_OK);
    failures += sc_test_expect_status("hard shutdown phases",
                              sc_runtime_loop_shutdown(loop,
                                                       &(sc_runtime_shutdown_options){
                                                           .struct_size = sizeof(sc_runtime_shutdown_options),
                                                           .drain_receipts = shutdown_drain,
                                                           .flush_observers = shutdown_flush,
                                                           .close_transports = shutdown_close,
                                                           .destroy_subsystems = shutdown_destroy,
                                                           .user_data = &shutdown,
                                                           .hard = true,
                                                       }),
                              SC_OK);
    failures += sc_test_expect_true("hard shutdown skips drain",
                            shutdown.drain == 0 && shutdown.flush == 0 && shutdown.close == 1 && shutdown.destroy == 2);
    sc_runtime_loop_destroy(loop);
    loop = nullptr;

    failures += sc_test_expect_status("token init", sc_cancel_token_init(&token, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("token cancel",
                              sc_cancel_token_cancel(&token, sc_allocator_heap(), sc_str_from_cstr("manual")),
                              SC_OK);
    failures += sc_test_expect_true("token state", token.cancel_requested && strcmp(token.reason.ptr, "manual") == 0);
    sc_cancel_token_clear(&token);

    failures += sc_test_expect_status("switch provider", scripted_provider_new(sc_allocator_heap(), model_steps, 1, &provider), SC_OK);
    failures += sc_test_expect_status("switch agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .model = sc_str_from_cstr("model-a"),
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("switch process", sc_agent_process_message(agent, &turn, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("switch output", strcmp(result.output.ptr, "switched") == 0);
    failures += sc_test_expect_true("switch result flag", result.model_switched && strcmp(result.active_model.ptr, "model-b") == 0);

    sc_agent_turn_result_clear(&result);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);

    provider = nullptr;
    agent = nullptr;
    failures += sc_test_expect_status("queue loop",
                              sc_runtime_loop_new(sc_allocator_heap(),
                                                  &(sc_runtime_loop_options){
                                                      .struct_size = sizeof(sc_runtime_loop_options),
                                                      .max_iterations = 2,
                                                  },
                                                  &loop),
                              SC_OK);
    failures += sc_test_expect_status("queue provider", scripted_provider_new(sc_allocator_heap(), queue_steps, 2, &provider), SC_OK);
    failures += sc_test_expect_status("queue agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("queue new", sc_runtime_turn_queue_new(sc_allocator_heap(), loop, agent, &turn_queue), SC_OK);
    failures += sc_test_expect_status("queue first",
                              sc_runtime_turn_queue_enqueue(turn_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .source = SC_RUNTIME_AGENT_JOB_CHANNEL,
                                                                .input = sc_str_from_cstr("old"),
                                                                .session_id = sc_str_from_cstr("session"),
                                                                .thread_id = sc_str_from_cstr("thread"),
                                                                .done = queue_done,
                                                                .user_data = &queue,
                                                            }),
                              SC_OK);
    failures += sc_test_expect_status("queue cancel previous",
                              sc_runtime_turn_queue_enqueue(turn_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .source = SC_RUNTIME_AGENT_JOB_WEBSOCKET,
                                                                .input = sc_str_from_cstr("new"),
                                                                .session_id = sc_str_from_cstr("session"),
                                                                .thread_id = sc_str_from_cstr("thread"),
                                                                .cancel_previous = true,
                                                                .done = queue_done,
                                                                .user_data = &queue,
                                                            }),
                              SC_OK);
    failures += sc_test_expect_true("queue len", sc_runtime_turn_queue_len(turn_queue) == 2);
    failures += sc_test_expect_status("queue schedule", sc_runtime_turn_queue_schedule(turn_queue, 0), SC_OK);
    failures += sc_test_expect_status("queue run", sc_runtime_loop_run(loop), SC_OK);
    failures += sc_test_expect_true("queue drained", sc_runtime_turn_queue_len(turn_queue) == 0);
    failures += sc_test_expect_true("queue callbacks", queue.calls == 2 && queue.cancelled == 1 && queue.last_status == SC_OK);
    failures += sc_test_expect_status("queue rejects after shutdown",
                              sc_runtime_turn_queue_enqueue(turn_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .source = SC_RUNTIME_AGENT_JOB_GATEWAY,
                                                                .input = sc_str_from_cstr("late"),
                                                            }),
                              SC_ERR_CANCELLED);

    sc_runtime_turn_queue_destroy(turn_queue);
    sc_runtime_loop_destroy(loop);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    return failures;
}

static int test_runtime_turn_queue_consecutive_messages(void)
{
    int failures = 0;
    pending_provider *pending_state = nullptr;
    sc_provider *pending = nullptr;
    sc_provider *trim_provider = nullptr;
    sc_provider *limit_provider = nullptr;
    sc_provider *unrelated_provider = nullptr;
    sc_agent *agent = nullptr;
    sc_agent *trim_agent = nullptr;
    sc_agent *limit_agent = nullptr;
    sc_agent *unrelated_agent = nullptr;
    sc_runtime_loop *loop = nullptr;
    sc_runtime_turn_queue *turn_queue = nullptr;
    sc_runtime_turn_queue *trim_queue = nullptr;
    sc_runtime_turn_queue *limit_queue = nullptr;
    sc_runtime_turn_queue *unrelated_queue = nullptr;
    queue_record queue = {0};
    queue_record trim_record = {0};
    queue_record limit_record = {0};
    queue_record unrelated_record = {0};
    const scripted_step trim_steps[] = {
        {.text = "trimmed", .required_prompt_substring = "1. b\n2. c"},
    };
    const scripted_step limit_steps[] = {
        {.text = "single", .required_prompt_substring = "User message\nnewest"},
    };
    const scripted_step unrelated_steps[] = {
        {.text = "old-ok", .required_prompt_substring = "User message\nold"},
        {.text = "new-ok", .required_prompt_substring = "User message\nnew"},
    };

    failures += sc_test_expect_status("consecutive loop",
                              sc_runtime_loop_new(sc_allocator_heap(),
                                                  &(sc_runtime_loop_options){
                                                      .struct_size = sizeof(sc_runtime_loop_options),
                                                      .max_iterations = 1,
                                                  },
                                                  &loop),
                              SC_OK);
    failures += sc_test_expect_status("pending provider", pending_provider_new(sc_allocator_heap(), &pending_state, &pending), SC_OK);
    failures += sc_test_expect_status("pending agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = pending,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("pending queue", sc_runtime_turn_queue_new(sc_allocator_heap(), loop, agent, &turn_queue), SC_OK);
    failures += sc_test_expect_status("enqueue active",
                              sc_runtime_turn_queue_enqueue(turn_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .source = SC_RUNTIME_AGENT_JOB_CHANNEL,
                                                                .input = sc_str_from_cstr("first"),
                                                                .session_id = sc_str_from_cstr("s"),
                                                                .thread_id = sc_str_from_cstr("t"),
                                                                .done = queue_done,
                                                                .user_data = &queue,
                                                            }),
                              SC_OK);
    failures += sc_test_expect_status("start active", runtime_turn_queue_task(turn_queue, nullptr, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_true("provider pending", pending_state != nullptr && pending_state->pending && pending_state->calls == 1);
    failures += sc_test_expect_status("enqueue replacement",
                              sc_runtime_turn_queue_enqueue(turn_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .source = SC_RUNTIME_AGENT_JOB_CHANNEL,
                                                                .input = sc_str_from_cstr("second"),
                                                                .session_id = sc_str_from_cstr("s"),
                                                                .thread_id = sc_str_from_cstr("t"),
                                                                .cancel_previous = true,
                                                                .done = queue_done,
                                                                .user_data = &queue,
                                                            }),
                              SC_OK);
    failures += sc_test_expect_status("complete cancelled active",
                              pending_provider_complete(pending_state, sc_str_from_cstr("first-output")),
                              SC_OK);
    failures += sc_test_expect_true("active cancelled no history",
                            queue.calls == 1 && queue.cancelled == 1 && sc_agent_history_len(agent) == 0);
    failures += sc_test_expect_status("start replacement", runtime_turn_queue_task(turn_queue, nullptr, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_true("replacement batched",
                            pending_state != nullptr && pending_state->calls == 2 &&
                                strstr(pending_state->last_prompt.ptr, "Consecutive user messages received") != nullptr &&
                                strstr(pending_state->last_prompt.ptr, "1. first") != nullptr &&
                                strstr(pending_state->last_prompt.ptr, "2. second") != nullptr);
    failures += sc_test_expect_status("complete replacement", pending_provider_complete(pending_state, sc_str_from_cstr("combined")), SC_OK);
    failures += sc_test_expect_true("replacement history", queue.calls == 2 && queue.cancelled == 1 && sc_agent_history_len(agent) == 2);

    failures += sc_test_expect_status("trim provider", scripted_provider_new(sc_allocator_heap(), trim_steps, SC_ARRAY_LEN(trim_steps), &trim_provider), SC_OK);
    failures += sc_test_expect_status("trim agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = trim_provider,
                                           },
                                           &trim_agent),
                              SC_OK);
    failures += sc_test_expect_status("trim queue", sc_runtime_turn_queue_new(sc_allocator_heap(), loop, trim_agent, &trim_queue), SC_OK);
    failures += sc_test_expect_status("trim a",
                              sc_runtime_turn_queue_enqueue(trim_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .input = sc_str_from_cstr("a"),
                                                                .session_id = sc_str_from_cstr("s"),
                                                                .thread_id = sc_str_from_cstr("t"),
                                                                .done = queue_done,
                                                                .user_data = &trim_record,
                                                            }),
                              SC_OK);
    failures += sc_test_expect_status("trim b",
                              sc_runtime_turn_queue_enqueue(trim_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .input = sc_str_from_cstr("b"),
                                                                .session_id = sc_str_from_cstr("s"),
                                                                .thread_id = sc_str_from_cstr("t"),
                                                                .cancel_previous = true,
                                                                .consecutive_message_limit = 2,
                                                                .done = queue_done,
                                                                .user_data = &trim_record,
                                                            }),
                              SC_OK);
    failures += sc_test_expect_status("trim c",
                              sc_runtime_turn_queue_enqueue(trim_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .input = sc_str_from_cstr("c"),
                                                                .session_id = sc_str_from_cstr("s"),
                                                                .thread_id = sc_str_from_cstr("t"),
                                                                .cancel_previous = true,
                                                                .consecutive_message_limit = 2,
                                                                .done = queue_done,
                                                                .user_data = &trim_record,
                                                            }),
                              SC_OK);
    failures += sc_test_expect_status("trim drain", sc_runtime_turn_queue_drain(trim_queue, 0, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_true("trim callbacks", trim_record.calls == 3 && trim_record.cancelled == 2 && sc_agent_history_len(trim_agent) == 2);

    failures += sc_test_expect_status("limit provider", scripted_provider_new(sc_allocator_heap(), limit_steps, SC_ARRAY_LEN(limit_steps), &limit_provider), SC_OK);
    failures += sc_test_expect_status("limit agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = limit_provider,
                                           },
                                           &limit_agent),
                              SC_OK);
    failures += sc_test_expect_status("limit queue", sc_runtime_turn_queue_new(sc_allocator_heap(), loop, limit_agent, &limit_queue), SC_OK);
    failures += sc_test_expect_status("limit older",
                              sc_runtime_turn_queue_enqueue(limit_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .input = sc_str_from_cstr("older"),
                                                                .session_id = sc_str_from_cstr("s"),
                                                                .thread_id = sc_str_from_cstr("t"),
                                                                .done = queue_done,
                                                                .user_data = &limit_record,
                                                            }),
                              SC_OK);
    failures += sc_test_expect_status("limit newest",
                              sc_runtime_turn_queue_enqueue(limit_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .input = sc_str_from_cstr("newest"),
                                                                .session_id = sc_str_from_cstr("s"),
                                                                .thread_id = sc_str_from_cstr("t"),
                                                                .cancel_previous = true,
                                                                .consecutive_message_limit = 1,
                                                                .done = queue_done,
                                                                .user_data = &limit_record,
                                                            }),
                              SC_OK);
    failures += sc_test_expect_status("limit drain", sc_runtime_turn_queue_drain(limit_queue, 0, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_true("limit callbacks", limit_record.calls == 2 && limit_record.cancelled == 1 && sc_agent_history_len(limit_agent) == 2);

    failures += sc_test_expect_status("unrelated provider",
                              scripted_provider_new(sc_allocator_heap(), unrelated_steps, SC_ARRAY_LEN(unrelated_steps), &unrelated_provider),
                              SC_OK);
    failures += sc_test_expect_status("unrelated agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = unrelated_provider,
                                           },
                                           &unrelated_agent),
                              SC_OK);
    failures += sc_test_expect_status("unrelated queue", sc_runtime_turn_queue_new(sc_allocator_heap(), loop, unrelated_agent, &unrelated_queue), SC_OK);
    failures += sc_test_expect_status("unrelated old",
                              sc_runtime_turn_queue_enqueue(unrelated_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .input = sc_str_from_cstr("old"),
                                                                .session_id = sc_str_from_cstr("s1"),
                                                                .thread_id = sc_str_from_cstr("t"),
                                                                .done = queue_done,
                                                                .user_data = &unrelated_record,
                                                            }),
                              SC_OK);
    failures += sc_test_expect_status("unrelated new",
                              sc_runtime_turn_queue_enqueue(unrelated_queue,
                                                            &(sc_runtime_agent_job){
                                                                .struct_size = sizeof(sc_runtime_agent_job),
                                                                .input = sc_str_from_cstr("new"),
                                                                .session_id = sc_str_from_cstr("s2"),
                                                                .thread_id = sc_str_from_cstr("t"),
                                                                .cancel_previous = true,
                                                                .done = queue_done,
                                                                .user_data = &unrelated_record,
                                                            }),
                              SC_OK);
    failures += sc_test_expect_status("unrelated drain", sc_runtime_turn_queue_drain(unrelated_queue, 0, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_true("unrelated callbacks",
                            unrelated_record.calls == 2 && unrelated_record.cancelled == 0 && sc_agent_history_len(unrelated_agent) == 4);

    sc_runtime_turn_queue_destroy(unrelated_queue);
    sc_runtime_turn_queue_destroy(limit_queue);
    sc_runtime_turn_queue_destroy(trim_queue);
    sc_runtime_turn_queue_destroy(turn_queue);
    sc_runtime_loop_destroy(loop);
    sc_agent_destroy(unrelated_agent);
    sc_agent_destroy(limit_agent);
    sc_agent_destroy(trim_agent);
    sc_agent_destroy(agent);
    sc_provider_destroy(unrelated_provider);
    sc_provider_destroy(limit_provider);
    sc_provider_destroy(trim_provider);
    sc_provider_destroy(pending);
    return failures;
}

static int test_agent_process_message_async(void)
{
    int failures = 0;
    const scripted_step steps[] = {
        {.text = "", .tool_name = {"mock_tool"}, .tool_args = {"{\"value\":\"async-tool\"}"}},
        {.text = "async-done", .required_prompt_substring = "async-tool"},
    };
    const scripted_step overflow_steps[] = {
        {.text = "async-seed"},
        {.text = nullptr, .error_code = SC_ERR_HTTP, .error_key = "sc.provider.context_length_exceeded"},
        {.text = "async-trimmed",
         .required_prompt_substring = "User message\nnew async",
         .forbidden_prompt_substring = "async-ancient-context"},
    };
    const char *async_old =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
        " async-ancient-context";
    sc_provider *provider = nullptr;
    sc_tool *tool = nullptr;
    sc_agent *agent = nullptr;
    sc_async_context *context = nullptr;
    async_turn_record record = {0};
    sc_agent_turn_result seed_result = {0};
    sc_turn turn = {
        .struct_size = sizeof(turn),
        .input = sc_str_from_cstr("run async"),
    };
    sc_turn old_turn = {
        .struct_size = sizeof(old_turn),
        .input = sc_str_from_cstr(async_old),
        .session_id = sc_str_from_cstr("async-overflow"),
    };
    sc_turn new_turn = {
        .struct_size = sizeof(new_turn),
        .input = sc_str_from_cstr("new async"),
        .session_id = sc_str_from_cstr("async-overflow"),
    };

    failures += sc_test_expect_status("async provider", scripted_provider_new(sc_allocator_heap(), steps, SC_ARRAY_LEN(steps), &provider), SC_OK);
    failures += sc_test_expect_status("async tool", echo_tool_new(sc_allocator_heap(), "mock_tool", false, &tool), SC_OK);
    failures += sc_test_expect_status("async agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .tools = &tool,
                                               .tool_count = 1,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("async context", sc_async_context_new(nullptr, &context), SC_OK);
    failures += sc_test_expect_status("async process",
                              sc_agent_process_message_async(agent,
                                                             context,
                                                             &turn,
                                                             sc_allocator_heap(),
                                                             async_turn_done,
                                                             &record,
                                                             nullptr),
                              SC_OK);
    failures += sc_test_expect_true("async callback", record.calls == 1 && record.status == SC_OK);
    failures += sc_test_expect_true("async output", strcmp(record.output.ptr, "async-done") == 0);
    failures += sc_test_expect_true("async counts", record.provider_calls == 2 && record.tool_calls == 1);
    failures += sc_test_expect_true("async history", sc_agent_history_len(agent) == 2);

    async_turn_record_clear(&record);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    agent = nullptr;
    provider = nullptr;

    failures += sc_test_expect_status("async overflow provider",
                              scripted_provider_new(sc_allocator_heap(), overflow_steps, SC_ARRAY_LEN(overflow_steps), &provider),
                              SC_OK);
    failures += sc_test_expect_status("async overflow agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){
                                               .struct_size = sizeof(sc_agent_options),
                                               .provider = provider,
                                               .max_prompt_bytes = 4096,
                                           },
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("async overflow seed",
                              sc_agent_process_message(agent, &old_turn, sc_allocator_heap(), &seed_result),
                              SC_OK);
    sc_agent_turn_result_clear(&seed_result);
    failures += sc_test_expect_status("async overflow process",
                              sc_agent_process_message_async(agent,
                                                             context,
                                                             &new_turn,
                                                             sc_allocator_heap(),
                                                             async_turn_done,
                                                             &record,
                                                             nullptr),
                              SC_OK);
    failures += sc_test_expect_true("async overflow callback", record.calls == 1 && record.status == SC_OK);
    failures += sc_test_expect_true("async overflow output", strcmp(record.output.ptr, "async-trimmed") == 0);
    failures += sc_test_expect_true("async overflow retry count", record.provider_calls == 2);

    async_turn_record_clear(&record);
    sc_async_context_destroy(context);
    sc_agent_destroy(agent);
    sc_tool_destroy(tool);
    sc_provider_destroy(provider);
    return failures;
}

static int test_runtime_facade_schema_and_streaming(void)
{
    int failures = 0;
    const scripted_step schema_steps[] = {
        {.text = "bad args", .tool_name = {"mock_tool", nullptr}, .tool_args = {"{}", nullptr}},
        {.text = "schema handled", .required_prompt_substring = "sc.agent.tool_schema.required_missing"},
    };
    sc_provider *provider = nullptr;
    sc_provider *streaming = nullptr;
    sc_provider *malformed = nullptr;
    sc_tool *tool = nullptr;
    sc_tool *tools[1] = {nullptr};
    sc_runtime *runtime = nullptr;
    sc_runtime_response response = {0};

    failures += sc_test_expect_status("facade provider", scripted_provider_new(sc_allocator_heap(), schema_steps, 2, &provider), SC_OK);
    failures += sc_test_expect_status("facade tool", echo_tool_new(sc_allocator_heap(), "mock_tool", false, &tool), SC_OK);
    tools[0] = tool;
    failures += sc_test_expect_status("runtime create",
                              sc_runtime_create(sc_allocator_heap(),
                                                &(sc_runtime_config){
                                                    .struct_size = sizeof(sc_runtime_config),
                                                    .provider = provider,
                                                    .tools = tools,
                                                    .tool_count = 1,
                                                    .deterministic_prompts = true,
                                                },
                                                &runtime),
                              SC_OK);
    failures += sc_test_expect_status("runtime process",
                              sc_runtime_process_message(runtime,
                                                         &(sc_runtime_message){
                                                             .struct_size = sizeof(sc_runtime_message),
                                                             .input = sc_str_from_cstr("run"),
                                                             .session_id = sc_str_from_cstr("s"),
                                                         },
                                                         sc_allocator_heap(),
                                                         &response),
                              SC_OK);
    failures += sc_test_expect_true("runtime response", response.turn_id == 1 && strstr(response.output.ptr, "schema handled") != nullptr);
    failures += sc_test_expect_true("schema receipt", response.receipts.receipts.len == 1);
    sc_runtime_response_clear(&response);
    sc_runtime_destroy(runtime);
    sc_provider_destroy(provider);
    sc_tool_destroy(tool);
    runtime = nullptr;
    provider = nullptr;
    tool = nullptr;

    failures += sc_test_expect_status("stream provider", stream_provider_new(sc_allocator_heap(), false, &streaming), SC_OK);
    failures += sc_test_expect_status("stream runtime",
                              sc_runtime_create(sc_allocator_heap(),
                                                &(sc_runtime_config){
                                                    .struct_size = sizeof(sc_runtime_config),
                                                    .provider = streaming,
                                                    .use_streaming = true,
                                                    .emit_stream_deltas = true,
                                                },
                                                &runtime),
                              SC_OK);
    failures += sc_test_expect_status("stream process",
                              sc_runtime_process_message(runtime,
                                                         &(sc_runtime_message){
                                                             .struct_size = sizeof(sc_runtime_message),
                                                             .input = sc_str_from_cstr("stream"),
                                                         },
                                                         sc_allocator_heap(),
                                                         &response),
                              SC_OK);
    failures += sc_test_expect_true("stream output", strcmp(response.output.ptr, "streamed") == 0);
    sc_runtime_response_clear(&response);
    sc_runtime_destroy(runtime);
    sc_provider_destroy(streaming);
    runtime = nullptr;

    failures += sc_test_expect_status("malformed stream provider", stream_provider_new(sc_allocator_heap(), true, &malformed), SC_OK);
    failures += sc_test_expect_status("malformed runtime",
                              sc_runtime_create(sc_allocator_heap(),
                                                &(sc_runtime_config){
                                                    .struct_size = sizeof(sc_runtime_config),
                                                    .provider = malformed,
                                                    .use_streaming = true,
                                                },
                                                &runtime),
                              SC_OK);
    failures += sc_test_expect_status("malformed stream process",
                              sc_runtime_process_message(runtime,
                                                         &(sc_runtime_message){
                                                             .struct_size = sizeof(sc_runtime_message),
                                                             .input = sc_str_from_cstr("stream"),
                                                         },
                                                         sc_allocator_heap(),
                                                         &response),
                              SC_ERR_PARSE);
    failures += sc_test_expect_true("malformed stream failed", response.events.len > 0);
    sc_runtime_response_clear(&response);
    sc_runtime_destroy(runtime);
    sc_provider_destroy(malformed);
    return failures;
}

static int test_direct_tool_rules(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_receipt_chain receipts = {0};
    sc_tool_context context = {0};
    sc_tool *file_tool = nullptr;
    sc_tool *file_tool_dup = nullptr;
    sc_tool *wrapped = nullptr;
    sc_tool *shell = nullptr;
    sc_tool *mcp = nullptr;
    sc_memory *memory = nullptr;
    sc_tool *memory_store = nullptr;
    sc_tool *memory_search = nullptr;
    sc_tool_spec spec = {0};
    sc_json_value *args = nullptr;
    sc_tool_result result = {0};
    sc_string path = {0};
    sc_tool *dupes[2] = {nullptr, nullptr};
    sc_json_value *model_specs = nullptr;

    failures += sc_test_expect_status("tool policy", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("tool workspace", sc_security_policy_set_workspace(&policy, sc_str_from_cstr("/tmp")), SC_OK);
    sc_receipt_chain_init(&receipts, sc_allocator_heap());
    context = (sc_tool_context){
        .struct_size = sizeof(context),
        .policy = &policy,
        .receipts = &receipts,
        .max_output_bytes = 4,
        .max_arg_bytes = 128,
    };

    failures += sc_test_expect_status("tool temp path", make_temp_path("tool-rules.txt", &path), SC_OK);
    failures += sc_test_expect_true("tool write fixture", write_file(sc_string_as_str(&path), "secret-data") == 0);
    failures += sc_test_expect_status("file tool direct", sc_tool_file_read_new(sc_allocator_heap(), &context, &file_tool), SC_OK);
    failures += sc_test_expect_status("file tool dup", sc_tool_file_read_new(sc_allocator_heap(), &context, &file_tool_dup), SC_OK);
    failures += sc_test_expect_status("file spec", sc_tool_spec_get(file_tool, &spec), SC_OK);
    failures += sc_test_expect_true("file descriptor metadata",
                            spec.output_schema != nullptr && spec.capability_category == SC_TOOL_CAPABILITY_FILESYSTEM &&
                                spec.side_effect == SC_TOOL_SIDE_EFFECT_READ && spec.default_autonomy == SC_AUTONOMY_AUTONOMOUS);

    dupes[0] = file_tool;
    dupes[1] = file_tool_dup;
    failures += sc_test_expect_status("duplicate tool registry",
                              sc_tool_registry_model_specs_from_tools(dupes, 2, sc_allocator_heap(), &model_specs),
                              SC_ERR_INVALID_ARGUMENT);

    failures += sc_test_expect_status("missing path json", parse_json_object(sc_str_from_cstr("{}"), &args), SC_OK);
    failures += sc_test_expect_status("schema rejects missing path",
                              sc_tool_invoke(file_tool,
                                             &(sc_tool_call){.struct_size = sizeof(sc_tool_call), .args = args},
                                             sc_allocator_heap(),
                                             &result),
                              SC_ERR_INVALID_ARGUMENT);
    sc_json_destroy(args);
    args = nullptr;

    {
        char body[512] = {0};
        int written = snprintf(body, sizeof(body), "{\"path\":\"%s\"}", path.ptr);
        failures += sc_test_expect_true("path json format", written > 0 && (size_t)written < sizeof(body));
        failures += sc_test_expect_status("path json", parse_json_object(sc_str_from_cstr(body), &args), SC_OK);
    }
    failures += sc_test_expect_status("file invoke",
                              sc_tool_invoke(file_tool,
                                             &(sc_tool_call){.struct_size = sizeof(sc_tool_call), .args = args},
                                             sc_allocator_heap(),
                                             &result),
                              SC_OK);
    failures += sc_test_expect_true("file output bounded", result.output.len == 4 && memcmp(result.output.ptr, "secr", 4) == 0);
    failures += sc_test_expect_true("receipt redacted",
                            receipts.receipts.len == 1 &&
                                strstr(((sc_tool_receipt *)sc_vec_at(&receipts.receipts, 0))->args_summary.ptr, path.ptr) == nullptr &&
                                strstr(((sc_tool_receipt *)sc_vec_at(&receipts.receipts, 0))->output_summary.ptr, "secr") == nullptr);
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("timeout wrapper", sc_tool_timeout_wrapper_new(sc_allocator_heap(), file_tool_dup, 0, &wrapped), SC_OK);
    file_tool_dup = nullptr;
    failures += sc_test_expect_status("timeout path json", parse_json_object(sc_str_from_cstr("{\"path\":\"/tmp/nope\"}"), &args), SC_OK);
    failures += sc_test_expect_status("timeout wrapper invoke",
                              sc_tool_invoke(wrapped,
                                             &(sc_tool_call){.struct_size = sizeof(sc_tool_call), .args = args},
                                             sc_allocator_heap(),
                                             &result),
                              SC_ERR_TIMEOUT);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("shell tool", sc_tool_shell_new(sc_allocator_heap(), &context, &shell), SC_OK);
    failures += sc_test_expect_status("shell spec", sc_tool_spec_get(shell, &spec), SC_OK);
    failures += sc_test_expect_true("shell descriptor metadata",
                            spec.capability_category == SC_TOOL_CAPABILITY_PROCESS &&
                                spec.side_effect == SC_TOOL_SIDE_EFFECT_PROCESS && spec.default_autonomy == SC_AUTONOMY_SUPERVISED);
    failures += sc_test_expect_status("shell json", parse_json_object(sc_str_from_cstr("{\"command\":\"echo hi\"}"), &args), SC_OK);
    failures += sc_test_expect_status("shell denied",
                              sc_tool_invoke(shell,
                                             &(sc_tool_call){.struct_size = sizeof(sc_tool_call), .args = args},
                                             sc_allocator_heap(),
                                             &result),
                              SC_ERR_SECURITY_DENIED);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("mcp tool",
                              sc_tool_mcp_server_new(sc_allocator_heap(),
                                                     &context,
                                                     sc_str_from_cstr("local"),
                                                     sc_str_from_cstr("http"),
                                                     sc_str_from_cstr(""),
                                                     sc_str_from_cstr(""),
                                                     sc_str_from_cstr("http://127.0.0.1:1/rpc"),
                                                     sc_str_from_cstr("[\"Authorization: Bearer secret\"]"),
                                                     &mcp),
                              SC_OK);
    failures += sc_test_expect_status("mcp json",
                              parse_json_object(sc_str_from_cstr("{\"tool\":\"read\",\"arguments_json\":\"{}\"}"), &args),
                              SC_OK);
    failures += sc_test_expect_status("mcp ssrf denied",
                              sc_tool_invoke(mcp,
                                             &(sc_tool_call){.struct_size = sizeof(sc_tool_call), .args = args},
                                             sc_allocator_heap(),
                                             &result),
                              SC_ERR_SECURITY_DENIED);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("memory none direct", sc_memory_none_new(sc_allocator_heap(), &memory), SC_OK);
    context.memory = memory;
    failures += sc_test_expect_status("memory store direct", sc_tool_memory_store_new(sc_allocator_heap(), &context, &memory_store), SC_OK);
    failures += sc_test_expect_status("memory spec", sc_tool_spec_get(memory_store, &spec), SC_OK);
    failures += sc_test_expect_true("memory descriptor metadata",
                            spec.capability_category == SC_TOOL_CAPABILITY_MEMORY &&
                                spec.side_effect == SC_TOOL_SIDE_EFFECT_WRITE && spec.output_schema != nullptr);
    failures += sc_test_expect_status("memory auto approve", sc_security_policy_add_auto_approved_tool(&policy, sc_str_from_cstr("memory_store")), SC_OK);
    failures += sc_test_expect_status("memory disabled json",
                              parse_json_object(sc_str_from_cstr("{\"namespace\":\"n\",\"key\":\"k\",\"content\":\"v\"}"), &args),
                              SC_OK);
    failures += sc_test_expect_status("memory disabled store",
                              sc_tool_invoke(memory_store,
                                             &(sc_tool_call){.struct_size = sizeof(sc_tool_call), .args = args},
                                             sc_allocator_heap(),
                                             &result),
                              SC_ERR_UNSUPPORTED);
    sc_json_destroy(args);
    args = nullptr;
    failures += sc_test_expect_status("memory search direct", sc_tool_memory_search_new(sc_allocator_heap(), &context, &memory_search), SC_OK);
    failures += sc_test_expect_status("memory search spec", sc_tool_spec_get(memory_search, &spec), SC_OK);
    failures += sc_test_expect_true("memory search descriptor metadata",
                            spec.capability_category == SC_TOOL_CAPABILITY_MEMORY &&
                                spec.side_effect == SC_TOOL_SIDE_EFFECT_READ &&
                                spec.default_autonomy == SC_AUTONOMY_AUTONOMOUS);

    sc_json_destroy(model_specs);
    sc_json_destroy(args);
    sc_tool_result_clear(&result);
    sc_tool_destroy(memory_search);
    sc_tool_destroy(memory_store);
    sc_memory_destroy(memory);
    sc_tool_destroy(mcp);
    sc_tool_destroy(shell);
    sc_tool_destroy(wrapped);
    sc_tool_destroy(file_tool_dup);
    sc_tool_destroy(file_tool);
    sc_string_clear(&path);
    sc_receipt_chain_clear(&receipts);
    sc_security_policy_clear(&policy);
    return failures;
}

static sc_status scripted_provider_new(sc_allocator *alloc,
                                       const scripted_step *steps,
                                       size_t step_count,
                                       sc_provider **out)
{
    scripted_provider *provider = nullptr;
    if (out == nullptr || steps == nullptr || step_count == 0) {
        return sc_status_invalid_argument("sc.test_provider.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    provider = sc_alloc(alloc, sizeof(*provider), _Alignof(scripted_provider));
    if (provider == nullptr) {
        return sc_status_no_memory();
    }
    *provider = (scripted_provider){.alloc = alloc, .steps = steps, .step_count = step_count};
    sc_status status = sc_provider_new(alloc, &scripted_vtab, provider, out);
    if (!sc_status_is_ok(status)) {
        scripted_destroy(provider);
    }
    return status;
}

static sc_status scripted_generate(void *impl,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_provider_response *out)
{
    scripted_provider *provider = impl;
    const scripted_step *step = nullptr;
    sc_status status;

    if (provider == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.test_provider.invalid_argument");
    }
    if (provider->calls >= provider->step_count) {
        return sc_status_cancelled("sc.test_provider.exhausted");
    }
    step = &provider->steps[provider->calls++];
    if (step->required_prompt_substring != nullptr &&
        (request->prompt.ptr == nullptr || strstr(request->prompt.ptr, step->required_prompt_substring) == nullptr)) {
        return sc_status_invalid_argument("sc.test_provider.prompt_missing");
    }
    if (step->forbidden_prompt_substring != nullptr &&
        request->prompt.ptr != nullptr &&
        strstr(request->prompt.ptr, step->forbidden_prompt_substring) != nullptr) {
        return sc_status_invalid_argument("sc.test_provider.prompt_forbidden");
    }
    if (step->required_model != nullptr &&
        (request->model.ptr == nullptr || strcmp(request->model.ptr, step->required_model) != 0)) {
        return sc_status_invalid_argument("sc.test_provider.model_missing");
    }
    if (step->error_code == SC_ERR_HTTP) {
        return sc_status_http(step->error_key == nullptr ? "sc.test_provider.http" : step->error_key);
    }

    sc_provider_response_init(out, alloc);
    status = sc_string_from_cstr(alloc, step->text == nullptr ? "" : step->text, &out->text);
    out->input_tokens = step->input_tokens;
    out->output_tokens = step->output_tokens;
    out->total_tokens = step->input_tokens + step->output_tokens;
    out->cost_usd = step->cost_usd;
    for (size_t i = 0; sc_status_is_ok(status) && i < 2; i += 1) {
        char call_id[16] = {0};
        if (step->tool_name[i] == nullptr) {
            continue;
        }
        (void)snprintf(call_id, sizeof(call_id), "call-%zu", i + 1);
        status = add_tool_call(out,
                               sc_str_from_cstr(call_id),
                               sc_str_from_cstr(step->tool_name[i]),
                               sc_str_from_cstr(step->tool_args[i] == nullptr ? "{}" : step->tool_args[i]));
    }
    if (!sc_status_is_ok(status)) {
        sc_provider_response_clear(out);
    }
    return status;
}

static void scripted_destroy(void *impl)
{
    scripted_provider *provider = impl;
    if (provider == nullptr) {
        return;
    }
    sc_free(provider->alloc, provider, sizeof(*provider), _Alignof(scripted_provider));
}

static sc_status stream_provider_new(sc_allocator *alloc, bool omit_done, sc_provider **out)
{
    stream_provider *provider = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.test_stream.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    provider = sc_alloc(alloc, sizeof(*provider), _Alignof(stream_provider));
    if (provider == nullptr) {
        return sc_status_no_memory();
    }
    *provider = (stream_provider){.alloc = alloc, .omit_done = omit_done};
    status = sc_provider_new(alloc, &stream_vtab, provider, out);
    if (!sc_status_is_ok(status)) {
        stream_destroy(provider);
    }
    return status;
}

static sc_status stream_generate(void *impl,
                                 const sc_provider_request *request,
                                 sc_allocator *alloc,
                                 sc_provider_response *out)
{
    (void)impl;
    (void)request;
    sc_provider_response_init(out, alloc);
    return sc_string_from_cstr(alloc, "generated", &out->text);
}

static sc_status stream_run(void *impl,
                            const sc_provider_request *request,
                            sc_allocator *alloc,
                            sc_provider_stream_callback callback,
                            void *callback_user_data)
{
    const stream_provider *provider = impl;
    sc_provider_stream_event event = {0};
    sc_status status;

    (void)request;
    if (provider == nullptr || callback == nullptr) {
        return sc_status_invalid_argument("sc.test_stream.invalid_argument");
    }
    event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_DELTA};
    status = sc_string_from_cstr(alloc, "streamed", &event.text);
    if (sc_status_is_ok(status)) {
        status = callback(callback_user_data, &event);
    }
    sc_provider_stream_event_clear(&event);
    if (sc_status_is_ok(status) && !provider->omit_done) {
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_DONE};
        status = callback(callback_user_data, &event);
    }
    return status;
}

static void stream_destroy(void *impl)
{
    stream_provider *provider = impl;
    if (provider == nullptr) {
        return;
    }
    sc_free(provider->alloc, provider, sizeof(*provider), _Alignof(stream_provider));
}

static sc_status pending_provider_new(sc_allocator *alloc, pending_provider **state_out, sc_provider **out)
{
    pending_provider *provider = nullptr;
    sc_status status;

    if (state_out == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.test_pending.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    provider = sc_alloc(alloc, sizeof(*provider), _Alignof(pending_provider));
    if (provider == nullptr) {
        return sc_status_no_memory();
    }
    *provider = (pending_provider){.alloc = alloc};
    status = sc_provider_new(alloc, &pending_vtab, provider, out);
    if (!sc_status_is_ok(status)) {
        pending_destroy(provider);
        return status;
    }
    *state_out = provider;
    return sc_status_ok();
}

static sc_status pending_generate(void *impl,
                                  const sc_provider_request *request,
                                  sc_allocator *alloc,
                                  sc_provider_response *out)
{
    (void)impl;
    (void)request;
    sc_provider_response_init(out, alloc);
    return sc_string_from_cstr(alloc, "pending-sync", &out->text);
}

static sc_status pending_generate_async(void *impl,
                                        sc_async_context *context,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_generate_complete_fn complete,
                                        void *complete_user_data,
                                        sc_async_op **out)
{
    pending_provider *provider = impl;

    (void)context;
    (void)alloc;
    if (provider == nullptr || request == nullptr || complete == nullptr) {
        return sc_status_invalid_argument("sc.test_pending.invalid_argument");
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    provider->calls += 1;
    provider->complete = complete;
    provider->complete_user_data = complete_user_data;
    provider->pending = true;
    sc_string_clear(&provider->last_prompt);
    return sc_string_from_str(provider->alloc, request->prompt, &provider->last_prompt);
}

static sc_status pending_provider_complete(pending_provider *provider, sc_str text)
{
    sc_provider_response response = {0};
    sc_status status;
    sc_provider_generate_complete_fn complete = nullptr;
    void *complete_user_data = nullptr;

    if (provider == nullptr || !provider->pending || provider->complete == nullptr) {
        return sc_status_invalid_argument("sc.test_pending.complete_invalid_argument");
    }
    complete = provider->complete;
    complete_user_data = provider->complete_user_data;
    provider->complete = nullptr;
    provider->complete_user_data = nullptr;
    provider->pending = false;

    sc_provider_response_init(&response, provider->alloc);
    status = sc_string_from_str(provider->alloc, text, &response.text);
    complete(complete_user_data, sc_status_is_ok(status) ? &response : nullptr, status);
    sc_provider_response_clear(&response);
    return sc_status_ok();
}

static void pending_destroy(void *impl)
{
    pending_provider *provider = impl;
    if (provider == nullptr) {
        return;
    }
    sc_string_clear(&provider->last_prompt);
    sc_free(provider->alloc, provider, sizeof(*provider), _Alignof(pending_provider));
}

static sc_status add_tool_call(sc_provider_response *response,
                               sc_str call_id,
                               sc_str name,
                               sc_str args_json)
{
    sc_provider_tool_call call = {.struct_size = sizeof(call)};
    sc_json_parse_error error = {0};
    sc_status status = sc_string_from_str(response->tool_calls.alloc, call_id, &call.call_id);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(response->tool_calls.alloc, name, &call.name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(response->tool_calls.alloc, args_json, &call.arguments_json);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_parse(response->tool_calls.alloc, args_json, &call.arguments, &error);
    }
    if (sc_status_is_ok(status)) {
        status = sc_provider_response_add_tool_call(response, &call);
    }
    sc_provider_tool_call_clear(&call);
    return status;
}

static sc_status echo_tool_new(sc_allocator *alloc, const char *name, bool fail, sc_tool **out)
{
    return fail ? echo_tool_new_with_failure_key(alloc, name, "sc.test_tool.failed", out) :
                  echo_tool_new_with_failure_key(alloc, name, nullptr, out);
}

static sc_status echo_tool_new_with_failure_key(sc_allocator *alloc,
                                                const char *name,
                                                const char *failure_key,
                                                sc_tool **out)
{
    echo_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.test_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(echo_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (echo_tool){.alloc = alloc, .failure_key = failure_key, .fail = failure_key != nullptr};
    status = sc_string_from_cstr(alloc, name == nullptr ? "mock_tool" : name, &tool->name);
    if (sc_status_is_ok(status)) {
        status = sc_json_schema_object(alloc, &tool->schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_schema_add_string_property(tool->schema, sc_str_from_cstr("value"), true);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, &echo_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        echo_destroy(tool);
    }
    return status;
}

static sc_status echo_tool_new_with_counter(sc_allocator *alloc,
                                            const char *name,
                                            int *invoke_count,
                                            sc_tool **out)
{
    return echo_tool_new_counting(alloc, name, invoke_count, false, out);
}

static sc_status echo_tool_new_with_counter_and_attachment(sc_allocator *alloc,
                                                           const char *name,
                                                           int *invoke_count,
                                                           sc_tool **out)
{
    return echo_tool_new_counting(alloc, name, invoke_count, true, out);
}

static sc_status echo_tool_new_counting(sc_allocator *alloc,
                                        const char *name,
                                        int *invoke_count,
                                        bool attach,
                                        sc_tool **out)
{
    echo_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.test_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(echo_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (echo_tool){
        .alloc = alloc,
        .invoke_count = invoke_count,
        .attach = attach,
        .attachment_delivery = attach ? SC_ATTACHMENT_DELIVERY_DOCUMENT : SC_ATTACHMENT_DELIVERY_DEFAULT,
    };
    status = sc_string_from_cstr(alloc, name == nullptr ? "mock_tool" : name, &tool->name);
    if (sc_status_is_ok(status)) {
        status = sc_json_schema_object(alloc, &tool->schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_json_schema_add_string_property(tool->schema, sc_str_from_cstr("value"), true);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, &echo_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        echo_destroy(tool);
    }
    return status;
}

static sc_status echo_spec(void *impl, sc_tool_spec *out)
{
    const echo_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.test_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_string_as_str(&tool->name),
        .description = sc_str_from_cstr("tool.mock.description"),
        .input_schema = tool->schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = SC_TOOL_RISK_READONLY,
    };
    return sc_status_ok();
}

static sc_status echo_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    const echo_tool *tool = impl;
    sc_json_value *value_json = nullptr;
    sc_str value = {0};

    if (tool == nullptr || call == nullptr || call->args == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.test_tool.invalid_argument");
    }
    if (tool->invoke_count != nullptr) {
        *tool->invoke_count += 1;
    }
    if (tool->fail) {
        return sc_status_io(tool->failure_key == nullptr ? "sc.test_tool.failed" : tool->failure_key);
    }
    value_json = sc_json_object_get(call->args, sc_str_from_cstr("value"));
    if (!sc_json_as_str(value_json, &value)) {
        return sc_status_invalid_argument("sc.test_tool.missing_value");
    }
    *out = (sc_tool_result){.struct_size = sizeof(*out), .success = true};
    sc_status status = sc_string_from_str(alloc, value, &out->output);
    if (sc_status_is_ok(status) && tool->attach) {
        status = sc_string_from_cstr(alloc, "image/png", &out->attachment_content_type);
    }
    if (sc_status_is_ok(status) && tool->attach) {
        status = sc_string_from_cstr(alloc, "mock.png", &out->attachment_filename);
    }
    if (sc_status_is_ok(status) && tool->attach) {
        status = sc_bytes_from_buf(alloc, sc_buf_from_parts("png", 3), &out->attachment_bytes);
    }
    if (sc_status_is_ok(status) && tool->attach) {
        out->attachment_delivery = tool->attachment_delivery;
    }
    if (!sc_status_is_ok(status)) {
        sc_tool_result_clear(out);
    }
    return status;
}

static void echo_destroy(void *impl)
{
    echo_tool *tool = impl;
    if (tool == nullptr) {
        return;
    }
    sc_string_clear(&tool->name);
    sc_json_destroy(tool->schema);
    sc_free(tool->alloc, tool, sizeof(*tool), _Alignof(echo_tool));
}

static sc_status record_emit(void *impl, const sc_observer_event *event)
{
    record_observer *observer = impl;
    (void)event;
    if (observer == nullptr) {
        return sc_status_invalid_argument("sc.test_observer.invalid_argument");
    }
    observer->calls += 1;
    return sc_status_ok();
}

static sc_status failing_observer_emit(void *impl, const sc_observer_event *event)
{
    (void)impl;
    (void)event;
    return sc_status_io("sc.test_observer.fail");
}

static void failing_observer_destroy(void *impl)
{
    (void)impl;
}

static void record_destroy(void *impl)
{
    record_observer *observer = impl;
    if (observer == nullptr) {
        return;
    }
    sc_free(sc_allocator_heap(), observer, sizeof(*observer), _Alignof(record_observer));
}

static sc_status loop_count_task(void *impl, const sc_cancel_token *cancel, sc_allocator *alloc)
{
    loop_counter *counter = impl;
    (void)alloc;
    if (counter == nullptr) {
        return sc_status_invalid_argument("sc.test_loop.invalid_argument");
    }
    counter->calls += 1;
    counter->saw_cancel = cancel != nullptr && cancel->cancel_requested;
    return sc_status_ok();
}

static void queue_done(void *user_data, const sc_agent_turn_result *result, sc_status status)
{
    queue_record *record = user_data;
    (void)result;
    if (record == nullptr) {
        return;
    }
    record->calls += 1;
    if (status.code == SC_ERR_CANCELLED) {
        record->cancelled += 1;
    }
    record->last_status = status.code;
}

static void async_turn_done(void *user_data, const sc_agent_turn_result *result, sc_status status)
{
    async_turn_record *record = user_data;

    if (record == nullptr) {
        sc_status_clear(&status);
        return;
    }
    record->calls += 1;
    record->status = status.code;
    if (result != nullptr) {
        record->provider_calls = result->provider_call_count;
        record->tool_calls = result->tool_call_count;
        sc_string_clear(&record->output);
        if (result->output.ptr != nullptr) {
            sc_status copy_status = sc_string_from_str(sc_allocator_heap(), sc_string_as_str(&result->output), &record->output);
            if (!sc_status_is_ok(copy_status) && record->status == SC_OK) {
                record->status = copy_status.code;
            }
            sc_status_clear(&copy_status);
        }
    }
    sc_status_clear(&status);
}

static void async_turn_record_clear(async_turn_record *record)
{
    if (record == nullptr) {
        return;
    }
    sc_string_clear(&record->output);
    *record = (async_turn_record){0};
}

static void record_turn_event(void *user_data, const sc_turn_event *event)
{
    turn_event_record *record = user_data;

    if (record == nullptr || event == nullptr) {
        return;
    }
    if (event->type == SC_TURN_EVENT_TOOL_CALLED) {
        record->tool_called += 1;
        if (event->name.ptr != nullptr) {
            size_t copy_len = event->name.len < sizeof(record->last_tool) - 1 ? event->name.len :
                                                                               sizeof(record->last_tool) - 1;
            (void)memcpy(record->last_tool, event->name.ptr, copy_len);
            record->last_tool[copy_len] = '\0';
        }
    } else if (event->type == SC_TURN_EVENT_TOOL_DENIED) {
        record->tool_denied += 1;
    } else if (event->type == SC_TURN_EVENT_TOOL_RESULT && event->status_code != SC_OK) {
        record->tool_failed += 1;
    }
}

static sc_status approve_tool(void *user_data, sc_str tool_name, sc_str arguments_json, sc_allocator *alloc, bool *out_approved)
{
    int *calls = user_data;
    (void)tool_name;
    (void)arguments_json;
    (void)alloc;

    if (out_approved == nullptr) {
        return sc_status_invalid_argument("sc.test.approval_invalid_argument");
    }
    if (calls != nullptr) {
        *calls += 1;
    }
    *out_approved = true;
    return sc_status_ok();
}

static sc_status decline_tool(void *user_data, sc_str tool_name, sc_str arguments_json, sc_allocator *alloc, bool *out_approved)
{
    int *calls = user_data;
    (void)tool_name;
    (void)arguments_json;
    (void)alloc;

    if (out_approved == nullptr) {
        return sc_status_invalid_argument("sc.test.approval_invalid_argument");
    }
    if (calls != nullptr) {
        *calls += 1;
    }
    *out_approved = false;
    return sc_status_ok();
}

static sc_status shutdown_drain(void *user_data, sc_allocator *alloc)
{
    shutdown_record *record = user_data;
    (void)alloc;
    record->drain = 1;
    return sc_status_ok();
}

static sc_status shutdown_flush(void *user_data, sc_allocator *alloc)
{
    shutdown_record *record = user_data;
    (void)alloc;
    record->flush = record->drain + 1;
    return sc_status_ok();
}

static sc_status shutdown_close(void *user_data, sc_allocator *alloc)
{
    shutdown_record *record = user_data;
    (void)alloc;
    record->close = record->flush + 1;
    return sc_status_ok();
}

static sc_status shutdown_destroy(void *user_data, sc_allocator *alloc)
{
    shutdown_record *record = user_data;
    (void)alloc;
    record->destroy = record->close + 1;
    return sc_status_ok();
}

static sc_status make_temp_path(const char *suffix, sc_string *out)
{
    char tmpl[] = "/tmp/sc-runtime-XXXXXX";
    char path[256] = {0};
    const char *dir = mkdtemp(tmpl);
    int written = 0;
    if (dir == nullptr || out == nullptr) {
        return sc_status_io("sc.test.temp_failed");
    }
    written = snprintf(path, sizeof(path), "%s/%s", dir, suffix);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return sc_status_io("sc.test.path_failed");
    }
    return sc_string_from_cstr(sc_allocator_heap(), path, out);
}

static int write_file(sc_str path, const char *text)
{
    FILE *file = fopen(path.ptr, "wb");
    if (file == nullptr) {
        return -1;
    }
    if (fputs(text, file) < 0) {
        (void)fclose(file);
        return -1;
    }
    return fclose(file);
}

static sc_status parse_json_object(sc_str json, sc_json_value **out)
{
    sc_json_parse_error error = {0};
    sc_status status = sc_json_parse(sc_allocator_heap(), json, out, &error);
    if (sc_status_is_ok(status) && sc_json_type_of(*out) != SC_JSON_OBJECT) {
        sc_json_destroy(*out);
        *out = nullptr;
        return sc_status_invalid_argument("sc.test.json_not_object");
    }
    return status;
}
