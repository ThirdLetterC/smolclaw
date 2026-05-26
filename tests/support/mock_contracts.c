#include "mock_contracts.h"

static sc_status mock_provider_generate(void *impl,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_response *out);
static void mock_provider_destroy(void *impl);
static sc_status mock_tool_spec(void *impl, sc_tool_spec *out);
static sc_status mock_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void mock_tool_destroy(void *impl);
static sc_status mock_channel_send(void *impl, const sc_channel_message *message);
static void mock_channel_destroy(void *impl);
static sc_status mock_memory_put(void *impl, const sc_memory_record *record);
static sc_status mock_memory_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out);
static void mock_memory_destroy(void *impl);
static sc_status mock_observer_emit(void *impl, const sc_observer_event *event);
static void mock_observer_destroy(void *impl);

const sc_provider_vtab sc_mock_provider_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock-provider",
    .display_name = "Mock provider",
    .feature_flag = "SC_PROVIDER_MOCK",
    .capabilities = SC_CONTRACT_CAP_TOOLS,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = mock_provider_generate,
    .destroy = mock_provider_destroy,
};

const sc_tool_vtab sc_mock_tool_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock-tool",
    .display_name = "Mock tool",
    .feature_flag = "SC_TOOL_MOCK",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = mock_tool_spec,
    .invoke = mock_tool_invoke,
    .destroy = mock_tool_destroy,
};

const sc_channel_vtab sc_mock_channel_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock-channel",
    .display_name = "Mock channel",
    .feature_flag = "SC_CHANNEL_MOCK",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = mock_channel_send,
    .destroy = mock_channel_destroy,
};

const sc_memory_vtab sc_mock_memory_vtab = {
    .struct_size = sizeof(sc_memory_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock-memory",
    .display_name = "Mock memory",
    .feature_flag = "SC_MEMORY_MOCK",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .put = mock_memory_put,
    .get = mock_memory_get,
    .destroy = mock_memory_destroy,
};

const sc_observer_vtab sc_mock_observer_vtab = {
    .struct_size = sizeof(sc_observer_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "mock-observer",
    .display_name = "Mock observer",
    .feature_flag = "SC_OBSERVER_MOCK",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .emit = mock_observer_emit,
    .flush = nullptr,
    .destroy = mock_observer_destroy,
};

sc_status sc_mock_provider_create(sc_allocator *alloc,
                                  sc_mock_counts *counts,
                                  sc_str text,
                                  sc_provider **out)
{
    sc_mock_provider_state *state = nullptr;
    sc_status status;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(sc_mock_provider_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (sc_mock_provider_state){.alloc = alloc, .counts = counts};
    status = sc_string_from_str(alloc, text, &state->text);
    if (!sc_status_is_ok(status)) {
        sc_free(alloc, state, sizeof(*state), _Alignof(sc_mock_provider_state));
        return status;
    }
    status = sc_provider_new(alloc, &sc_mock_provider_vtab, state, out);
    if (!sc_status_is_ok(status)) {
        mock_provider_destroy(state);
    }
    return status;
}

sc_status sc_mock_provider_factory(sc_allocator *alloc, sc_provider **out)
{
    return sc_mock_provider_create(alloc, nullptr, sc_str_from_cstr("mock provider"), out);
}

sc_status sc_mock_tool_create(sc_allocator *alloc, sc_mock_counts *counts, sc_str output, sc_tool **out)
{
    sc_mock_tool_state *state = nullptr;
    sc_status status;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(sc_mock_tool_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (sc_mock_tool_state){.alloc = alloc, .counts = counts};
    status = sc_string_from_str(alloc, output, &state->output);
    if (!sc_status_is_ok(status)) {
        sc_free(alloc, state, sizeof(*state), _Alignof(sc_mock_tool_state));
        return status;
    }
    status = sc_tool_new(alloc, &sc_mock_tool_vtab, state, out);
    if (!sc_status_is_ok(status)) {
        mock_tool_destroy(state);
    }
    return status;
}

sc_status sc_mock_tool_factory(sc_allocator *alloc, sc_tool **out)
{
    return sc_mock_tool_create(alloc, nullptr, sc_str_from_cstr("mock output"), out);
}

sc_status sc_mock_channel_create(sc_allocator *alloc,
                                 sc_mock_counts *counts,
                                 sc_mock_channel_state **state_out,
                                 sc_channel **out)
{
    sc_mock_channel_state *state = nullptr;
    sc_status status;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(sc_mock_channel_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (sc_mock_channel_state){.alloc = alloc, .counts = counts};
    status = sc_channel_new(alloc, &sc_mock_channel_vtab, state, out);
    if (!sc_status_is_ok(status)) {
        mock_channel_destroy(state);
        return status;
    }
    if (state_out != nullptr) {
        *state_out = state;
    }
    return sc_status_ok();
}

sc_status sc_mock_channel_factory(sc_allocator *alloc, sc_channel **out)
{
    return sc_mock_channel_create(alloc, nullptr, nullptr, out);
}

sc_status sc_mock_memory_create(sc_allocator *alloc,
                                sc_mock_counts *counts,
                                sc_mock_memory_state **state_out,
                                sc_memory **out)
{
    sc_mock_memory_state *state = nullptr;
    sc_status status;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(sc_mock_memory_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (sc_mock_memory_state){.alloc = alloc, .counts = counts};
    status = sc_memory_new(alloc, &sc_mock_memory_vtab, state, out);
    if (!sc_status_is_ok(status)) {
        mock_memory_destroy(state);
        return status;
    }
    if (state_out != nullptr) {
        *state_out = state;
    }
    return sc_status_ok();
}

sc_status sc_mock_memory_factory(sc_allocator *alloc, sc_memory **out)
{
    return sc_mock_memory_create(alloc, nullptr, nullptr, out);
}

sc_status sc_mock_observer_create(sc_allocator *alloc,
                                  sc_mock_counts *counts,
                                  sc_mock_observer_state **state_out,
                                  sc_observer **out)
{
    sc_mock_observer_state *state = nullptr;
    sc_status status;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(sc_mock_observer_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (sc_mock_observer_state){.alloc = alloc, .counts = counts};
    status = sc_observer_new(alloc, &sc_mock_observer_vtab, state, out);
    if (!sc_status_is_ok(status)) {
        mock_observer_destroy(state);
        return status;
    }
    if (state_out != nullptr) {
        *state_out = state;
    }
    return sc_status_ok();
}

sc_status sc_mock_observer_factory(sc_allocator *alloc, sc_observer **out)
{
    return sc_mock_observer_create(alloc, nullptr, nullptr, out);
}

static sc_status mock_provider_generate(void *impl,
                                        const sc_provider_request *request,
                                        sc_allocator *alloc,
                                        sc_provider_response *out)
{
    sc_mock_provider_state *state = impl;
    (void)request;
    if (state->counts != nullptr) {
        state->counts->generate_calls += 1;
    }
    out->struct_size = sizeof(*out);
    return sc_string_from_str(alloc, sc_string_as_str(&state->text), &out->text);
}

static void mock_provider_destroy(void *impl)
{
    sc_mock_provider_state *state = impl;
    if (state == nullptr) {
        return;
    }
    if (state->counts != nullptr) {
        state->counts->destroy_calls += 1;
    }
    sc_string_clear(&state->text);
    sc_free(state->alloc, state, sizeof(*state), _Alignof(sc_mock_provider_state));
}

static sc_status mock_tool_spec(void *impl, sc_tool_spec *out)
{
    (void)impl;
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_str_from_cstr("mock-tool"),
        .description = sc_str_from_cstr("Mock tool"),
        .input_schema = nullptr,
        .capabilities = SC_CONTRACT_CAP_NONE,
    };
    return sc_status_ok();
}

static sc_status mock_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_mock_tool_state *state = impl;
    (void)call;
    if (state->counts != nullptr) {
        state->counts->invoke_calls += 1;
    }
    out->struct_size = sizeof(*out);
    out->success = true;
    return sc_string_from_str(alloc, sc_string_as_str(&state->output), &out->output);
}

static void mock_tool_destroy(void *impl)
{
    sc_mock_tool_state *state = impl;
    if (state == nullptr) {
        return;
    }
    if (state->counts != nullptr) {
        state->counts->destroy_calls += 1;
    }
    sc_string_clear(&state->output);
    sc_free(state->alloc, state, sizeof(*state), _Alignof(sc_mock_tool_state));
}

static sc_status mock_channel_send(void *impl, const sc_channel_message *message)
{
    sc_mock_channel_state *state = impl;
    if (state->counts != nullptr) {
        state->counts->send_calls += 1;
    }
    sc_string_clear(&state->last_message);
    return sc_string_from_str(state->alloc, message->text, &state->last_message);
}

static void mock_channel_destroy(void *impl)
{
    sc_mock_channel_state *state = impl;
    if (state == nullptr) {
        return;
    }
    if (state->counts != nullptr) {
        state->counts->destroy_calls += 1;
    }
    sc_string_clear(&state->last_message);
    sc_free(state->alloc, state, sizeof(*state), _Alignof(sc_mock_channel_state));
}

static sc_status mock_memory_put(void *impl, const sc_memory_record *record)
{
    sc_mock_memory_state *state = impl;
    sc_status status;
    if (state->counts != nullptr) {
        state->counts->memory_put_calls += 1;
    }
    sc_string_clear(&state->key);
    sc_string_clear(&state->value);
    status = sc_string_from_str(state->alloc, record->key, &state->key);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(state->alloc, record->value, &state->value);
    }
    return status;
}

static sc_status mock_memory_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out)
{
    sc_mock_memory_state *state = impl;
    (void)namespace_name;
    if (state->counts != nullptr) {
        state->counts->memory_get_calls += 1;
    }
    if (!sc_str_equal(sc_string_as_str(&state->key), key)) {
        return sc_status_invalid_argument("sc.mock_memory.not_found");
    }
    return sc_string_from_str(alloc, sc_string_as_str(&state->value), out);
}

static void mock_memory_destroy(void *impl)
{
    sc_mock_memory_state *state = impl;
    if (state == nullptr) {
        return;
    }
    if (state->counts != nullptr) {
        state->counts->destroy_calls += 1;
    }
    sc_string_clear(&state->key);
    sc_string_clear(&state->value);
    sc_free(state->alloc, state, sizeof(*state), _Alignof(sc_mock_memory_state));
}

static sc_status mock_observer_emit(void *impl, const sc_observer_event *event)
{
    sc_mock_observer_state *state = impl;
    if (state->counts != nullptr) {
        state->counts->emit_calls += 1;
    }
    sc_string_clear(&state->last_event);
    return sc_string_from_str(state->alloc, event->name, &state->last_event);
}

static void mock_observer_destroy(void *impl)
{
    sc_mock_observer_state *state = impl;
    if (state == nullptr) {
        return;
    }
    if (state->counts != nullptr) {
        state->counts->destroy_calls += 1;
    }
    sc_string_clear(&state->last_event);
    sc_free(state->alloc, state, sizeof(*state), _Alignof(sc_mock_observer_state));
}
