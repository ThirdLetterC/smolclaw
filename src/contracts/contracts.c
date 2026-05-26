#include "sc/channel.h"
#include "sc/memory.h"
#include "sc/observer.h"
#include "sc/peripheral.h"
#include "sc/provider.h"
#include "sc/runtime.h"
#include "sc/sandbox.h"
#include "sc/tool.h"

#include "contracts/contracts_internal.h"

#include <ctype.h>
#include <string.h>

struct sc_provider {
    sc_contract_handle base;
};

struct sc_channel {
    sc_contract_handle base;
};

struct sc_tool {
    sc_contract_handle base;
};

struct sc_memory {
    sc_contract_handle base;
};

struct sc_observer {
    sc_contract_handle base;
};

struct sc_runtime {
    sc_contract_handle base;
};

struct sc_peripheral {
    sc_contract_handle base;
};

struct sc_sandbox {
    sc_contract_handle base;
};

typedef struct runtime_facade {
    sc_allocator *alloc;
    sc_agent *agent;
    sc_cancel_token active_cancel;
    sc_runtime_turn_id next_turn_id;
    sc_runtime_turn_id active_turn_id;
    bool active;
} runtime_facade;

static sc_status provider_stream_from_generate(sc_provider *provider,
                                               const sc_provider_request *request,
                                               sc_allocator *alloc,
                                               sc_provider_stream_callback callback,
                                               void *callback_user_data);
static sc_status provider_emit_tool_call_event(sc_allocator *alloc,
                                               const sc_provider_tool_call *call,
                                               sc_provider_stream_callback callback,
                                               void *callback_user_data);
static sc_status runtime_facade_run_turn(void *impl,
                                         const sc_runtime_turn *turn,
                                         sc_allocator *alloc,
                                         sc_runtime_turn_result *out);
static void runtime_facade_destroy(void *impl);

static const sc_runtime_vtab runtime_facade_vtab = {
    .struct_size = sizeof(sc_runtime_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "runtime-facade",
    .display_name = "Runtime facade",
    .feature_flag = "SC_RUNTIME_FACADE",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .run_turn = runtime_facade_run_turn,
    .destroy = runtime_facade_destroy,
};

bool sc_contract_name_is_valid(sc_str name)
{
    if (name.len == 0 || name.len > 64 || name.ptr == nullptr) {
        return false;
    }
    if (!islower((unsigned char)name.ptr[0]) && !isdigit((unsigned char)name.ptr[0])) {
        return false;
    }
    for (size_t i = 0; i < name.len; ++i) {
        unsigned char ch = (unsigned char)name.ptr[i];
        if (!islower(ch) && !isdigit(ch) && ch != '-' && ch != '_' && ch != '.') {
            return false;
        }
    }
    return true;
}

bool sc_provider_vtab_valid(const sc_provider_vtab *vtab)
{
    return vtab != nullptr &&
           sc_contract_common_vtab_valid(vtab->struct_size, vtab->abi_major, vtab->name, vtab->generate != nullptr, vtab->destroy != nullptr) &&
           vtab->struct_size >= sizeof(*vtab);
}

sc_status sc_provider_new(sc_allocator *alloc,
                          const sc_provider_vtab *vtab,
                          void *impl,
                          sc_provider **out)
{
    if (out == nullptr || !sc_provider_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.provider.invalid_vtab");
    }
    return sc_contract_handle_new(alloc, vtab, impl, sizeof(sc_provider), (void **)out);
}

sc_status sc_provider_generate(sc_provider *provider,
                               const sc_provider_request *request,
                               sc_allocator *alloc,
                               sc_provider_response *out)
{
    const sc_provider_vtab *vtab = provider == nullptr ? nullptr : provider->base.vtab;
    if (provider == nullptr || request == nullptr || out == nullptr || vtab == nullptr || vtab->generate == nullptr) {
        return sc_status_invalid_argument("sc.provider.invalid_argument");
    }
    if (request->cancel_requested) {
        return sc_status_cancelled("sc.provider.cancelled");
    }
    return vtab->generate(provider->base.impl, request, alloc, out);
}

sc_status sc_provider_stream(sc_provider *provider,
                             const sc_provider_request *request,
                             sc_allocator *alloc,
                             sc_provider_stream_callback callback,
                             void *callback_user_data)
{
    const sc_provider_vtab *vtab = provider == nullptr ? nullptr : provider->base.vtab;
    if (provider == nullptr || request == nullptr || callback == nullptr || vtab == nullptr || vtab->generate == nullptr) {
        return sc_status_invalid_argument("sc.provider.stream_invalid_argument");
    }
    if (request->cancel_requested) {
        return sc_status_cancelled("sc.provider.cancelled");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    if (vtab->stream != nullptr) {
        return vtab->stream(provider->base.impl, request, alloc, callback, callback_user_data);
    }
    return provider_stream_from_generate(provider, request, alloc, callback, callback_user_data);
}

sc_status sc_provider_generate_async(sc_provider *provider,
                                     sc_async_context *context,
                                     const sc_provider_request *request,
                                     sc_allocator *alloc,
                                     sc_provider_generate_complete_fn complete,
                                     void *complete_user_data,
                                     sc_async_op **out)
{
    const sc_provider_vtab *vtab = provider == nullptr ? nullptr : provider->base.vtab;
    sc_provider_response response = {0};
    sc_status status;

    if (provider == nullptr || request == nullptr || complete == nullptr || vtab == nullptr || vtab->generate == nullptr) {
        return sc_status_invalid_argument("sc.provider.async_invalid_argument");
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    if (vtab->generate_async != nullptr) {
        return vtab->generate_async(provider->base.impl, context, request, alloc, complete, complete_user_data, out);
    }
    (void)context;
    status = sc_provider_generate(provider, request, alloc, &response);
    complete(complete_user_data, sc_status_is_ok(status) ? &response : nullptr, status);
    sc_provider_response_clear(&response);
    return sc_status_ok();
}

sc_status sc_provider_stream_async(sc_provider *provider,
                                   sc_async_context *context,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_provider_stream_callback callback,
                                   void *callback_user_data,
                                   sc_provider_stream_complete_fn complete,
                                   void *complete_user_data,
                                   sc_async_op **out)
{
    const sc_provider_vtab *vtab = provider == nullptr ? nullptr : provider->base.vtab;
    sc_status status;

    if (provider == nullptr || request == nullptr || callback == nullptr || complete == nullptr || vtab == nullptr || vtab->generate == nullptr) {
        return sc_status_invalid_argument("sc.provider.async_stream_invalid_argument");
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    if (vtab->stream_async != nullptr) {
        return vtab->stream_async(provider->base.impl,
                                  context,
                                  request,
                                  alloc,
                                  callback,
                                  callback_user_data,
                                  complete,
                                  complete_user_data,
                                  out);
    }
    (void)context;
    status = sc_provider_stream(provider, request, alloc, callback, callback_user_data);
    complete(complete_user_data, status);
    return sc_status_ok();
}

const sc_provider_vtab *sc_provider_vtab_of(const sc_provider *provider)
{
    return provider == nullptr ? nullptr : provider->base.vtab;
}

void sc_provider_destroy(sc_provider *provider)
{
    const sc_provider_vtab *vtab = provider == nullptr ? nullptr : provider->base.vtab;
    sc_contract_handle_destroy(provider == nullptr ? nullptr : &provider->base, sizeof(*provider), vtab == nullptr ? nullptr : vtab->destroy);
}

static sc_status provider_stream_from_generate(sc_provider *provider,
                                               const sc_provider_request *request,
                                               sc_allocator *alloc,
                                               sc_provider_stream_callback callback,
                                               void *callback_user_data)
{
    sc_provider_response response = {0};
    sc_provider_stream_event event = {.struct_size = sizeof(event)};
    sc_status status = sc_provider_generate(provider, request, alloc, &response);

    if (sc_status_is_ok(status) && response.text.len > 0) {
        event.type = SC_PROVIDER_STREAM_DELTA;
        status = sc_string_from_str(alloc, sc_string_as_str(&response.text), &event.text);
        if (sc_status_is_ok(status)) {
            status = callback(callback_user_data, &event);
        }
        sc_provider_stream_event_clear(&event);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < response.tool_calls.len; i += 1) {
        const sc_provider_tool_call *call = sc_vec_at_const(&response.tool_calls, i);
        status = provider_emit_tool_call_event(alloc, call, callback, callback_user_data);
    }
    if (sc_status_is_ok(status)) {
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_DONE};
        status = callback(callback_user_data, &event);
    }
    sc_provider_response_clear(&response);
    return status;
}

static sc_status provider_emit_tool_call_event(sc_allocator *alloc,
                                               const sc_provider_tool_call *call,
                                               sc_provider_stream_callback callback,
                                               void *callback_user_data)
{
    sc_provider_stream_event event = {
        .struct_size = sizeof(event),
        .type = SC_PROVIDER_STREAM_TOOL_CALL,
    };
    sc_status status;

    if (call == nullptr) {
        return sc_status_invalid_argument("sc.provider.stream_tool_call_invalid_argument");
    }
    status = sc_string_from_str(alloc, sc_string_as_str(&call->call_id), &event.tool_call.call_id);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&call->name), &event.tool_call.name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&call->arguments_json), &event.tool_call.arguments_json);
    }
    if (sc_status_is_ok(status) && call->arguments != nullptr) {
        status = sc_json_clone(call->arguments, alloc, &event.tool_call.arguments);
    }
    if (sc_status_is_ok(status)) {
        status = callback(callback_user_data, &event);
    }
    sc_provider_stream_event_clear(&event);
    return status;
}

bool sc_channel_vtab_valid(const sc_channel_vtab *vtab)
{
    return vtab != nullptr &&
           sc_contract_common_vtab_valid(vtab->struct_size, vtab->abi_major, vtab->name, vtab->send != nullptr, vtab->destroy != nullptr) &&
           vtab->struct_size >= sizeof(*vtab);
}

sc_status sc_channel_new(sc_allocator *alloc, const sc_channel_vtab *vtab, void *impl, sc_channel **out)
{
    if (out == nullptr || !sc_channel_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.channel.invalid_vtab");
    }
    return sc_contract_handle_new(alloc, vtab, impl, sizeof(sc_channel), (void **)out);
}

sc_status sc_channel_send(sc_channel *channel, const sc_channel_message *message)
{
    const sc_channel_vtab *vtab = channel == nullptr ? nullptr : channel->base.vtab;
    if (channel == nullptr || message == nullptr || vtab == nullptr || vtab->send == nullptr) {
        return sc_status_invalid_argument("sc.channel.invalid_argument");
    }
    return vtab->send(channel->base.impl, message);
}

sc_status sc_channel_listen(sc_channel *channel, sc_allocator *alloc, sc_channel_inbound *out)
{
    const sc_channel_vtab *vtab = channel == nullptr ? nullptr : channel->base.vtab;
    if (channel == nullptr || out == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.channel.invalid_argument");
    }
    if (vtab->listen == nullptr) {
        return sc_status_unsupported("sc.channel.listen_unsupported");
    }
    return vtab->listen(channel->base.impl, alloc, out);
}

sc_status sc_channel_health_check(sc_channel *channel, sc_allocator *alloc, sc_channel_health *out)
{
    const sc_channel_vtab *vtab = channel == nullptr ? nullptr : channel->base.vtab;
    if (channel == nullptr || out == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.channel.invalid_argument");
    }
    if (vtab->health == nullptr) {
        *out = (sc_channel_health){.struct_size = sizeof(*out), .healthy = true};
        return sc_string_from_cstr(alloc, "ok", &out->message);
    }
    return vtab->health(channel->base.impl, alloc, out);
}

sc_status sc_channel_request_approval(sc_channel *channel,
                                      const sc_channel_approval_request *request,
                                      sc_allocator *alloc,
                                      sc_channel_approval_response *out)
{
    const sc_channel_vtab *vtab = channel == nullptr ? nullptr : channel->base.vtab;
    if (channel == nullptr || request == nullptr || out == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.channel.invalid_argument");
    }
    if (vtab->request_approval == nullptr) {
        return sc_status_unsupported("sc.channel.approval_unsupported");
    }
    return vtab->request_approval(channel->base.impl, request, alloc, out);
}

const sc_channel_vtab *sc_channel_vtab_of(const sc_channel *channel)
{
    return channel == nullptr ? nullptr : channel->base.vtab;
}

void sc_channel_destroy(sc_channel *channel)
{
    const sc_channel_vtab *vtab = channel == nullptr ? nullptr : channel->base.vtab;
    sc_contract_handle_destroy(channel == nullptr ? nullptr : &channel->base, sizeof(*channel), vtab == nullptr ? nullptr : vtab->destroy);
}

bool sc_tool_vtab_valid(const sc_tool_vtab *vtab)
{
    return vtab != nullptr &&
           sc_contract_common_vtab_valid(vtab->struct_size, vtab->abi_major, vtab->name, vtab->invoke != nullptr, vtab->destroy != nullptr) &&
           vtab->struct_size >= sizeof(*vtab) && vtab->spec != nullptr;
}

sc_status sc_tool_new(sc_allocator *alloc, const sc_tool_vtab *vtab, void *impl, sc_tool **out)
{
    if (out == nullptr || !sc_tool_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.tool.invalid_vtab");
    }
    return sc_contract_handle_new(alloc, vtab, impl, sizeof(sc_tool), (void **)out);
}

sc_status sc_tool_spec_get(sc_tool *tool, sc_tool_spec *out)
{
    const sc_tool_vtab *vtab = tool == nullptr ? nullptr : tool->base.vtab;
    if (tool == nullptr || out == nullptr || vtab == nullptr || vtab->spec == nullptr) {
        return sc_status_invalid_argument("sc.tool.invalid_argument");
    }
    return vtab->spec(tool->base.impl, out);
}

sc_status sc_tool_invoke(sc_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    const sc_tool_vtab *vtab = tool == nullptr ? nullptr : tool->base.vtab;
    sc_tool_spec spec = {0};
    sc_status status;
    if (tool == nullptr || call == nullptr || out == nullptr || vtab == nullptr || vtab->invoke == nullptr) {
        return sc_status_invalid_argument("sc.tool.invalid_argument");
    }
    status = sc_tool_spec_get(tool, &spec);
    if (sc_status_is_ok(status)) {
        status = sc_tool_validate_call_against_schema(&spec, call);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return vtab->invoke(tool->base.impl, call, alloc, out);
}

sc_status sc_tool_invoke_async(sc_tool *tool,
                               sc_async_context *context,
                               const sc_tool_call *call,
                               sc_allocator *alloc,
                               sc_tool_invoke_complete_fn complete,
                               void *complete_user_data,
                               sc_async_op **out)
{
    const sc_tool_vtab *vtab = tool == nullptr ? nullptr : tool->base.vtab;
    sc_tool_result result = {0};
    sc_status status;

    if (tool == nullptr || call == nullptr || complete == nullptr || vtab == nullptr || vtab->invoke == nullptr) {
        return sc_status_invalid_argument("sc.tool.async_invalid_argument");
    }
    if (out != nullptr) {
        *out = nullptr;
    }
    if (vtab->invoke_async != nullptr) {
        return vtab->invoke_async(tool->base.impl, context, call, alloc, complete, complete_user_data, out);
    }
    (void)context;
    status = sc_tool_invoke(tool, call, alloc, &result);
    complete(complete_user_data, sc_status_is_ok(status) ? &result : nullptr, status);
    sc_tool_result_clear(&result);
    return sc_status_ok();
}

const sc_tool_vtab *sc_tool_vtab_of(const sc_tool *tool)
{
    return tool == nullptr ? nullptr : tool->base.vtab;
}

void sc_tool_destroy(sc_tool *tool)
{
    const sc_tool_vtab *vtab = tool == nullptr ? nullptr : tool->base.vtab;
    sc_contract_handle_destroy(tool == nullptr ? nullptr : &tool->base, sizeof(*tool), vtab == nullptr ? nullptr : vtab->destroy);
}

bool sc_memory_vtab_valid(const sc_memory_vtab *vtab)
{
    return vtab != nullptr &&
           sc_contract_common_vtab_valid(vtab->struct_size, vtab->abi_major, vtab->name, vtab->put != nullptr, vtab->destroy != nullptr) &&
           vtab->struct_size >= sizeof(*vtab) && vtab->get != nullptr;
}

sc_status sc_memory_new(sc_allocator *alloc, const sc_memory_vtab *vtab, void *impl, sc_memory **out)
{
    if (out == nullptr || !sc_memory_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.memory.invalid_vtab");
    }
    return sc_contract_handle_new(alloc, vtab, impl, sizeof(sc_memory), (void **)out);
}

sc_status sc_memory_put(sc_memory *memory, const sc_memory_record *record)
{
    const sc_memory_vtab *vtab = memory == nullptr ? nullptr : memory->base.vtab;
    if (memory == nullptr || record == nullptr || vtab == nullptr || vtab->put == nullptr) {
        return sc_status_invalid_argument("sc.memory.invalid_argument");
    }
    return vtab->put(memory->base.impl, record);
}

sc_status sc_memory_get(sc_memory *memory, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out)
{
    const sc_memory_vtab *vtab = memory == nullptr ? nullptr : memory->base.vtab;
    if (memory == nullptr || out == nullptr || vtab == nullptr || vtab->get == nullptr) {
        return sc_status_invalid_argument("sc.memory.invalid_argument");
    }
    return vtab->get(memory->base.impl, namespace_name, key, alloc, out);
}

sc_status sc_memory_search(sc_memory *memory, const sc_memory_query *query, sc_allocator *alloc, sc_memory_result *out)
{
    const sc_memory_vtab *vtab = memory == nullptr ? nullptr : memory->base.vtab;
    if (memory == nullptr || out == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.memory.invalid_argument");
    }
    if (vtab->search == nullptr) {
        return sc_status_unsupported("sc.memory.search_unsupported");
    }
    return vtab->search(memory->base.impl, query, alloc, out);
}

sc_status sc_memory_forget(sc_memory *memory, sc_str namespace_name, sc_str key)
{
    const sc_memory_vtab *vtab = memory == nullptr ? nullptr : memory->base.vtab;
    if (memory == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.memory.invalid_argument");
    }
    if (vtab->forget == nullptr) {
        return sc_status_unsupported("sc.memory.forget_unsupported");
    }
    return vtab->forget(memory->base.impl, namespace_name, key);
}

sc_status sc_memory_redact(sc_memory *memory, sc_str namespace_name, sc_str key)
{
    const sc_memory_vtab *vtab = memory == nullptr ? nullptr : memory->base.vtab;
    if (memory == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.memory.invalid_argument");
    }
    if (vtab->redact != nullptr) {
        return vtab->redact(memory->base.impl, namespace_name, key);
    }
    if (vtab->forget == nullptr) {
        return sc_status_unsupported("sc.memory.redact_unsupported");
    }
    return vtab->forget(memory->base.impl, namespace_name, key);
}

sc_status sc_memory_purge_namespace(sc_memory *memory, sc_str namespace_name)
{
    const sc_memory_vtab *vtab = memory == nullptr ? nullptr : memory->base.vtab;
    if (memory == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.memory.invalid_argument");
    }
    if (vtab->purge_namespace == nullptr) {
        return sc_status_unsupported("sc.memory.purge_namespace_unsupported");
    }
    return vtab->purge_namespace(memory->base.impl, namespace_name);
}

sc_status sc_memory_purge_session(sc_memory *memory, sc_str namespace_name, sc_str session_id)
{
    const sc_memory_vtab *vtab = memory == nullptr ? nullptr : memory->base.vtab;
    if (memory == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.memory.invalid_argument");
    }
    if (vtab->purge_session == nullptr) {
        return sc_status_unsupported("sc.memory.purge_session_unsupported");
    }
    return vtab->purge_session(memory->base.impl, namespace_name, session_id);
}

sc_status sc_memory_export_snapshot(sc_memory *memory, const sc_memory_query *query, sc_allocator *alloc, sc_string *out)
{
    return sc_memory_export_snapshot_ex(memory, query, nullptr, alloc, out);
}

sc_status sc_memory_export_snapshot_ex(sc_memory *memory,
                                       const sc_memory_query *query,
                                       const sc_memory_export_options *options,
                                       sc_allocator *alloc,
                                       sc_string *out)
{
    const sc_memory_vtab *vtab = memory == nullptr ? nullptr : memory->base.vtab;
    if (memory == nullptr || out == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.memory.invalid_argument");
    }
    if (vtab->export_snapshot_ex != nullptr) {
        return vtab->export_snapshot_ex(memory->base.impl, query, options, alloc, out);
    }
    if (vtab->export_snapshot == nullptr) {
        return sc_status_unsupported("sc.memory.export_unsupported");
    }
    return vtab->export_snapshot(memory->base.impl, query, alloc, out);
}

const sc_memory_vtab *sc_memory_vtab_of(const sc_memory *memory)
{
    return memory == nullptr ? nullptr : memory->base.vtab;
}

void sc_memory_destroy(sc_memory *memory)
{
    const sc_memory_vtab *vtab = memory == nullptr ? nullptr : memory->base.vtab;
    sc_contract_handle_destroy(memory == nullptr ? nullptr : &memory->base, sizeof(*memory), vtab == nullptr ? nullptr : vtab->destroy);
}

bool sc_observer_vtab_valid(const sc_observer_vtab *vtab)
{
    return vtab != nullptr &&
           sc_contract_common_vtab_valid(vtab->struct_size, vtab->abi_major, vtab->name, vtab->emit != nullptr, vtab->destroy != nullptr) &&
           vtab->struct_size >= sizeof(*vtab);
}

sc_status sc_observer_new(sc_allocator *alloc, const sc_observer_vtab *vtab, void *impl, sc_observer **out)
{
    if (out == nullptr || !sc_observer_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.observer.invalid_vtab");
    }
    return sc_contract_handle_new(alloc, vtab, impl, sizeof(sc_observer), (void **)out);
}

sc_status sc_observer_emit(sc_observer *observer, const sc_observer_event *event)
{
    const sc_observer_vtab *vtab = observer == nullptr ? nullptr : observer->base.vtab;
    sc_log_field stack_fields[32] = {0};
    sc_log_field *fields = stack_fields;
    sc_observer_event sanitized = {0};
    sc_status status;

    if (observer == nullptr || event == nullptr || vtab == nullptr || vtab->emit == nullptr) {
        return sc_status_invalid_argument("sc.observer.invalid_argument");
    }
    if (event->field_count > 0 && event->fields == nullptr) {
        return sc_status_invalid_argument("sc.observer.invalid_argument");
    }
    if (event->field_count > SC_ARRAY_LEN(stack_fields)) {
        if (event->field_count > SIZE_MAX / sizeof(*fields)) {
            return sc_status_invalid_argument("sc.observer.field_count_overflow");
        }
        fields = sc_alloc(sc_allocator_heap(), event->field_count * sizeof(*fields), _Alignof(sc_log_field));
        if (fields == nullptr) {
            return sc_status_no_memory();
        }
    }
    for (size_t i = 0; i < event->field_count; i += 1) {
        fields[i] = event->fields[i];
        fields[i].value = sc_log_redact_field(&event->fields[i]);
        fields[i].secret = event->fields[i].secret || !sc_str_equal(fields[i].value, event->fields[i].value);
    }
    sanitized = *event;
    sanitized.fields = fields;
    status = vtab->emit(observer->base.impl, &sanitized);
    if (fields != stack_fields) {
        sc_free(sc_allocator_heap(), fields, event->field_count * sizeof(*fields), _Alignof(sc_log_field));
    }
    return status;
}

sc_status sc_observer_emit_safe(sc_observer *observer, const sc_observer_event *event, size_t *failure_count)
{
    sc_status status;

    if (failure_count != nullptr) {
        *failure_count = 0;
    }
    if (observer == nullptr) {
        return sc_status_ok();
    }
    status = sc_observer_emit(observer, event);
    if (!sc_status_is_ok(status)) {
        if (failure_count != nullptr) {
            *failure_count = 1;
        }
        sc_status_clear(&status);
    }
    return sc_status_ok();
}

sc_status sc_observer_flush(sc_observer *observer)
{
    const sc_observer_vtab *vtab = observer == nullptr ? nullptr : observer->base.vtab;
    if (observer == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.observer.invalid_argument");
    }
    if (vtab->flush == nullptr) {
        return sc_status_ok();
    }
    return vtab->flush(observer->base.impl);
}

const sc_observer_vtab *sc_observer_vtab_of(const sc_observer *observer)
{
    return observer == nullptr ? nullptr : observer->base.vtab;
}

void sc_observer_destroy(sc_observer *observer)
{
    const sc_observer_vtab *vtab = observer == nullptr ? nullptr : observer->base.vtab;
    sc_contract_handle_destroy(observer == nullptr ? nullptr : &observer->base, sizeof(*observer), vtab == nullptr ? nullptr : vtab->destroy);
}

bool sc_runtime_vtab_valid(const sc_runtime_vtab *vtab)
{
    return vtab != nullptr &&
           sc_contract_common_vtab_valid(vtab->struct_size, vtab->abi_major, vtab->name, vtab->run_turn != nullptr, vtab->destroy != nullptr) &&
           vtab->struct_size >= sizeof(*vtab);
}

sc_status sc_runtime_new(sc_allocator *alloc, const sc_runtime_vtab *vtab, void *impl, sc_runtime **out)
{
    if (out == nullptr || !sc_runtime_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.runtime.invalid_vtab");
    }
    return sc_contract_handle_new(alloc, vtab, impl, sizeof(sc_runtime), (void **)out);
}

sc_status sc_runtime_run_turn(sc_runtime *runtime,
                              const sc_runtime_turn *turn,
                              sc_allocator *alloc,
                              sc_runtime_turn_result *out)
{
    const sc_runtime_vtab *vtab = runtime == nullptr ? nullptr : runtime->base.vtab;
    if (runtime == nullptr || turn == nullptr || out == nullptr || vtab == nullptr || vtab->run_turn == nullptr) {
        return sc_status_invalid_argument("sc.runtime.invalid_argument");
    }
    return vtab->run_turn(runtime->base.impl, turn, alloc, out);
}

const sc_runtime_vtab *sc_runtime_vtab_of(const sc_runtime *runtime)
{
    return runtime == nullptr ? nullptr : runtime->base.vtab;
}

void sc_runtime_destroy(sc_runtime *runtime)
{
    const sc_runtime_vtab *vtab = runtime == nullptr ? nullptr : runtime->base.vtab;
    sc_contract_handle_destroy(runtime == nullptr ? nullptr : &runtime->base, sizeof(*runtime), vtab == nullptr ? nullptr : vtab->destroy);
}

sc_status sc_runtime_create(sc_allocator *alloc, const sc_runtime_config *config, sc_runtime **out)
{
    runtime_facade *facade = nullptr;
    sc_agent_options agent_options = {0};
    sc_status status;

    if (out == nullptr || config == nullptr || config->provider == nullptr) {
        return sc_status_invalid_argument("sc.runtime_create.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    facade = sc_alloc(alloc, sizeof(*facade), _Alignof(runtime_facade));
    if (facade == nullptr) {
        return sc_status_no_memory();
    }
    *facade = (runtime_facade){.alloc = alloc, .next_turn_id = 1};
    status = sc_cancel_token_init(&facade->active_cancel, alloc);
    agent_options = (sc_agent_options){
        .struct_size = sizeof(agent_options),
        .provider = config->provider,
        .memory = config->memory,
        .tools = config->tools,
        .tool_count = config->tool_count,
        .observer = config->observer,
        .policy = config->policy,
        .estop = config->estop,
        .model = config->model,
        .identity = config->identity,
        .workspace = config->workspace,
        .runtime_environment = config->runtime_environment,
        .memory_namespace = config->memory_namespace,
        .turn_namespace = config->turn_namespace,
        .max_history_messages = config->max_history_messages,
        .max_memory_entries = config->max_memory_entries,
        .max_prompt_bytes = config->max_prompt_bytes,
        .max_tool_iterations = config->max_tool_iterations,
        .max_tool_output_bytes = config->max_tool_output_bytes,
        .deterministic_prompts = config->deterministic_prompts || !config->include_wall_time,
        .include_wall_time = config->include_wall_time,
        .use_streaming = config->use_streaming,
        .emit_stream_deltas = config->emit_stream_deltas,
        .default_timeout_ms = config->default_timeout_ms,
    };
    if (sc_status_is_ok(status)) {
        status = sc_agent_new(alloc, &agent_options, &facade->agent);
    }
    if (sc_status_is_ok(status)) {
        status = sc_runtime_new(alloc, &runtime_facade_vtab, facade, out);
    }
    if (!sc_status_is_ok(status)) {
        runtime_facade_destroy(facade);
    }
    return status;
}

sc_status sc_runtime_process_message(sc_runtime *runtime,
                                     const sc_runtime_message *message,
                                     sc_allocator *alloc,
                                     sc_runtime_response *out)
{
    runtime_facade *facade = runtime == nullptr || runtime->base.vtab != &runtime_facade_vtab ? nullptr : runtime->base.impl;
    sc_agent_turn_result result = {0};
    sc_turn turn = {0};
    sc_status status;

    if (facade == nullptr || message == nullptr || out == nullptr || message->input.ptr == nullptr || message->input.len == 0) {
        return sc_status_invalid_argument("sc.runtime_process.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_runtime_response){.struct_size = sizeof(*out)};
    out->turn_id = facade->next_turn_id++;
    sc_cancel_token_clear(&facade->active_cancel);
    status = sc_cancel_token_init(&facade->active_cancel, facade->alloc);
    facade->active = sc_status_is_ok(status);
    facade->active_turn_id = out->turn_id;
    turn = (sc_turn){
        .struct_size = sizeof(turn),
        .input = message->input,
        .session_id = message->session_id,
        .media = message->media,
        .media_count = message->media_count,
        .media_context = message->media_context,
        .allowed_tools = message->allowed_tools,
        .allowed_tool_count = message->allowed_tool_count,
        .max_total_tokens = message->max_total_tokens,
        .max_cost_usd = message->max_cost_usd,
        .model_switch = message->model_switch,
        .turn_id = out->turn_id,
        .timeout_ms = message->timeout_ms,
        .use_streaming = message->use_streaming,
        .emit_stream_deltas = message->emit_stream_deltas,
        .cancel_token = &facade->active_cancel,
    };
    if (sc_status_is_ok(status)) {
        status = sc_agent_process_message(facade->agent, &turn, alloc, &result);
    }
    facade->active = false;
    facade->active_turn_id = 0;

    out->output = result.output;
    result.output = (sc_string){0};
    out->receipts = result.receipts;
    result.receipts = (sc_receipt_chain){0};
    out->events = result.events;
    result.events = (sc_vec){0};
    out->provider_call_count = result.provider_call_count;
    out->tool_call_count = result.tool_call_count;
    out->input_tokens = result.input_tokens;
    out->output_tokens = result.output_tokens;
    out->total_tokens = result.total_tokens;
    out->cost_usd = result.cost_usd;
    out->budget_exceeded = result.budget_exceeded;
    out->model_switched = result.model_switched;
    out->active_model = result.active_model;
    result.active_model = (sc_string){0};
    out->cancelled = result.cancelled;
    out->timed_out = result.timed_out;
    out->attachment_content_type = result.attachment_content_type;
    result.attachment_content_type = (sc_string){0};
    out->attachment_filename = result.attachment_filename;
    result.attachment_filename = (sc_string){0};
    out->attachment_bytes = result.attachment_bytes;
    result.attachment_bytes = (sc_bytes){0};
    sc_agent_turn_result_clear(&result);
    return status;
}

sc_status sc_runtime_cancel(sc_runtime *runtime, sc_runtime_turn_id turn_id)
{
    runtime_facade *facade = runtime == nullptr || runtime->base.vtab != &runtime_facade_vtab ? nullptr : runtime->base.impl;

    if (facade == nullptr || turn_id == 0) {
        return sc_status_invalid_argument("sc.runtime_cancel.invalid_argument");
    }
    if (!facade->active || facade->active_turn_id != turn_id) {
        return sc_status_invalid_argument("sc.runtime_cancel.turn_not_active");
    }
    return sc_cancel_token_cancel(&facade->active_cancel, facade->alloc, sc_str_from_cstr("runtime_cancel"));
}

static sc_status runtime_facade_run_turn(void *impl,
                                         const sc_runtime_turn *turn,
                                         sc_allocator *alloc,
                                         sc_runtime_turn_result *out)
{
    runtime_facade *facade = impl;
    sc_agent_turn_result result = {0};
    sc_turn agent_turn = {0};
    sc_status status;

    if (facade == nullptr || turn == nullptr || out == nullptr || turn->input.ptr == nullptr || turn->input.len == 0) {
        return sc_status_invalid_argument("sc.runtime_facade.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    agent_turn = (sc_turn){
        .struct_size = sizeof(agent_turn),
        .input = turn->input,
    };
    status = sc_agent_process_message(facade->agent, &agent_turn, alloc, &result);
    if (sc_status_is_ok(status)) {
        out->struct_size = sizeof(*out);
        out->output = result.output;
        result.output = (sc_string){0};
    }
    sc_agent_turn_result_clear(&result);
    return status;
}

static void runtime_facade_destroy(void *impl)
{
    runtime_facade *facade = impl;
    if (facade == nullptr) {
        return;
    }
    sc_cancel_token_clear(&facade->active_cancel);
    sc_agent_destroy(facade->agent);
    sc_free(facade->alloc, facade, sizeof(*facade), _Alignof(runtime_facade));
}

bool sc_peripheral_vtab_valid(const sc_peripheral_vtab *vtab)
{
    const size_t required_size = offsetof(sc_peripheral_vtab, device_class);
    return vtab != nullptr &&
           sc_contract_common_vtab_valid(vtab->struct_size, vtab->abi_major, vtab->name, vtab->command != nullptr, vtab->destroy != nullptr) &&
           vtab->struct_size >= required_size;
}

sc_status sc_peripheral_new(sc_allocator *alloc,
                            const sc_peripheral_vtab *vtab,
                            void *impl,
                            sc_peripheral **out)
{
    if (out == nullptr || !sc_peripheral_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.peripheral.invalid_vtab");
    }
    return sc_contract_handle_new(alloc, vtab, impl, sizeof(sc_peripheral), (void **)out);
}

sc_status sc_peripheral_command_send(sc_peripheral *peripheral,
                                     const sc_peripheral_command *command,
                                     sc_allocator *alloc,
                                     sc_peripheral_result *out)
{
    const sc_peripheral_vtab *vtab = peripheral == nullptr ? nullptr : peripheral->base.vtab;
    if (peripheral == nullptr || command == nullptr || out == nullptr || vtab == nullptr || vtab->command == nullptr) {
        return sc_status_invalid_argument("sc.peripheral.invalid_argument");
    }
    return vtab->command(peripheral->base.impl, command, alloc, out);
}

sc_status sc_peripheral_health_check(sc_peripheral *peripheral, sc_allocator *alloc, sc_peripheral_health *out)
{
    const sc_peripheral_vtab *vtab = peripheral == nullptr ? nullptr : peripheral->base.vtab;
    if (peripheral == nullptr || out == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.peripheral.invalid_argument");
    }
    if (vtab->health == nullptr) {
        *out = (sc_peripheral_health){.struct_size = sizeof(*out), .healthy = true};
        return sc_string_from_cstr(alloc, "ok", &out->message);
    }
    return vtab->health(peripheral->base.impl, alloc, out);
}

sc_status sc_peripheral_describe_context(sc_peripheral *peripheral, sc_allocator *alloc, sc_peripheral_context *out)
{
    const sc_peripheral_vtab *vtab = peripheral == nullptr ? nullptr : peripheral->base.vtab;
    if (peripheral == nullptr || out == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.peripheral.invalid_argument");
    }
    if (vtab->describe_context == nullptr) {
        *out = (sc_peripheral_context){.struct_size = sizeof(*out)};
        return sc_string_from_cstr(alloc, "", &out->text);
    }
    return vtab->describe_context(peripheral->base.impl, alloc, out);
}

sc_status sc_peripheral_safe_shutdown(sc_peripheral *peripheral)
{
    const sc_peripheral_vtab *vtab = peripheral == nullptr ? nullptr : peripheral->base.vtab;
    const size_t safe_shutdown_size = offsetof(sc_peripheral_vtab, safe_shutdown) + sizeof(vtab->safe_shutdown);
    if (peripheral == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.peripheral.invalid_argument");
    }
    if (vtab->struct_size < safe_shutdown_size || vtab->safe_shutdown == nullptr) {
        return sc_status_unsupported("sc.peripheral.safe_shutdown.unsupported");
    }
    return vtab->safe_shutdown(peripheral->base.impl);
}

const sc_peripheral_vtab *sc_peripheral_vtab_of(const sc_peripheral *peripheral)
{
    return peripheral == nullptr ? nullptr : peripheral->base.vtab;
}

void sc_peripheral_destroy(sc_peripheral *peripheral)
{
    const sc_peripheral_vtab *vtab = peripheral == nullptr ? nullptr : peripheral->base.vtab;
    sc_contract_handle_destroy(peripheral == nullptr ? nullptr : &peripheral->base, sizeof(*peripheral), vtab == nullptr ? nullptr : vtab->destroy);
}

bool sc_sandbox_vtab_valid(const sc_sandbox_vtab *vtab)
{
    return vtab != nullptr &&
           sc_contract_common_vtab_valid(vtab->struct_size, vtab->abi_major, vtab->name, vtab->check != nullptr, vtab->destroy != nullptr) &&
           vtab->struct_size >= sizeof(*vtab);
}

sc_status sc_sandbox_new(sc_allocator *alloc, const sc_sandbox_vtab *vtab, void *impl, sc_sandbox **out)
{
    if (out == nullptr || !sc_sandbox_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.sandbox.invalid_vtab");
    }
    return sc_contract_handle_new(alloc, vtab, impl, sizeof(sc_sandbox), (void **)out);
}

sc_status sc_sandbox_check(sc_sandbox *sandbox,
                           const sc_sandbox_request *request,
                           sc_allocator *alloc,
                           sc_sandbox_decision *out)
{
    const sc_sandbox_vtab *vtab = sandbox == nullptr ? nullptr : sandbox->base.vtab;
    if (sandbox == nullptr || request == nullptr || out == nullptr || vtab == nullptr || vtab->check == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.invalid_argument");
    }
    return vtab->check(sandbox->base.impl, request, alloc, out);
}

const sc_sandbox_vtab *sc_sandbox_vtab_of(const sc_sandbox *sandbox)
{
    return sandbox == nullptr ? nullptr : sandbox->base.vtab;
}

void sc_sandbox_destroy(sc_sandbox *sandbox)
{
    const sc_sandbox_vtab *vtab = sandbox == nullptr ? nullptr : sandbox->base.vtab;
    sc_contract_handle_destroy(sandbox == nullptr ? nullptr : &sandbox->base, sizeof(*sandbox), vtab == nullptr ? nullptr : vtab->destroy);
}
