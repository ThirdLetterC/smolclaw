#include "mock_contracts.h"

#include "sc/peripheral.h"
#include "sc/registry.h"
#include "sc/runtime.h"
#include "sc/sandbox.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>

static int test_name_and_null_behavior(void);
static int test_provider_registry(void);
static int test_mock_contracts(void);
static int test_observer_list(void);
static int test_runtime_peripheral_sandbox(void);

static sc_status runtime_turn(void *impl,
                              const sc_runtime_turn *turn,
                              sc_allocator *alloc,
                              sc_runtime_turn_result *out);
static void counted_destroy(void *impl);
static sc_status peripheral_command(void *impl,
                                    const sc_peripheral_command *command,
                                    sc_allocator *alloc,
                                    sc_peripheral_result *out);
static sc_status sandbox_check(void *impl,
                               const sc_sandbox_request *request,
                               sc_allocator *alloc,
                               sc_sandbox_decision *out);

static const sc_runtime_vtab runtime_vtab = {
    .struct_size = sizeof(sc_runtime_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock-runtime",
    .display_name = "Mock runtime",
    .feature_flag = "SC_RUNTIME_MOCK",
    .capabilities = SC_CONTRACT_CAP_ASYNC,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .run_turn = runtime_turn,
    .destroy = counted_destroy,
};

static const sc_peripheral_vtab peripheral_vtab = {
    .struct_size = sizeof(sc_peripheral_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock-peripheral",
    .display_name = "Mock peripheral",
    .feature_flag = "SC_PERIPHERAL_MOCK",
    .capabilities = SC_CONTRACT_CAP_BINARY,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .command = peripheral_command,
    .destroy = counted_destroy,
};

static const sc_sandbox_vtab sandbox_vtab = {
    .struct_size = sizeof(sc_sandbox_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock-sandbox",
    .display_name = "Mock sandbox",
    .feature_flag = "SC_SANDBOX_MOCK",
    .capabilities = SC_CONTRACT_CAP_SECURE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .check = sandbox_check,
    .destroy = counted_destroy,
};

int main(void)
{
    int failures = 0;

    failures += test_name_and_null_behavior();
    failures += test_provider_registry();
    failures += test_mock_contracts();
    failures += test_observer_list();
    failures += test_runtime_peripheral_sandbox();

    return failures == 0 ? 0 : 1;
}

static int test_name_and_null_behavior(void)
{
    int failures = 0;
    sc_provider_response response = {0};
    sc_provider_vtab invalid = sc_mock_provider_vtab;

    failures += sc_test_expect_true("valid contract name", sc_contract_name_is_valid(sc_str_from_cstr("mock.provider-1")));
    failures += sc_test_expect_true("invalid contract name", !sc_contract_name_is_valid(sc_str_from_cstr("Mock Provider")));
    failures += sc_test_expect_status("null provider call",
                              sc_provider_generate(nullptr, nullptr, sc_allocator_heap(), &response),
                              SC_ERR_INVALID_ARGUMENT);

    invalid.generate = nullptr;
    failures += sc_test_expect_true("invalid missing vtab function", !sc_provider_vtab_valid(&invalid));
    invalid = sc_mock_provider_vtab;
    invalid.abi_major += 1;
    failures += sc_test_expect_true("invalid abi", !sc_provider_vtab_valid(&invalid));

    return failures;
}

static int test_provider_registry(void)
{
    int failures = 0;
    sc_provider_registry registry = {0};
    const sc_provider_registry_entry *entry = nullptr;
    sc_provider *provider = nullptr;
    sc_provider_response response = {0};
    sc_provider_request request = {
        .struct_size = sizeof(request),
        .model = sc_str_from_cstr("mock"),
        .prompt = sc_str_from_cstr("hello"),
    };

    sc_provider_registry_init(&registry, sc_allocator_heap());
    failures += sc_test_expect_status("registry register provider",
                              sc_provider_registry_register(&registry,
                                                            &sc_mock_provider_vtab,
                                                            sc_mock_provider_factory),
                              SC_OK);
    failures += sc_test_expect_status("registry duplicate provider",
                              sc_provider_registry_register(&registry,
                                                            &sc_mock_provider_vtab,
                                                            sc_mock_provider_factory),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_true("registry len", sc_provider_registry_len(&registry) == 1);
    entry = sc_provider_registry_at(&registry, 0);
    failures += sc_test_expect_true("registry deterministic at", entry != nullptr && strcmp(entry->vtab->name, "mock-provider") == 0);
    failures += sc_test_expect_true("registry capability survives",
                            entry != nullptr && (entry->vtab->capabilities & SC_CONTRACT_CAP_TOOLS) != 0);
    failures += sc_test_expect_status("registry create provider",
                              sc_provider_registry_create(&registry,
                                                          sc_str_from_cstr("mock-provider"),
                                                          sc_allocator_heap(),
                                                          &provider),
                              SC_OK);
    failures += sc_test_expect_status("registry provider generate",
                              sc_provider_generate(provider, &request, sc_allocator_heap(), &response),
                              SC_OK);
    failures += sc_test_expect_true("registry provider response", strcmp(response.text.ptr, "mock provider") == 0);

    sc_string_clear(&response.text);
    sc_provider_destroy(provider);
    sc_provider_registry_clear(&registry);
    return failures;
}

static int test_mock_contracts(void)
{
    int failures = 0;
    sc_mock_counts counts = {0};
    sc_provider *provider = nullptr;
    sc_tool *tool = nullptr;
    sc_channel *channel = nullptr;
    sc_memory *memory = nullptr;
    sc_mock_channel_state *channel_state = nullptr;
    sc_mock_memory_state *memory_state = nullptr;
    sc_provider_request request = {.struct_size = sizeof(request), .prompt = sc_str_from_cstr("p")};
    sc_provider_response provider_response = {0};
    sc_tool_call call = {.struct_size = sizeof(call), .call_id = sc_str_from_cstr("1")};
    sc_tool_result tool_result = {0};
    sc_channel_message message = {
        .struct_size = sizeof(message),
        .conversation_id = sc_str_from_cstr("c"),
        .text = sc_str_from_cstr("outbound"),
    };
    sc_memory_record record = {
        .struct_size = sizeof(record),
        .namespace_name = sc_str_from_cstr("default"),
        .key = sc_str_from_cstr("k"),
        .value = sc_str_from_cstr("v"),
    };
    sc_string memory_value = {0};

    failures += sc_test_expect_status("mock provider create",
                              sc_mock_provider_create(sc_allocator_heap(),
                                                      &counts,
                                                      sc_str_from_cstr("fixed"),
                                                      &provider),
                              SC_OK);
    failures += sc_test_expect_status("mock provider generate",
                              sc_provider_generate(provider, &request, sc_allocator_heap(), &provider_response),
                              SC_OK);
    failures += sc_test_expect_true("mock provider text", strcmp(provider_response.text.ptr, "fixed") == 0);

    failures += sc_test_expect_status("mock tool create",
                              sc_mock_tool_create(sc_allocator_heap(),
                                                  &counts,
                                                  sc_str_from_cstr("tool-output"),
                                                  &tool),
                              SC_OK);
    failures += sc_test_expect_status("mock tool invoke", sc_tool_invoke(tool, &call, sc_allocator_heap(), &tool_result), SC_OK);
    failures += sc_test_expect_true("mock tool output", strcmp(tool_result.output.ptr, "tool-output") == 0);

    failures += sc_test_expect_status("mock channel create",
                              sc_mock_channel_create(sc_allocator_heap(), &counts, &channel_state, &channel),
                              SC_OK);
    failures += sc_test_expect_status("mock channel send", sc_channel_send(channel, &message), SC_OK);
    failures += sc_test_expect_true("mock channel capture",
                            channel_state != nullptr && strcmp(channel_state->last_message.ptr, "outbound") == 0);

    failures += sc_test_expect_status("mock memory create",
                              sc_mock_memory_create(sc_allocator_heap(), &counts, &memory_state, &memory),
                              SC_OK);
    failures += sc_test_expect_status("mock memory put", sc_memory_put(memory, &record), SC_OK);
    failures += sc_test_expect_status("mock memory get",
                              sc_memory_get(memory,
                                            sc_str_from_cstr("default"),
                                            sc_str_from_cstr("k"),
                                            sc_allocator_heap(),
                                            &memory_value),
                              SC_OK);
    failures += sc_test_expect_true("mock memory value", strcmp(memory_value.ptr, "v") == 0);
    failures += sc_test_expect_true("mock memory state retained", memory_state != nullptr);

    sc_string_clear(&provider_response.text);
    sc_string_clear(&tool_result.output);
    sc_string_clear(&memory_value);
    sc_provider_destroy(provider);
    sc_tool_destroy(tool);
    sc_channel_destroy(channel);
    sc_memory_destroy(memory);
    failures += sc_test_expect_true("destroy once per mock", counts.destroy_calls == 4);
    failures += sc_test_expect_true("mock call counters",
                            counts.generate_calls == 1 && counts.invoke_calls == 1 && counts.send_calls == 1 &&
                                counts.memory_put_calls == 1 && counts.memory_get_calls == 1);
    return failures;
}

static int test_observer_list(void)
{
    int failures = 0;
    sc_mock_counts counts = {0};
    sc_observer *observer = nullptr;
    sc_observer_list list = {0};
    sc_observer_event event = {
        .struct_size = sizeof(event),
        .target = sc_str_from_cstr("test"),
        .name = sc_str_from_cstr("contracts.event"),
    };

    sc_observer_list_init(&list, sc_allocator_heap());
    failures += sc_test_expect_status("mock observer create",
                              sc_mock_observer_create(sc_allocator_heap(), &counts, nullptr, &observer),
                              SC_OK);
    failures += sc_test_expect_status("observer list add", sc_observer_list_add(&list, observer), SC_OK);
    failures += sc_test_expect_true("observer list len", sc_observer_list_len(&list) == 1);
    failures += sc_test_expect_status("observer list emit", sc_observer_list_emit(&list, &event), SC_OK);
    failures += sc_test_expect_status("observer list flush optional", sc_observer_list_flush(&list), SC_OK);
    sc_observer_list_clear(&list);
    failures += sc_test_expect_true("observer list destroy owned", counts.destroy_calls == 1 && counts.emit_calls == 1);
    return failures;
}

static int test_runtime_peripheral_sandbox(void)
{
    int failures = 0;
    int destroy_count = 0;
    sc_runtime *runtime = nullptr;
    sc_peripheral *peripheral = nullptr;
    sc_sandbox *sandbox = nullptr;
    sc_runtime_turn turn = {.struct_size = sizeof(turn), .input = sc_str_from_cstr("turn")};
    sc_runtime_turn_result turn_result = {0};
    sc_peripheral_command command = {.struct_size = sizeof(command), .operation = sc_str_from_cstr("ping")};
    sc_peripheral_result peripheral_result = {0};
    sc_sandbox_request sandbox_request = {
        .struct_size = sizeof(sandbox_request),
        .operation = sc_str_from_cstr("read"),
        .subject = sc_str_from_cstr("/tmp"),
    };
    sc_sandbox_decision decision = {0};

    failures += sc_test_expect_status("runtime create",
                              sc_runtime_new(sc_allocator_heap(), &runtime_vtab, &destroy_count, &runtime),
                              SC_OK);
    failures += sc_test_expect_status("runtime turn",
                              sc_runtime_run_turn(runtime, &turn, sc_allocator_heap(), &turn_result),
                              SC_OK);
    failures += sc_test_expect_true("runtime output", strcmp(turn_result.output.ptr, "turn") == 0);

    failures += sc_test_expect_status("peripheral create",
                              sc_peripheral_new(sc_allocator_heap(), &peripheral_vtab, &destroy_count, &peripheral),
                              SC_OK);
    failures += sc_test_expect_status("peripheral command",
                              sc_peripheral_command_send(peripheral,
                                                         &command,
                                                         sc_allocator_heap(),
                                                         &peripheral_result),
                              SC_OK);
    failures += sc_test_expect_true("peripheral payload", peripheral_result.payload.len == 4);

    failures += sc_test_expect_status("sandbox create",
                              sc_sandbox_new(sc_allocator_heap(), &sandbox_vtab, &destroy_count, &sandbox),
                              SC_OK);
    failures += sc_test_expect_status("sandbox check",
                              sc_sandbox_check(sandbox, &sandbox_request, sc_allocator_heap(), &decision),
                              SC_OK);
    failures += sc_test_expect_true("sandbox allowed", decision.allowed);

    sc_string_clear(&turn_result.output);
    sc_bytes_clear(&peripheral_result.payload);
    sc_string_clear(&decision.reason);
    sc_runtime_destroy(runtime);
    sc_peripheral_destroy(peripheral);
    sc_sandbox_destroy(sandbox);
    failures += sc_test_expect_true("runtime peripheral sandbox destroy", destroy_count == 3);
    return failures;
}

static sc_status runtime_turn(void *impl,
                              const sc_runtime_turn *turn,
                              sc_allocator *alloc,
                              sc_runtime_turn_result *out)
{
    (void)impl;
    out->struct_size = sizeof(*out);
    return sc_string_from_str(alloc, turn->input, &out->output);
}

static void counted_destroy(void *impl)
{
    int *count = impl;
    if (count != nullptr) {
        *count += 1;
    }
}

static sc_status peripheral_command(void *impl,
                                    const sc_peripheral_command *command,
                                    sc_allocator *alloc,
                                    sc_peripheral_result *out)
{
    (void)impl;
    (void)command;
    out->struct_size = sizeof(*out);
    return sc_bytes_from_buf(alloc, sc_buf_from_parts("pong", 4), &out->payload);
}

static sc_status sandbox_check(void *impl,
                               const sc_sandbox_request *request,
                               sc_allocator *alloc,
                               sc_sandbox_decision *out)
{
    (void)impl;
    (void)request;
    out->struct_size = sizeof(*out);
    out->allowed = true;
    return sc_string_from_cstr(alloc, "allowed", &out->reason);
}
