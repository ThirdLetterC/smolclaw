#include "tools/tool_internal.h"

#include <string.h>

typedef enum memory_tool_kind {
    MEMORY_STORE = 0,
    MEMORY_RECALL,
    MEMORY_SEARCH,
    MEMORY_PIN,
    MEMORY_FORGET,
    MEMORY_EXPORT,
    MEMORY_PURGE
} memory_tool_kind;

typedef struct memory_tool {
    sc_tool_impl_context base;
    memory_tool_kind kind;
} memory_tool;

static sc_status memory_tool_spec(void *impl, sc_tool_spec *out);
static sc_status memory_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void memory_tool_destroy(void *impl);
static sc_status memory_tool_new(sc_allocator *alloc,
                                 const sc_tool_context *context,
                                 memory_tool_kind kind,
                                 const char *name,
                                 sc_tool **out);
static sc_str memory_tool_name(memory_tool_kind kind);
static sc_tool_risk memory_tool_risk(memory_tool_kind kind);
static sc_status build_schema(sc_allocator *alloc, memory_tool_kind kind, sc_json_value **out);
static sc_status invoke_store(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_recall(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_search(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_pin(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_forget(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_export(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_purge(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static bool memory_backend_is_none(const memory_tool *tool);
static sc_status memory_require_enabled_backend(const memory_tool *tool);
static bool memory_status_is_not_found(sc_status status);
static sc_status memory_set_location_output(sc_allocator *alloc,
                                            const sc_tool_impl_context *context,
                                            sc_tool_result *out,
                                            sc_str verb,
                                            sc_str namespace_name,
                                            sc_str key);

static const sc_tool_vtab memory_store_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "memory_store",
    .display_name = "Memory store",
    .feature_flag = "SC_TOOL_MEMORY_STORE",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = memory_tool_spec,
    .invoke = memory_tool_invoke,
    .destroy = memory_tool_destroy,
};

static const sc_tool_vtab memory_recall_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "memory_recall",
    .display_name = "Memory recall",
    .feature_flag = "SC_TOOL_MEMORY_RECALL",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = memory_tool_spec,
    .invoke = memory_tool_invoke,
    .destroy = memory_tool_destroy,
};

static const sc_tool_vtab memory_forget_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "memory_forget",
    .display_name = "Memory forget",
    .feature_flag = "SC_TOOL_MEMORY_FORGET",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = memory_tool_spec,
    .invoke = memory_tool_invoke,
    .destroy = memory_tool_destroy,
};

static const sc_tool_vtab memory_search_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "memory_search",
    .display_name = "Memory search",
    .feature_flag = "SC_TOOL_MEMORY_SEARCH",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = memory_tool_spec,
    .invoke = memory_tool_invoke,
    .destroy = memory_tool_destroy,
};

static const sc_tool_vtab memory_pin_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "memory_pin",
    .display_name = "Memory pin",
    .feature_flag = "SC_TOOL_MEMORY_PIN",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = memory_tool_spec,
    .invoke = memory_tool_invoke,
    .destroy = memory_tool_destroy,
};

static const sc_tool_vtab memory_export_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "memory_export",
    .display_name = "Memory export",
    .feature_flag = "SC_TOOL_MEMORY_EXPORT",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = memory_tool_spec,
    .invoke = memory_tool_invoke,
    .destroy = memory_tool_destroy,
};

static const sc_tool_vtab memory_purge_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "memory_purge",
    .display_name = "Memory purge",
    .feature_flag = "SC_TOOL_MEMORY_PURGE",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = memory_tool_spec,
    .invoke = memory_tool_invoke,
    .destroy = memory_tool_destroy,
};

sc_status sc_tool_memory_store_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return memory_tool_new(alloc, context, MEMORY_STORE, memory_store_vtab.name, out);
}

sc_status sc_tool_memory_recall_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return memory_tool_new(alloc, context, MEMORY_RECALL, memory_recall_vtab.name, out);
}

sc_status sc_tool_memory_search_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return memory_tool_new(alloc, context, MEMORY_SEARCH, memory_search_vtab.name, out);
}

sc_status sc_tool_memory_pin_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return memory_tool_new(alloc, context, MEMORY_PIN, memory_pin_vtab.name, out);
}

sc_status sc_tool_memory_forget_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return memory_tool_new(alloc, context, MEMORY_FORGET, memory_forget_vtab.name, out);
}

sc_status sc_tool_memory_export_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return memory_tool_new(alloc, context, MEMORY_EXPORT, memory_export_vtab.name, out);
}

sc_status sc_tool_memory_purge_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return memory_tool_new(alloc, context, MEMORY_PURGE, memory_purge_vtab.name, out);
}

static sc_status memory_tool_spec(void *impl, sc_tool_spec *out)
{
    const memory_tool *tool = impl;
    sc_str name = {0};
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.memory_tool.invalid_argument");
    }
    name = memory_tool_name(tool->kind);
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = name,
        .description = sc_str_from_cstr("tool.memory.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = memory_tool_risk(tool->kind),
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_MEMORY,
        .side_effect = tool->kind == MEMORY_RECALL || tool->kind == MEMORY_SEARCH || tool->kind == MEMORY_EXPORT ? SC_TOOL_SIDE_EFFECT_READ :
            (tool->kind == MEMORY_PURGE ? SC_TOOL_SIDE_EFFECT_DESTRUCTIVE : SC_TOOL_SIDE_EFFECT_WRITE),
        .default_autonomy = memory_tool_risk(tool->kind) == SC_TOOL_RISK_READONLY ? SC_AUTONOMY_AUTONOMOUS : SC_AUTONOMY_SUPERVISED,
        .catalog_metadata_key = sc_str_from_cstr("tool.memory.catalog"),
    };
    return sc_status_ok();
}

static sc_status memory_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    memory_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.memory_tool.invalid_argument");
    }
    if (tool->base.context.memory == nullptr) {
        return sc_status_invalid_argument("sc.memory_tool.no_memory");
    }
    switch (tool->kind) {
    case MEMORY_STORE:
        return invoke_store(tool, call, alloc, out);
    case MEMORY_RECALL:
        return invoke_recall(tool, call, alloc, out);
    case MEMORY_SEARCH:
        return invoke_search(tool, call, alloc, out);
    case MEMORY_PIN:
        return invoke_pin(tool, call, alloc, out);
    case MEMORY_FORGET:
        return invoke_forget(tool, call, alloc, out);
    case MEMORY_EXPORT:
        return invoke_export(tool, call, alloc, out);
    case MEMORY_PURGE:
        return invoke_purge(tool, call, alloc, out);
    }
    return sc_status_invalid_argument("sc.memory_tool.unknown");
}

static void memory_tool_destroy(void *impl)
{
    memory_tool *tool = impl;
    sc_allocator *alloc = nullptr;
    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc;
    sc_tool_impl_context_clear(&tool->base);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(memory_tool));
}

static sc_status memory_tool_new(sc_allocator *alloc,
                                 const sc_tool_context *context,
                                 memory_tool_kind kind,
                                 const char *name,
                                 sc_tool **out)
{
    memory_tool *tool = nullptr;
    const sc_tool_vtab *vtab = nullptr;
    sc_status status;

    if (out == nullptr || name == nullptr) {
        return sc_status_invalid_argument("sc.memory_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(memory_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (memory_tool){.kind = kind};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = build_schema(alloc, kind, &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (kind == MEMORY_STORE) {
        vtab = &memory_store_vtab;
    } else if (kind == MEMORY_RECALL) {
        vtab = &memory_recall_vtab;
    } else if (kind == MEMORY_SEARCH) {
        vtab = &memory_search_vtab;
    } else if (kind == MEMORY_PIN) {
        vtab = &memory_pin_vtab;
    } else if (kind == MEMORY_FORGET) {
        vtab = &memory_forget_vtab;
    } else if (kind == MEMORY_EXPORT) {
        vtab = &memory_export_vtab;
    } else {
        vtab = &memory_purge_vtab;
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        memory_tool_destroy(tool);
    }
    return status;
}

static sc_str memory_tool_name(memory_tool_kind kind)
{
    switch (kind) {
    case MEMORY_STORE:
        return sc_str_from_cstr("memory_store");
    case MEMORY_RECALL:
        return sc_str_from_cstr("memory_recall");
    case MEMORY_SEARCH:
        return sc_str_from_cstr("memory_search");
    case MEMORY_PIN:
        return sc_str_from_cstr("memory_pin");
    case MEMORY_FORGET:
        return sc_str_from_cstr("memory_forget");
    case MEMORY_EXPORT:
        return sc_str_from_cstr("memory_export");
    case MEMORY_PURGE:
        return sc_str_from_cstr("memory_purge");
    }
    return sc_str_from_cstr("memory");
}

static sc_tool_risk memory_tool_risk(memory_tool_kind kind)
{
    return kind == MEMORY_RECALL || kind == MEMORY_SEARCH || kind == MEMORY_EXPORT ? SC_TOOL_RISK_READONLY : SC_TOOL_RISK_SIDE_EFFECT;
}

static sc_status build_schema(sc_allocator *alloc, memory_tool_kind kind, sc_json_value **out)
{
    if (kind == MEMORY_STORE || kind == MEMORY_PIN) {
        return sc_tool_schema_three_strings(alloc,
                                            sc_str_from_cstr("namespace"),
                                            true,
                                            sc_str_from_cstr("key"),
                                            true,
                                            sc_str_from_cstr("content"),
                                            true,
                                            out);
    }
    if (kind == MEMORY_SEARCH) {
        return sc_tool_schema_three_strings(alloc,
                                            sc_str_from_cstr("namespace"),
                                            true,
                                            sc_str_from_cstr("query"),
                                            false,
                                            sc_str_from_cstr("limit"),
                                            false,
                                            out);
    }
    if (kind == MEMORY_EXPORT || kind == MEMORY_PURGE) {
        return sc_tool_schema_string_required(alloc,
                                              sc_str_from_cstr("namespace"),
                                              sc_str_from_cstr("tool.memory.description"),
                                              out);
    }
    return sc_tool_schema_two_strings(alloc,
                                      sc_str_from_cstr("namespace"),
                                      true,
                                      sc_str_from_cstr("key"),
                                      true,
                                      out);
}

static sc_status invoke_store(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str namespace_name = {0};
    sc_str key = {0};
    sc_str content = {0};
    sc_memory_record record = {.struct_size = sizeof(record)};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("namespace"), &namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("key"), &key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("content"), &content);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("memory_store"),
                                        SC_TOOL_RISK_SIDE_EFFECT,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        status = memory_require_enabled_backend(tool);
    }
    if (sc_status_is_ok(status)) {
        record.namespace_name = namespace_name;
        record.key = key;
        record.value = content;
        status = sc_memory_put(tool->base.context.memory, &record);
    }
    if (sc_status_is_ok(status)) {
        status = memory_set_location_output(alloc,
                                            &tool->base,
                                            out,
                                            sc_str_from_cstr("stored"),
                                            namespace_name,
                                            key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("memory_store"), key, sc_str_from_cstr("stored"), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("memory_store"),
                                            key,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    return status;
}

static sc_status invoke_recall(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str namespace_name = {0};
    sc_str key = {0};
    sc_string value = {0};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("namespace"), &namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("key"), &key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("memory_recall"),
                                        SC_TOOL_RISK_READONLY,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        status = memory_require_enabled_backend(tool);
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_get(tool->base.context.memory, namespace_name, key, alloc, &value);
    }
    if (memory_status_is_not_found(status)) {
        sc_status_clear(&status);
        status = memory_set_location_output(alloc,
                                            &tool->base,
                                            out,
                                            sc_str_from_cstr("not_found"),
                                            namespace_name,
                                            key);
        goto receipt;
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&value), true);
    }
receipt:
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("memory_recall"), key, sc_string_as_str(&out->output), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("memory_recall"),
                                            key,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    sc_string_clear(&value);
    return status;
}

static sc_status invoke_search(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str namespace_name = {0};
    sc_str query_text = {0};
    sc_str limit_text = {0};
    sc_string snapshot = {0};
    sc_memory_query query = {.struct_size = sizeof(query), .limit = 10};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("namespace"), &namespace_name);
    }
    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("query"), &query_text);
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("limit"), &limit_text);
        if (limit_text.len > 0) {
            double parsed = 0;
            sc_json_value *value = sc_json_object_get(call->args, sc_str_from_cstr("limit"));
            if (sc_json_as_number(value, &parsed) && parsed > 0) {
                query.limit = (size_t)parsed;
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("memory_search"),
                                        SC_TOOL_RISK_READONLY,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        query.namespace_name = namespace_name;
        query.query = query_text;
        status = sc_memory_export_snapshot(tool->base.context.memory, &query, alloc, &snapshot);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&snapshot), true);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("memory_search"), namespace_name, sc_string_as_str(&out->output), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("memory_search"),
                                            namespace_name,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    sc_string_clear(&snapshot);
    return status;
}

static sc_status invoke_pin(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str namespace_name = {0};
    sc_str key = {0};
    sc_str content = {0};
    sc_memory_record record = {.struct_size = sizeof(record)};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("namespace"), &namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("key"), &key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("content"), &content);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("memory_pin"),
                                        SC_TOOL_RISK_SIDE_EFFECT,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status) && memory_backend_is_none(tool)) {
        status = memory_set_location_output(alloc,
                                            &tool->base,
                                            out,
                                            sc_str_from_cstr("pinned"),
                                            namespace_name,
                                            key);
        goto receipt;
    }
    if (sc_status_is_ok(status)) {
        status = memory_require_enabled_backend(tool);
    }
    if (sc_status_is_ok(status)) {
        record.namespace_name = namespace_name;
        record.key = key;
        record.value = content;
        record.category = sc_str_from_cstr("pinned");
        record.retention_policy = SC_MEMORY_RETENTION_PERSISTENT;
        record.metadata_json = sc_str_from_cstr("{\"pinned\":true}");
        status = sc_memory_put(tool->base.context.memory, &record);
    }
    if (sc_status_is_ok(status)) {
        status = memory_set_location_output(alloc,
                                            &tool->base,
                                            out,
                                            sc_str_from_cstr("pinned"),
                                            namespace_name,
                                            key);
    }
receipt:
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("memory_pin"), key, sc_str_from_cstr("pinned"), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("memory_pin"),
                                            key,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    return status;
}

static bool memory_status_is_not_found(sc_status status)
{
    return status.error_key != nullptr &&
           (strcmp(status.error_key, "sc.memory_store.not_found") == 0 ||
            strcmp(status.error_key, "sc.memory_none.not_found") == 0 ||
            strcmp(status.error_key, "sc.memory_sqlite.not_found") == 0);
}

static bool memory_backend_is_none(const memory_tool *tool)
{
    const sc_memory_vtab *vtab = tool == nullptr ? nullptr : sc_memory_vtab_of(tool->base.context.memory);

    return vtab == nullptr || vtab->name == nullptr || strcmp(vtab->name, "none") == 0;
}

static sc_status memory_require_enabled_backend(const memory_tool *tool)
{
    if (memory_backend_is_none(tool)) {
        return sc_status_unsupported("sc.memory_tool.backend_disabled");
    }
    return sc_status_ok();
}

static sc_status memory_set_location_output(sc_allocator *alloc,
                                            const sc_tool_impl_context *context,
                                            sc_tool_result *out,
                                            sc_str verb,
                                            sc_str namespace_name,
                                            sc_str key)
{
    sc_string text = {0};
    sc_string_builder builder = {0};
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, verb);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, " namespace=");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, " key=");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &text);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, context, out, sc_string_as_str(&text), true);
    }
    sc_string_clear(&text);
    return status;
}

static sc_status invoke_forget(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str namespace_name = {0};
    sc_str key = {0};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("namespace"), &namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("key"), &key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("memory_forget"),
                                        SC_TOOL_RISK_SIDE_EFFECT,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        status = memory_require_enabled_backend(tool);
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_forget(tool->base.context.memory, namespace_name, key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_str_from_cstr("forgot"), true);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("memory_forget"), key, sc_str_from_cstr("forgot"), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("memory_forget"),
                                            key,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    return status;
}

static sc_status invoke_export(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str namespace_name = {0};
    sc_string snapshot = {0};
    sc_memory_query query = {.struct_size = sizeof(query)};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("namespace"), &namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("memory_export"),
                                        SC_TOOL_RISK_READONLY,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        query.namespace_name = namespace_name;
        status = sc_memory_export_snapshot(tool->base.context.memory, &query, alloc, &snapshot);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&snapshot), true);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("memory_export"), namespace_name, sc_string_as_str(&out->output), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("memory_export"),
                                            namespace_name,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    sc_string_clear(&snapshot);
    return status;
}

static sc_status invoke_purge(memory_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str namespace_name = {0};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("namespace"), &namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("memory_purge"),
                                        SC_TOOL_RISK_SIDE_EFFECT,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        status = memory_require_enabled_backend(tool);
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_purge_namespace(tool->base.context.memory, namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_str_from_cstr("purged"), true);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("memory_purge"), namespace_name, sc_str_from_cstr("purged"), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("memory_purge"),
                                            namespace_name,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    return status;
}
