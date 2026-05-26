#include "tools/tool_internal.h"
#include "sc/time.h"

typedef enum tool_wrapper_kind {
    TOOL_WRAPPER_RATE_LIMIT = 0,
    TOOL_WRAPPER_DOMAIN_GUARD,
    TOOL_WRAPPER_TIMEOUT
} tool_wrapper_kind;

typedef struct tool_wrapper {
    sc_allocator *alloc;
    sc_tool *inner;
    const sc_security_policy *policy;
    tool_wrapper_kind kind;
    size_t max_calls;
    size_t call_count;
    int64_t timeout_ms;
} tool_wrapper;

static sc_status wrapper_spec(void *impl, sc_tool_spec *out);
static sc_status wrapper_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void wrapper_destroy(void *impl);
static sc_status wrapper_new(sc_allocator *alloc, sc_tool *inner, tool_wrapper_kind kind, tool_wrapper **wrapper_out, sc_tool **out);

static const sc_tool_vtab wrapper_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "tool_wrapper",
    .display_name = "Tool wrapper",
    .feature_flag = "SC_TOOL_WRAPPER",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = wrapper_spec,
    .invoke = wrapper_invoke,
    .destroy = wrapper_destroy,
};

sc_status sc_tool_rate_limit_wrapper_new(sc_allocator *alloc, sc_tool *inner, size_t max_calls, sc_tool **out)
{
    tool_wrapper *wrapper = nullptr;
    sc_status status = wrapper_new(alloc, inner, TOOL_WRAPPER_RATE_LIMIT, &wrapper, out);
    if (sc_status_is_ok(status) && wrapper != nullptr) {
        wrapper->max_calls = max_calls;
    }
    return status;
}

sc_status sc_tool_domain_guard_wrapper_new(sc_allocator *alloc,
                                           sc_tool *inner,
                                           const sc_security_policy *policy,
                                           sc_tool **out)
{
    tool_wrapper *wrapper = nullptr;
    sc_status status = wrapper_new(alloc, inner, TOOL_WRAPPER_DOMAIN_GUARD, &wrapper, out);
    if (sc_status_is_ok(status) && wrapper != nullptr) {
        wrapper->policy = policy;
    }
    return status;
}

sc_status sc_tool_timeout_wrapper_new(sc_allocator *alloc,
                                      sc_tool *inner,
                                      int64_t timeout_ms,
                                      sc_tool **out)
{
    tool_wrapper *wrapper = nullptr;
    sc_status status = wrapper_new(alloc, inner, TOOL_WRAPPER_TIMEOUT, &wrapper, out);
    if (sc_status_is_ok(status) && wrapper != nullptr) {
        wrapper->timeout_ms = timeout_ms;
    }
    return status;
}

static sc_status wrapper_spec(void *impl, sc_tool_spec *out)
{
    tool_wrapper *wrapper = impl;
    if (wrapper == nullptr || wrapper->inner == nullptr) {
        return sc_status_invalid_argument("sc.tool_wrapper.invalid_argument");
    }
    return sc_tool_spec_get(wrapper->inner, out);
}

static sc_status wrapper_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    tool_wrapper *wrapper = impl;
    sc_str url = {0};
    sc_status status;
    sc_instant started = {0};
    sc_instant ended = {0};

    if (wrapper == nullptr || wrapper->inner == nullptr) {
        return sc_status_invalid_argument("sc.tool_wrapper.invalid_argument");
    }
    if (wrapper->kind == TOOL_WRAPPER_RATE_LIMIT) {
        if (wrapper->max_calls == 0 || wrapper->call_count >= wrapper->max_calls) {
            return sc_status_timeout("sc.tool.rate_limited");
        }
        wrapper->call_count += 1;
    } else if (wrapper->kind == TOOL_WRAPPER_DOMAIN_GUARD) {
        if (wrapper->policy == nullptr) {
            return sc_status_invalid_argument("sc.tool.domain_guard_policy_missing");
        }
        status = sc_tool_get_optional_string_arg(call, sc_str_from_cstr("url"), &url);
        if (!sc_status_is_ok(status)) {
            return status;
        }
        if (url.len > 0) {
            status = sc_security_validate_url(wrapper->policy, url);
            if (!sc_status_is_ok(status)) {
                return status;
            }
        }
    } else if (wrapper->kind == TOOL_WRAPPER_TIMEOUT) {
        if (wrapper->timeout_ms <= 0) {
            return sc_status_timeout("sc.tool.timeout");
        }
        status = sc_clock_monotonic(&started);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    status = sc_tool_invoke(wrapper->inner, call, alloc, out);
    if (wrapper->kind == TOOL_WRAPPER_TIMEOUT && sc_status_is_ok(sc_clock_monotonic(&ended)) &&
        ended.ns - started.ns > wrapper->timeout_ms * 1'000'000) {
        sc_tool_result_clear(out);
        return sc_status_timeout("sc.tool.timeout");
    }
    return status;
}

static void wrapper_destroy(void *impl)
{
    tool_wrapper *wrapper = impl;
    if (wrapper == nullptr) {
        return;
    }
    sc_tool_destroy(wrapper->inner);
    sc_free(wrapper->alloc, wrapper, sizeof(*wrapper), _Alignof(tool_wrapper));
}

static sc_status wrapper_new(sc_allocator *alloc, sc_tool *inner, tool_wrapper_kind kind, tool_wrapper **wrapper_out, sc_tool **out)
{
    tool_wrapper *wrapper = nullptr;
    sc_status status;

    if (out == nullptr || wrapper_out == nullptr || inner == nullptr) {
        return sc_status_invalid_argument("sc.tool_wrapper.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    wrapper = sc_alloc(alloc, sizeof(*wrapper), _Alignof(tool_wrapper));
    if (wrapper == nullptr) {
        return sc_status_no_memory();
    }
    *wrapper = (tool_wrapper){.alloc = alloc, .inner = inner, .kind = kind};
    status = sc_tool_new(alloc, &wrapper_vtab, wrapper, out);
    if (!sc_status_is_ok(status)) {
        wrapper_destroy(wrapper);
    } else {
        *wrapper_out = wrapper;
    }
    return status;
}
