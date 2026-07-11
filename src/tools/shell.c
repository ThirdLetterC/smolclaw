// cppcheck-suppress-file redundantInitialization
#include "tools/tool_internal.h"
#include "tools/process_runner.h"

#include "sc/config.h"
#include "sc/log.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef struct shell_tool {
    sc_tool_impl_context base;
} shell_tool;

static sc_status shell_spec(void *impl, sc_tool_spec *out);
static sc_status shell_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void shell_destroy(void *impl);
static sc_status validate_shell_command(const shell_tool *tool, sc_str command);
static bool command_contains_danger(sc_str command);
static sc_status command_basename(sc_allocator *alloc, sc_str command, sc_string *out);
static sc_status shell_execution_command(const shell_tool *tool, sc_str command, sc_string *storage, sc_str *out);
static bool command_has_shell_control(sc_str command);
static bool rtk_command_available(sc_allocator *alloc, sc_str command);
static bool rtk_command_available_on_path(sc_allocator *alloc, sc_str command);
static bool command_has_slash(sc_str command);
static sc_status make_cstr(sc_allocator *alloc, sc_str input, char **out);
static sc_status shell_quote(sc_allocator *alloc, sc_str input, sc_string *out);
static sc_status build_rtk_command(sc_allocator *alloc, sc_str rtk_command, bool ultra_compact, sc_str command, sc_string *out);
static void log_rtk_unavailable(sc_str rtk_command, bool fallback_passthrough);
static bool json_array_contains(sc_str raw_json, sc_str value);

static const sc_tool_vtab shell_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "shell",
    .display_name = "Shell",
    .feature_flag = "SC_TOOL_SHELL",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = shell_spec,
    .invoke = shell_invoke,
    .destroy = shell_destroy,
};

sc_status sc_tool_shell_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    shell_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.shell_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(shell_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (shell_tool){0};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status)) {
        status = sc_tool_schema_string_required(alloc,
                                                sc_str_from_cstr("command"),
                                                sc_str_from_cstr("tool.shell.description"),
                                                &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, &shell_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        shell_destroy(tool);
    }
    return status;
}

static sc_status shell_spec(void *impl, sc_tool_spec *out)
{
    const shell_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.shell_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_str_from_cstr("shell"),
        .description = sc_str_from_cstr("tool.shell.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = SC_TOOL_RISK_SHELL,
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_PROCESS,
        .side_effect = SC_TOOL_SIDE_EFFECT_PROCESS,
        .default_autonomy = SC_AUTONOMY_SUPERVISED,
        .catalog_metadata_key = sc_str_from_cstr("tool.shell.catalog"),
    };
    return sc_status_ok();
}

static sc_status shell_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    shell_tool *tool = impl;
    sc_str command = {0};
    sc_str execution_command = {0};
    sc_str otp = {0};
    sc_string wrapped_command = {0};
    sc_string output = {0};
    sc_status status;

    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.shell_tool.invalid_argument");
    }
    status = sc_tool_check_cancelled(&tool->base, call);
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("command"), &command);
    }
    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("otp"), &otp);
        status = sc_tool_security_check_ex(&tool->base,
                                           sc_str_from_cstr("shell"),
                                           SC_TOOL_RISK_SHELL,
                                           sc_str_from_cstr(""),
                                           false,
                                           sc_str_from_cstr(""),
                                           command,
                                           sc_str_from_cstr(""),
                                           otp);
    }
    if (sc_status_is_ok(status)) {
        status = validate_shell_command(tool, command);
    }
    if (sc_status_is_ok(status)) {
        status = shell_execution_command(tool, command, &wrapped_command, &execution_command);
    }
    if (sc_status_is_ok(status)) {
        sc_str args[] = {sc_str_from_cstr("-c"), execution_command};
        sc_tool_process_request request = {
            .executable = sc_str_from_cstr("/bin/sh"),
            .args = args,
            .arg_count = SC_ARRAY_LEN(args),
            .cwd = sc_string_as_str(&tool->base.context.policy->workspace_root),
            .policy = tool->base.context.policy,
            .may_use_network = true,
            .timeout_ms = tool->base.context.timeout_ms == 0 ? 30000 : tool->base.context.timeout_ms,
            .max_output_bytes = tool->base.context.max_output_bytes == 0 ? 4096 : tool->base.context.max_output_bytes,
            .cancel_token = call == nullptr ? tool->base.context.cancel_token : call->cancel_token,
        };
        status = sc_tool_process_run(alloc, &request, &output);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&output), true);
    }
    (void)sc_tool_record_receipt_status(&tool->base,
                                        sc_str_from_cstr("shell"),
                                        command,
                                        sc_status_is_ok(status) ? sc_string_as_str(&out->output) : sc_str_from_cstr("error"),
                                        sc_status_is_ok(status),
                                        status);
    sc_string_clear(&wrapped_command);
    sc_string_clear(&output);
    return status;
}

static void shell_destroy(void *impl)
{
    shell_tool *tool = impl;
    sc_allocator *alloc = nullptr;
    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc;
    sc_tool_impl_context_clear(&tool->base);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(shell_tool));
}

static sc_status validate_shell_command(const shell_tool *tool, sc_str command)
{
    sc_string basename = {0};
    sc_string allowed_raw = {0};
    sc_string forbidden_raw = {0};
    sc_status status;

    if (tool == nullptr || command.ptr == nullptr || command.len == 0) {
        return sc_status_invalid_argument("sc.shell_tool.command_invalid");
    }
    if (command_contains_danger(command)) {
        return sc_status_security_denied("sc.shell_tool.command_denied");
    }
    status = command_basename(tool->base.alloc, command, &basename);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (tool->base.context.config != nullptr &&
        sc_status_is_ok(sc_config_get_prop(tool->base.context.config,
                                           sc_str_from_cstr("autonomy.forbidden_commands"),
                                           tool->base.alloc,
                                           &forbidden_raw)) &&
        json_array_contains(sc_string_as_str(&forbidden_raw), sc_string_as_str(&basename))) {
        status = sc_status_security_denied("sc.shell_tool.command_forbidden");
    }
    if (sc_status_is_ok(status) && tool->base.context.config != nullptr &&
        sc_status_is_ok(sc_config_get_prop(tool->base.context.config,
                                           sc_str_from_cstr("autonomy.allowed_commands"),
                                           tool->base.alloc,
                                           &allowed_raw)) &&
        allowed_raw.len > 2) {
        if (command_has_shell_control(command)) {
            status = sc_status_security_denied("sc.shell_tool.command_composition_denied");
        } else if (!json_array_contains(sc_string_as_str(&allowed_raw), sc_string_as_str(&basename))) {
            status = sc_status_security_denied("sc.shell_tool.command_not_allowed");
        }
    }
    sc_string_clear(&basename);
    sc_string_clear(&allowed_raw);
    sc_string_clear(&forbidden_raw);
    return status;
}

static bool command_contains_danger(sc_str command)
{
    static const char *needles[] = {"rm -rf /", "mkfs", "shutdown", "reboot", ":(){", "dd if=", ">/dev/"};
    for (size_t i = 0; i < SC_ARRAY_LEN(needles); i += 1) {
        size_t needle_len = strlen(needles[i]);
        for (size_t j = 0; j + needle_len <= command.len; j += 1) {
            if (memcmp(&command.ptr[j], needles[i], needle_len) == 0) {
                return true;
            }
        }
    }
    return false;
}

static sc_status command_basename(sc_allocator *alloc, sc_str command, sc_string *out)
{
    size_t start = 0;
    size_t end = 0;
    size_t slash = 0;

    while (start < command.len && (command.ptr[start] == ' ' || command.ptr[start] == '\t')) {
        start += 1;
    }
    end = start;
    while (end < command.len && command.ptr[end] != ' ' && command.ptr[end] != '\t' &&
           command.ptr[end] != '\n' && command.ptr[end] != ';' && command.ptr[end] != '|') {
        end += 1;
    }
    if (end == start) {
        return sc_status_invalid_argument("sc.shell_tool.command_invalid");
    }
    slash = start;
    for (size_t i = start; i < end; i += 1) {
        if (command.ptr[i] == '/') {
            slash = i + 1;
        }
    }
    return sc_string_from_str(alloc, sc_str_from_parts(&command.ptr[slash], end - slash), out);
}

static sc_status shell_execution_command(const shell_tool *tool, sc_str command, sc_string *storage, sc_str *out)
{
    sc_string basename = {0};
    sc_string allowed_raw = {0};
    sc_string rtk_command = {0};
    bool fallback_passthrough = true;
    bool ultra_compact = true;
    sc_status status;

    if (tool == nullptr || storage == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.shell_tool.invalid_argument");
    }
    *out = command;
    if (tool->base.context.config == nullptr ||
        !sc_config_get_bool(tool->base.context.config, sc_str_from_cstr("tools.rtk.enabled"), false)) {
        return sc_status_ok();
    }

    status = command_basename(tool->base.alloc, command, &basename);
    if (sc_status_is_ok(status)) {
        status = sc_config_get_prop(tool->base.context.config,
                                    sc_str_from_cstr("tools.rtk.allowed_commands"),
                                    tool->base.alloc,
                                    &allowed_raw);
    }
    if (sc_status_is_ok(status) &&
        (!json_array_contains(sc_string_as_str(&allowed_raw), sc_string_as_str(&basename)) ||
         command_has_shell_control(command))) {
        goto cleanup;
    }
    if (sc_status_is_ok(status)) {
        status = sc_config_get_prop(tool->base.context.config,
                                    sc_str_from_cstr("tools.rtk.command"),
                                    tool->base.alloc,
                                    &rtk_command);
    }
    if (sc_status_is_ok(status) && rtk_command.len == 0) {
        status = sc_string_from_cstr(tool->base.alloc, "rtk", &rtk_command);
    }
    if (sc_status_is_ok(status)) {
        fallback_passthrough = sc_config_get_bool(tool->base.context.config,
                                                  sc_str_from_cstr("tools.rtk.fallback_passthrough"),
                                                  true);
        ultra_compact = sc_config_get_bool(tool->base.context.config,
                                           sc_str_from_cstr("tools.rtk.ultra_compact"),
                                           true);
        if (!rtk_command_available(tool->base.alloc, sc_string_as_str(&rtk_command))) {
            log_rtk_unavailable(sc_string_as_str(&rtk_command), fallback_passthrough);
            status = fallback_passthrough ? sc_status_ok() : sc_status_unsupported("sc.shell_tool.rtk_unavailable");
            goto cleanup;
        }
    }
    if (sc_status_is_ok(status)) {
        status = build_rtk_command(tool->base.alloc,
                                   sc_string_as_str(&rtk_command),
                                   ultra_compact,
                                   command,
                                   storage);
    }
    if (sc_status_is_ok(status)) {
        *out = sc_string_as_str(storage);
    }

cleanup:
    sc_string_clear(&basename);
    sc_string_clear(&allowed_raw);
    sc_string_clear(&rtk_command);
    return status;
}

static bool command_has_shell_control(sc_str command)
{
    for (size_t i = 0; i < command.len; i += 1) {
        switch (command.ptr[i]) {
        case ';':
        case '|':
        case '&':
        case '<':
        case '>':
        case '\n':
        case '\r':
        case '`':
            return true;
        case '$':
            if (i + 1u < command.len && command.ptr[i + 1u] == '(') {
                return true;
            }
            break;
        default:
            break;
        }
    }
    return false;
}

static bool rtk_command_available(sc_allocator *alloc, sc_str command)
{
    char *path = nullptr;
    bool available = false;

    if (command.ptr == nullptr || command.len == 0) {
        return false;
    }
    if (!command_has_slash(command)) {
        return rtk_command_available_on_path(alloc, command);
    }
    if (!sc_status_is_ok(make_cstr(alloc, command, &path))) {
        return false;
    }
    if (path == nullptr) {
        return false;
    }
    available = access(path, X_OK) == 0;
    sc_free(alloc, path, command.len + 1u, _Alignof(char));
    return available;
}

static bool rtk_command_available_on_path(sc_allocator *alloc, sc_str command)
{
    const char *path = getenv("PATH");
    size_t offset = 0;

    if (path == nullptr || command.len == 0 || command.len > 255u) {
        return false;
    }
    while (path[offset] != '\0') {
        size_t start = offset;
        size_t dir_len = 0;
        const char *dir = nullptr;
        size_t total = 0;
        char *candidate = nullptr;

        while (path[offset] != '\0' && path[offset] != ':') {
            offset += 1;
        }
        dir_len = offset - start;
        dir = &path[start];
        if (dir_len == 0) {
            dir_len = 1;
            dir = ".";
        }
        if (dir_len <= SIZE_MAX - command.len - 2u) {
            total = dir_len + command.len + 2u;
            candidate = sc_alloc(alloc, total, _Alignof(char));
        }
        if (candidate != nullptr) {
            (void)memcpy(candidate, dir, dir_len);
            candidate[dir_len] = '/';
            (void)memcpy(candidate + dir_len + 1u, command.ptr, command.len);
            candidate[total - 1u] = '\0';
            bool found = access(candidate, X_OK) == 0;

            sc_free(alloc, candidate, total, _Alignof(char));
            if (found) {
                return true;
            }
        }
        if (path[offset] == ':') {
            offset += 1;
        }
    }
    return false;
}

static bool command_has_slash(sc_str command)
{
    for (size_t i = 0; i < command.len; i += 1) {
        if (command.ptr[i] == '/') {
            return true;
        }
    }
    return false;
}

static sc_status make_cstr(sc_allocator *alloc, sc_str input, char **out)
{
    char *copy = nullptr;

    if (out == nullptr || input.ptr == nullptr || input.len > SIZE_MAX - 1u) {
        return sc_status_invalid_argument("sc.shell_tool.string_invalid");
    }
    copy = sc_alloc(alloc, input.len + 1u, _Alignof(char));
    if (copy == nullptr) {
        return sc_status_no_memory();
    }
    (void)memcpy(copy, input.ptr, input.len);
    copy[input.len] = '\0';
    *out = copy;
    return sc_status_ok();
}

static sc_status shell_quote(sc_allocator *alloc, sc_str input, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    if (out == nullptr || input.ptr == nullptr) {
        return sc_status_invalid_argument("sc.shell_tool.quote_invalid");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "'");
    for (size_t i = 0; sc_status_is_ok(status) && i < input.len; i += 1) {
        if (input.ptr[i] == '\'') {
            status = sc_string_builder_append_cstr(&builder, "'\\''");
        } else {
            status = sc_string_builder_append(&builder, sc_str_from_parts(&input.ptr[i], 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "'");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status build_rtk_command(sc_allocator *alloc, sc_str rtk_command, bool ultra_compact, sc_str command, sc_string *out)
{
    sc_string quoted_rtk = {0};
    sc_string_builder builder = {0};
    sc_status status = shell_quote(alloc, rtk_command, &quoted_rtk);

    if (!sc_status_is_ok(status)) {
        return status;
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "exec ");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, sc_string_as_str(&quoted_rtk));
    }
    if (sc_status_is_ok(status) && ultra_compact) {
        status = sc_string_builder_append_cstr(&builder, " -u");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, " ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, command);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    sc_string_clear(&quoted_rtk);
    return status;
}

static void log_rtk_unavailable(sc_str rtk_command, bool fallback_passthrough)
{
    sc_log_field fields[] = {
        {.key = "command", .value = rtk_command, .secret = false},
        {.key = "fallback_passthrough", .value = sc_str_from_cstr(fallback_passthrough ? "true" : "false"), .secret = false},
    };
    sc_log_write(SC_LOG_WARN, "sc.tools", "sc.shell_tool.rtk_unavailable", fields, SC_ARRAY_LEN(fields));
}

static bool json_array_contains(sc_str raw_json, sc_str value)
{
    sc_json_value *array = nullptr;
    sc_json_parse_error error = {0};
    bool found = false;

    if (raw_json.ptr == nullptr || raw_json.len == 0 || value.ptr == nullptr || value.len == 0 ||
        !sc_status_is_ok(sc_json_parse(sc_allocator_heap(), raw_json, &array, &error))) {
        return false;
    }
    for (size_t i = 0; i < sc_json_array_len(array); i += 1) {
        sc_str item = {0};
        if (sc_json_as_str(sc_json_array_get(array, i), &item) && sc_str_equal(item, value)) {
            found = true;
            break;
        }
    }
    sc_json_destroy(array);
    return found;
}
