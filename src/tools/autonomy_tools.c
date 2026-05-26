#include "tools/tool_internal.h"

#include "sc/autonomy.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum autonomy_tool_kind {
    AUTONOMY_TOOL_SOP_INSPECT = 0,
    AUTONOMY_TOOL_SOP_ADVANCE,
    AUTONOMY_TOOL_CRON_LIST,
    AUTONOMY_TOOL_CRON_UPSERT,
    AUTONOMY_TOOL_CRON_REMOVE
} autonomy_tool_kind;

typedef struct autonomy_tool {
    sc_tool_impl_context base;
    autonomy_tool_kind kind;
} autonomy_tool;

static sc_status autonomy_tool_spec(void *impl, sc_tool_spec *out);
static sc_status autonomy_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void autonomy_tool_destroy(void *impl);
static sc_status autonomy_tool_new(sc_allocator *alloc,
                                   const sc_tool_context *context,
                                   autonomy_tool_kind kind,
                                   const sc_tool_vtab *vtab,
                                   sc_tool **out);
static sc_status build_schema(sc_allocator *alloc, autonomy_tool_kind kind, sc_json_value **out);
static sc_str tool_name(autonomy_tool_kind kind);
static sc_str tool_description(autonomy_tool_kind kind);
static sc_str tool_catalog(autonomy_tool_kind kind);
static sc_tool_risk tool_risk(autonomy_tool_kind kind);
static sc_tool_side_effect tool_side_effect(autonomy_tool_kind kind);
static sc_status invoke_sop_inspect(autonomy_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_sop_advance(autonomy_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_cron_list(autonomy_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_cron_upsert(autonomy_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_cron_remove(autonomy_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status append_sop_summary(sc_string_builder *builder, const sc_sop_document *document);
static sc_status append_cron_job(sc_string_builder *builder, const sc_cron_job *job);
static sc_status string_set(sc_allocator *alloc, sc_string *out, sc_str value);

static const sc_tool_vtab sop_inspect_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "sop_inspect",
    .display_name = "SOP inspect",
    .feature_flag = "SC_TOOL_SOP",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = autonomy_tool_spec,
    .invoke = autonomy_tool_invoke,
    .destroy = autonomy_tool_destroy,
};

static const sc_tool_vtab sop_advance_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "sop_advance",
    .display_name = "SOP advance",
    .feature_flag = "SC_TOOL_SOP",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = autonomy_tool_spec,
    .invoke = autonomy_tool_invoke,
    .destroy = autonomy_tool_destroy,
};

static const sc_tool_vtab cron_list_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "cron_list",
    .display_name = "Cron list",
    .feature_flag = "SC_TOOL_CRON",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = autonomy_tool_spec,
    .invoke = autonomy_tool_invoke,
    .destroy = autonomy_tool_destroy,
};

static const sc_tool_vtab cron_upsert_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "cron_upsert",
    .display_name = "Cron upsert",
    .feature_flag = "SC_TOOL_CRON",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = autonomy_tool_spec,
    .invoke = autonomy_tool_invoke,
    .destroy = autonomy_tool_destroy,
};

static const sc_tool_vtab cron_remove_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "cron_remove",
    .display_name = "Cron remove",
    .feature_flag = "SC_TOOL_CRON",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = autonomy_tool_spec,
    .invoke = autonomy_tool_invoke,
    .destroy = autonomy_tool_destroy,
};

sc_status sc_tool_sop_inspect_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return autonomy_tool_new(alloc, context, AUTONOMY_TOOL_SOP_INSPECT, &sop_inspect_vtab, out);
}

sc_status sc_tool_sop_advance_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return autonomy_tool_new(alloc, context, AUTONOMY_TOOL_SOP_ADVANCE, &sop_advance_vtab, out);
}

sc_status sc_tool_cron_list_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return autonomy_tool_new(alloc, context, AUTONOMY_TOOL_CRON_LIST, &cron_list_vtab, out);
}

sc_status sc_tool_cron_upsert_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return autonomy_tool_new(alloc, context, AUTONOMY_TOOL_CRON_UPSERT, &cron_upsert_vtab, out);
}

sc_status sc_tool_cron_remove_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return autonomy_tool_new(alloc, context, AUTONOMY_TOOL_CRON_REMOVE, &cron_remove_vtab, out);
}

static sc_status autonomy_tool_spec(void *impl, sc_tool_spec *out)
{
    const autonomy_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.autonomy_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = tool_name(tool->kind),
        .description = tool_description(tool->kind),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = tool_risk(tool->kind),
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_NONE,
        .side_effect = tool_side_effect(tool->kind),
        .default_autonomy = tool_risk(tool->kind) == SC_TOOL_RISK_READONLY ? SC_AUTONOMY_AUTONOMOUS : SC_AUTONOMY_SUPERVISED,
        .catalog_metadata_key = tool_catalog(tool->kind),
    };
    return sc_status_ok();
}

static sc_status autonomy_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    autonomy_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.autonomy_tool.invalid_argument");
    }
    switch (tool->kind) {
    case AUTONOMY_TOOL_SOP_INSPECT:
        return invoke_sop_inspect(tool, call, alloc, out);
    case AUTONOMY_TOOL_SOP_ADVANCE:
        return invoke_sop_advance(tool, call, alloc, out);
    case AUTONOMY_TOOL_CRON_LIST:
        return invoke_cron_list(tool, call, alloc, out);
    case AUTONOMY_TOOL_CRON_UPSERT:
        return invoke_cron_upsert(tool, call, alloc, out);
    case AUTONOMY_TOOL_CRON_REMOVE:
        return invoke_cron_remove(tool, call, alloc, out);
    }
    return sc_status_invalid_argument("sc.autonomy_tool.unknown");
}

static void autonomy_tool_destroy(void *impl)
{
    autonomy_tool *tool = impl;
    sc_allocator *alloc = nullptr;
    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc;
    sc_tool_impl_context_clear(&tool->base);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(autonomy_tool));
}

static sc_status autonomy_tool_new(sc_allocator *alloc,
                                   const sc_tool_context *context,
                                   autonomy_tool_kind kind,
                                   const sc_tool_vtab *vtab,
                                   sc_tool **out)
{
    autonomy_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr || vtab == nullptr) {
        return sc_status_invalid_argument("sc.autonomy_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(autonomy_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (autonomy_tool){.kind = kind};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = build_schema(alloc, kind, &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        autonomy_tool_destroy(tool);
    }
    return status;
}

static sc_status build_schema(sc_allocator *alloc, autonomy_tool_kind kind, sc_json_value **out)
{
    if (kind == AUTONOMY_TOOL_SOP_INSPECT) {
        return sc_tool_schema_string_required(alloc, sc_str_from_cstr("path"), sc_str_from_cstr("tool.sop_inspect.description"), out);
    }
    if (kind == AUTONOMY_TOOL_SOP_ADVANCE) {
        return sc_tool_schema_two_strings(alloc,
                                          sc_str_from_cstr("path"),
                                          true,
                                          sc_str_from_cstr("current_step"),
                                          false,
                                          out);
    }
    if (kind == AUTONOMY_TOOL_CRON_LIST) {
        return sc_tool_schema_string(alloc, sc_str_from_cstr("id"), false, out);
    }
    if (kind == AUTONOMY_TOOL_CRON_REMOVE) {
        return sc_tool_schema_string_required(alloc, sc_str_from_cstr("id"), sc_str_from_cstr("tool.cron_remove.description"), out);
    }
    return sc_tool_schema_four_strings(alloc,
                                       sc_str_from_cstr("id"),
                                       true,
                                       sc_str_from_cstr("schedule"),
                                       true,
                                       sc_str_from_cstr("prompt"),
                                       true,
                                       sc_str_from_cstr("delivery_target"),
                                       false,
                                       out);
}

static sc_str tool_name(autonomy_tool_kind kind)
{
    switch (kind) {
    case AUTONOMY_TOOL_SOP_INSPECT:
        return sc_str_from_cstr("sop_inspect");
    case AUTONOMY_TOOL_SOP_ADVANCE:
        return sc_str_from_cstr("sop_advance");
    case AUTONOMY_TOOL_CRON_LIST:
        return sc_str_from_cstr("cron_list");
    case AUTONOMY_TOOL_CRON_UPSERT:
        return sc_str_from_cstr("cron_upsert");
    case AUTONOMY_TOOL_CRON_REMOVE:
        return sc_str_from_cstr("cron_remove");
    }
    return sc_str_from_cstr("autonomy_tool");
}

static sc_str tool_description(autonomy_tool_kind kind)
{
    switch (kind) {
    case AUTONOMY_TOOL_SOP_INSPECT:
        return sc_str_from_cstr("tool.sop_inspect.description");
    case AUTONOMY_TOOL_SOP_ADVANCE:
        return sc_str_from_cstr("tool.sop_advance.description");
    case AUTONOMY_TOOL_CRON_LIST:
        return sc_str_from_cstr("tool.cron_list.description");
    case AUTONOMY_TOOL_CRON_UPSERT:
        return sc_str_from_cstr("tool.cron_upsert.description");
    case AUTONOMY_TOOL_CRON_REMOVE:
        return sc_str_from_cstr("tool.cron_remove.description");
    }
    return sc_str_from_cstr("tool.autonomy.description");
}

static sc_str tool_catalog(autonomy_tool_kind kind)
{
    switch (kind) {
    case AUTONOMY_TOOL_SOP_INSPECT:
        return sc_str_from_cstr("tool.sop_inspect.catalog");
    case AUTONOMY_TOOL_SOP_ADVANCE:
        return sc_str_from_cstr("tool.sop_advance.catalog");
    case AUTONOMY_TOOL_CRON_LIST:
        return sc_str_from_cstr("tool.cron_list.catalog");
    case AUTONOMY_TOOL_CRON_UPSERT:
        return sc_str_from_cstr("tool.cron_upsert.catalog");
    case AUTONOMY_TOOL_CRON_REMOVE:
        return sc_str_from_cstr("tool.cron_remove.catalog");
    }
    return sc_str_from_cstr("tool.autonomy.catalog");
}

static sc_tool_risk tool_risk(autonomy_tool_kind kind)
{
    return kind == AUTONOMY_TOOL_SOP_INSPECT || kind == AUTONOMY_TOOL_SOP_ADVANCE ||
            kind == AUTONOMY_TOOL_CRON_LIST ?
        SC_TOOL_RISK_READONLY :
        SC_TOOL_RISK_SIDE_EFFECT;
}

static sc_tool_side_effect tool_side_effect(autonomy_tool_kind kind)
{
    return kind == AUTONOMY_TOOL_SOP_INSPECT || kind == AUTONOMY_TOOL_SOP_ADVANCE ||
            kind == AUTONOMY_TOOL_CRON_LIST ?
        SC_TOOL_SIDE_EFFECT_READ :
        SC_TOOL_SIDE_EFFECT_WRITE;
}

static sc_status invoke_sop_inspect(autonomy_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str path = {0};
    sc_string resolved = {0};
    sc_sop_document document = {0};
    sc_string_builder builder = {0};
    sc_string text = {0};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("path"), &path);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("sop_inspect"),
                                        SC_TOOL_RISK_READONLY,
                                        path,
                                        true,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        status = sc_workspace_resolve(tool->base.context.policy, path, true, alloc, &resolved);
    }
    if (sc_status_is_ok(status)) {
        status = sc_sop_markdown_load(alloc, sc_string_as_str(&resolved), &document);
    }
    if (sc_status_is_ok(status)) {
        sc_string_builder_init(&builder, alloc);
        status = append_sop_summary(&builder, &document);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &text);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&text), true);
    }
    (void)sc_tool_record_receipt_status(&tool->base,
                                        sc_str_from_cstr("sop_inspect"),
                                        path,
                                        sc_status_is_ok(status) ? sc_str_from_cstr("sop summary") : sc_str_from_cstr("error"),
                                        sc_status_is_ok(status),
                                        status);
    sc_string_clear(&text);
    sc_sop_document_clear(&document);
    sc_string_clear(&resolved);
    return status;
}

static sc_status invoke_sop_advance(autonomy_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str path = {0};
    sc_str step_text = {0};
    sc_string resolved = {0};
    sc_sop_document document = {0};
    sc_sop_run_state run = {0};
    sc_audit_chain audit = {0};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("path"), &path);
    }
    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("current_step"), &step_text);
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("sop_advance"),
                                        SC_TOOL_RISK_READONLY,
                                        path,
                                        true,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        status = sc_workspace_resolve(tool->base.context.policy, path, true, alloc, &resolved);
    }
    if (sc_status_is_ok(status)) {
        status = sc_sop_markdown_load(alloc, sc_string_as_str(&resolved), &document);
    }
    if (sc_status_is_ok(status)) {
        sc_sop_run_start(&run);
        if (step_text.len > 0) {
            sc_string cstr = {0};
            status = sc_string_from_str(alloc, step_text, &cstr);
            if (sc_status_is_ok(status)) {
                char *end = nullptr;
                unsigned long parsed = strtoul(cstr.ptr, &end, 10);
                if (end == cstr.ptr || *end != '\0') {
                    status = sc_status_invalid_argument("sc.sop_advance.current_step_invalid");
                } else {
                    run.current_step = (size_t)parsed;
                }
            }
            sc_string_clear(&cstr);
        }
    }
    if (sc_status_is_ok(status)) {
        sc_audit_chain_init(&audit, alloc);
        status = sc_sop_run_advance(&run, &document, &audit);
    }
    if (sc_status_is_ok(status) || (status.code == SC_ERR_CANCELLED && status.error_key != nullptr &&
                                    strcmp(status.error_key, "sc.sop.manual_approval_required") == 0)) {
        sc_status display_status = status;
        char text[128] = {0};
        int written = snprintf(text,
                               sizeof(text),
                               "current_step=%zu waiting_approval=%s completed=%s status=%s",
                               run.current_step,
                               run.waiting_approval ? "true" : "false",
                               run.completed ? "true" : "false",
                               display_status.error_key == nullptr ? "ok" : display_status.error_key);
        if (written < 0 || (size_t)written >= sizeof(text)) {
            status = sc_status_no_memory();
        } else {
            if (!sc_status_is_ok(status)) {
                sc_status_clear(&status);
            }
            status = sc_tool_set_output(alloc, &tool->base, out, sc_str_from_cstr(text), true);
        }
    }
    (void)sc_tool_record_receipt_status(&tool->base,
                                        sc_str_from_cstr("sop_advance"),
                                        path,
                                        sc_status_is_ok(status) ? sc_str_from_cstr("advanced") : sc_str_from_cstr("error"),
                                        sc_status_is_ok(status),
                                        status);
    sc_audit_chain_clear(&audit);
    sc_sop_document_clear(&document);
    sc_string_clear(&resolved);
    return status;
}

static sc_status invoke_cron_list(autonomy_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str id = {0};
    sc_string_builder builder = {0};
    sc_string text = {0};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("id"), &id);
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("cron_list"),
                                        SC_TOOL_RISK_READONLY,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status) && tool->base.context.cron_jobs == nullptr) {
        status = sc_status_unsupported("sc.cron_tool.store_missing");
    }
    if (sc_status_is_ok(status)) {
        sc_string_builder_init(&builder, alloc);
        for (size_t i = 0; sc_status_is_ok(status) && i < tool->base.context.cron_jobs->jobs.len; i += 1) {
            const sc_cron_job *job = sc_vec_at_const(&tool->base.context.cron_jobs->jobs, i);
            if (job != nullptr && (id.len == 0 || sc_str_equal(sc_string_as_str(&job->id), id))) {
                status = append_cron_job(&builder, job);
            }
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &text);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&text), true);
    }
    (void)sc_tool_record_receipt_status(&tool->base,
                                        sc_str_from_cstr("cron_list"),
                                        id,
                                        sc_status_is_ok(status) ? sc_str_from_cstr("listed") : sc_str_from_cstr("error"),
                                        sc_status_is_ok(status),
                                        status);
    sc_string_clear(&text);
    return status;
}

static sc_status invoke_cron_upsert(autonomy_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str id = {0};
    sc_str schedule = {0};
    sc_str prompt = {0};
    sc_str target = {0};
    sc_cron_job job = {.struct_size = sizeof(job), .kind = SC_CRON_JOB_AGENT, .enabled = true};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("id"), &id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("schedule"), &schedule);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("prompt"), &prompt);
    }
    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("delivery_target"), &target);
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("cron_upsert"),
                                        SC_TOOL_RISK_SIDE_EFFECT,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status) && tool->base.context.cron_jobs == nullptr) {
        status = sc_status_unsupported("sc.cron_tool.store_missing");
    }
    if (sc_status_is_ok(status)) {
        status = sc_cron_schedule_parse(schedule, &job.schedule);
    }
    if (sc_status_is_ok(status)) {
        status = string_set(alloc, &job.id, id);
    }
    if (sc_status_is_ok(status)) {
        status = string_set(alloc, &job.command, prompt);
    }
    if (sc_status_is_ok(status)) {
        status = string_set(alloc, &job.delivery_target, target.len == 0 ? sc_str_from_cstr("default") : target);
    }
    if (sc_status_is_ok(status)) {
        status = string_set(alloc, &job.schedule_text, schedule);
    }
    if (sc_status_is_ok(status)) {
        status = sc_cron_job_store_put(tool->base.context.cron_jobs, &job);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_str_from_cstr("upserted"), true);
    }
    (void)sc_tool_record_receipt_status(&tool->base,
                                        sc_str_from_cstr("cron_upsert"),
                                        id,
                                        sc_status_is_ok(status) ? sc_str_from_cstr("upserted") : sc_str_from_cstr("error"),
                                        sc_status_is_ok(status),
                                        status);
    sc_cron_job_clear(&job);
    return status;
}

static sc_status invoke_cron_remove(autonomy_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str id = {0};
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("id"), &id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("cron_remove"),
                                        SC_TOOL_RISK_SIDE_EFFECT,
                                        sc_str_from_cstr(""),
                                        false,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status) && tool->base.context.cron_jobs == nullptr) {
        status = sc_status_unsupported("sc.cron_tool.store_missing");
    }
    if (sc_status_is_ok(status)) {
        status = sc_cron_job_store_remove(tool->base.context.cron_jobs, id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_str_from_cstr("removed"), true);
    }
    (void)sc_tool_record_receipt_status(&tool->base,
                                        sc_str_from_cstr("cron_remove"),
                                        id,
                                        sc_status_is_ok(status) ? sc_str_from_cstr("removed") : sc_str_from_cstr("error"),
                                        sc_status_is_ok(status),
                                        status);
    return status;
}

static sc_status append_sop_summary(sc_string_builder *builder, const sc_sop_document *document)
{
    sc_status status;

    if (builder == nullptr || document == nullptr) {
        return sc_status_invalid_argument("sc.sop_tool.summary_invalid_argument");
    }
    status = sc_string_builder_append_cstr(builder, "title=");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(builder, sc_string_as_str(&document->title));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\nsteps=");
    }
    if (sc_status_is_ok(status)) {
        char count[32] = {0};
        int written = snprintf(count, sizeof(count), "%zu\n", document->steps.len);
        if (written < 0 || (size_t)written >= sizeof(count)) {
            return sc_status_no_memory();
        }
        status = sc_string_builder_append_cstr(builder, count);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < document->steps.len; i += 1) {
        const sc_sop_step *step = sc_vec_at_const(&document->steps, i);
        char prefix[48] = {0};
        int written = snprintf(prefix, sizeof(prefix), "%zu:", i);
        if (written < 0 || (size_t)written >= sizeof(prefix)) {
            return sc_status_no_memory();
        }
        status = sc_string_builder_append_cstr(builder, prefix);
        if (sc_status_is_ok(status) && step != nullptr) {
            status = sc_string_builder_append(builder, sc_string_as_str(&step->name));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(builder, "\n");
        }
    }
    return status;
}

static sc_status append_cron_job(sc_string_builder *builder, const sc_cron_job *job)
{
    sc_status status;
    sc_string schedule_owner = {0};
    sc_str schedule = {0};

    if (builder == nullptr || job == nullptr) {
        return sc_status_invalid_argument("sc.cron_tool.job_invalid_argument");
    }
    if (job->schedule_text.len > 0) {
        schedule = sc_string_as_str(&job->schedule_text);
    } else {
        status = sc_cron_schedule_format(&job->schedule, job->id.alloc, &schedule_owner);
        if (!sc_status_is_ok(status)) {
            return status;
        }
        schedule = sc_string_as_str(&schedule_owner);
    }
    status = sc_string_builder_append(builder, sc_string_as_str(&job->id));
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, " enabled=");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, job->enabled ? "true" : "false");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, " schedule=");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(builder, schedule);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, " prompt=");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(builder, sc_string_as_str(&job->command));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\n");
    }
    sc_string_clear(&schedule_owner);
    return status;
}

static sc_status string_set(sc_allocator *alloc, sc_string *out, sc_str value)
{
    sc_string_clear(out);
    return sc_string_from_str(alloc, value, out);
}
