#include "tools/tool_internal.h"

#include "sc/time.h"

#include <stdio.h>
#include <time.h>

typedef struct time_tool {
    sc_tool_impl_context base;
} time_tool;

static sc_status time_spec(void *impl, sc_tool_spec *out);
static sc_status time_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void time_destroy(void *impl);
static bool timezone_is_utc(sc_str timezone);

static const sc_tool_vtab time_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "time",
    .display_name = "Time",
    .feature_flag = "SC_TOOL_TIME",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = time_spec,
    .invoke = time_invoke,
    .destroy = time_destroy,
};

sc_status sc_tool_time_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    time_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.time_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(time_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (time_tool){0};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = sc_tool_schema_string(alloc, sc_str_from_cstr("timezone"), false, &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, &time_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        time_destroy(tool);
    }
    return status;
}

static sc_status time_spec(void *impl, sc_tool_spec *out)
{
    const time_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.time_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_str_from_cstr("time"),
        .description = sc_str_from_cstr("tool.time.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = SC_TOOL_RISK_READONLY,
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_NONE,
        .side_effect = SC_TOOL_SIDE_EFFECT_READ,
        .default_autonomy = SC_AUTONOMY_AUTONOMOUS,
        .catalog_metadata_key = sc_str_from_cstr("tool.time.catalog"),
    };
    return sc_status_ok();
}

static sc_status time_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    time_tool *tool = impl;
    sc_str timezone_name = sc_str_from_cstr("UTC");
    time_t now;
    struct tm tm_utc = {0};
    char text[128] = {0};
    sc_status status;

    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.time_tool.invalid_argument");
    }
    status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("timezone"), &timezone_name);
        if (timezone_name.len == 0) {
            timezone_name = sc_str_from_cstr("UTC");
        }
        if (!timezone_is_utc(timezone_name)) {
            status = sc_status_unsupported("sc.time_tool.timezone_unsupported");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("time"),
                                        SC_TOOL_RISK_READONLY,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        now = time(nullptr);
        if (gmtime_r(&now, &tm_utc) == nullptr) {
            status = sc_status_io("sc.time_tool.clock_failed");
        }
    }
    if (sc_status_is_ok(status)) {
        int written = snprintf(text,
                               sizeof(text),
                               "%04d-%02d-%02dT%02d:%02d:%02dZ timezone=%.*s",
                               tm_utc.tm_year + 1900,
                               tm_utc.tm_mon + 1,
                               tm_utc.tm_mday,
                               tm_utc.tm_hour,
                               tm_utc.tm_min,
                               tm_utc.tm_sec,
                               (int)timezone_name.len,
                               timezone_name.ptr);
        if (written < 0 || (size_t)written >= sizeof(text)) {
            status = sc_status_invalid_argument("sc.time_tool.output_too_large");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_str_from_cstr(text), true);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("time"), timezone_name, sc_string_as_str(&out->output), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("time"),
                                            timezone_name,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    return status;
}

static void time_destroy(void *impl)
{
    time_tool *tool = impl;
    sc_allocator *alloc = nullptr;
    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc;
    sc_tool_impl_context_clear(&tool->base);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(time_tool));
}

static bool timezone_is_utc(sc_str timezone_name)
{
    return sc_str_equal(timezone_name, sc_str_from_cstr("UTC")) ||
           sc_str_equal(timezone_name, sc_str_from_cstr("Etc/UTC")) ||
           sc_str_equal(timezone_name, sc_str_from_cstr("Z")) ||
           sc_str_equal(timezone_name, sc_str_from_cstr("+00:00")) ||
           sc_str_equal(timezone_name, sc_str_from_cstr("-00:00"));
}
