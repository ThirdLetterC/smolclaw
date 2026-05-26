#include "sc/provider.h"

#include "json/json_internal.h"
#include "tools/process_runner.h"

#include <stdint.h>
#include <string.h>

typedef struct gemini_cli_provider {
    sc_allocator *alloc;
    sc_provider_options options;
    sc_string provider_name;
    sc_string default_model;
    sc_string command;
} gemini_cli_provider;

static sc_status gemini_cli_generate(void *impl,
                                     const sc_provider_request *request,
                                     sc_allocator *alloc,
                                     sc_provider_response *out);
static sc_status gemini_cli_stream(void *impl,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_provider_stream_callback callback,
                                   void *callback_user_data);
static void gemini_cli_destroy(void *impl);
static sc_status gemini_cli_copy_options(gemini_cli_provider *provider, const sc_provider_options *options);
static sc_status gemini_cli_compose_prompt(sc_allocator *alloc, const sc_provider_request *request, sc_string *out);
static sc_status gemini_cli_run(sc_allocator *alloc,
                                const gemini_cli_provider *provider,
                                sc_str model,
                                sc_str prompt,
                                const sc_provider_request *request,
                                sc_tool_process_result *out);
static sc_status gemini_cli_parse_response(sc_allocator *alloc,
                                           sc_str json,
                                           sc_str model,
                                           sc_provider_response *out);
static sc_status gemini_cli_parse_usage(sc_json_value *root, sc_provider_response *out);
static int64_t json_number_i64(sc_json_value *object, const char *key);
static sc_status copy_response_model(sc_allocator *alloc, sc_str model, sc_provider_response *out);
static sc_status map_process_status(sc_status status);

static const sc_provider_vtab gemini_cli_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "gemini-cli",
    .display_name = "Gemini CLI process provider",
    .feature_flag = "SC_PROVIDER_PROCESS_ADAPTERS",
    .capabilities = SC_CONTRACT_CAP_STREAMING,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = gemini_cli_generate,
    .stream = gemini_cli_stream,
    .destroy = gemini_cli_destroy,
    .description_key = "sc.provider.gemini_cli.description",
    .config_schema_ref = "sc.schema.provider.gemini_cli.v1",
    .default_timeout = {.struct_size = sizeof(sc_provider_timeout_policy),
                        .connect_timeout_ms = 10000,
                        .read_timeout_ms = 30000,
                        .write_timeout_ms = 30000,
                        .total_timeout_ms = 30000,
                        .response_body_limit_bytes = 1024U * 1024U},
    .provider_modes = SC_PROVIDER_MODE_CHAT | SC_PROVIDER_MODE_STREAM,
};

sc_status sc_provider_gemini_cli_new(sc_allocator *alloc, const sc_provider_options *options, sc_provider **out)
{
    gemini_cli_provider *provider = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini_cli.invalid_argument");
    }
    *out = nullptr;
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    provider = sc_alloc(alloc, sizeof(*provider), _Alignof(gemini_cli_provider));
    if (provider == nullptr) {
        return sc_status_no_memory();
    }
    *provider = (gemini_cli_provider){.alloc = alloc};
    status = gemini_cli_copy_options(provider, options);
    if (sc_status_is_ok(status)) {
        status = sc_provider_new(alloc, &gemini_cli_vtab, provider, out);
    }
    if (!sc_status_is_ok(status)) {
        gemini_cli_destroy(provider);
    }
    return status;
}

static sc_status gemini_cli_generate(void *impl,
                                     const sc_provider_request *request,
                                     sc_allocator *alloc,
                                     sc_provider_response *out)
{
    gemini_cli_provider *provider = impl;
    sc_string prompt = {0};
    sc_tool_process_result process = {0};
    sc_str model = {0};
    sc_status status;

    if (provider == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini_cli.invalid_argument");
    }
    status = sc_provider_validate_request(&gemini_cli_vtab, &provider->options, request);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (request->cancel_requested) {
        return sc_status_cancelled("sc.provider_gemini_cli.cancelled");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    model = request->model.len > 0 ? request->model : sc_string_as_str(&provider->default_model);
    status = gemini_cli_compose_prompt(alloc, request, &prompt);
    if (sc_status_is_ok(status)) {
        status = gemini_cli_run(alloc, provider, model, sc_string_as_str(&prompt), request, &process);
    }
    if (!sc_status_is_ok(status)) {
        status = map_process_status(status);
    }
    if (sc_status_is_ok(status)) {
        status = gemini_cli_parse_response(alloc, sc_string_as_str(&process.output), model, out);
    }
    sc_tool_process_result_clear(&process);
    sc_string_clear(&prompt);
    return status;
}

static sc_status gemini_cli_stream(void *impl,
                                   const sc_provider_request *request,
                                   sc_allocator *alloc,
                                   sc_provider_stream_callback callback,
                                   void *callback_user_data)
{
    sc_provider_response response = {0};
    sc_provider_stream_event event = {0};
    sc_status status;

    if (callback == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini_cli.stream_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = gemini_cli_generate(impl, request, alloc, &response);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (response.text.len > 0) {
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_DELTA};
        status = sc_string_from_str(alloc, sc_string_as_str(&response.text), &event.text);
        if (sc_status_is_ok(status)) {
            status = callback(callback_user_data, &event);
        }
        sc_provider_stream_event_clear(&event);
    }
    if (sc_status_is_ok(status)) {
        event = (sc_provider_stream_event){.struct_size = sizeof(event), .type = SC_PROVIDER_STREAM_DONE};
        event.input_tokens = response.input_tokens;
        event.output_tokens = response.output_tokens;
        event.total_tokens = response.total_tokens;
        event.cost_usd = response.cost_usd;
        status = callback(callback_user_data, &event);
    }
    sc_provider_response_clear(&response);
    return status;
}

static void gemini_cli_destroy(void *impl)
{
    gemini_cli_provider *provider = impl;
    if (provider == nullptr) {
        return;
    }
    sc_string_clear(&provider->provider_name);
    sc_string_clear(&provider->default_model);
    sc_string_clear(&provider->command);
    sc_free(provider->alloc, provider, sizeof(*provider), _Alignof(gemini_cli_provider));
}

static sc_status gemini_cli_copy_options(gemini_cli_provider *provider, const sc_provider_options *options)
{
    sc_status status = sc_status_ok();

    if (provider == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini_cli.invalid_argument");
    }
    if (options != nullptr) {
        provider->options = *options;
        status = sc_string_from_str(provider->alloc, options->provider_name, &provider->provider_name);
    }
    if (sc_status_is_ok(status) && options != nullptr) {
        status = sc_string_from_str(provider->alloc, options->default_model, &provider->default_model);
    }
    if (sc_status_is_ok(status) && options != nullptr) {
        status = sc_string_from_str(provider->alloc, options->command, &provider->command);
    }
    provider->options.provider_name = sc_string_as_str(&provider->provider_name);
    provider->options.default_model = sc_string_as_str(&provider->default_model);
    provider->options.command = sc_string_as_str(&provider->command);
    return status;
}

static sc_status gemini_cli_compose_prompt(sc_allocator *alloc, const sc_provider_request *request, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini_cli.prompt_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    if (request->system_instruction.len > 0) {
        status = sc_string_builder_append_cstr(&builder, "System:\n");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, request->system_instruction);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\n\n");
        }
    }
    if (sc_status_is_ok(status) && request->media_context.len > 0) {
        status = sc_string_builder_append_cstr(&builder, "Context:\n");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, request->media_context);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\n\n");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, request->prompt);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status gemini_cli_run(sc_allocator *alloc,
                                const gemini_cli_provider *provider,
                                sc_str model,
                                sc_str prompt,
                                const sc_provider_request *request,
                                sc_tool_process_result *out)
{
    static const sc_str env_passthrough[] = {
        {.ptr = "HOME", .len = 4},
        {.ptr = "XDG_CONFIG_HOME", .len = 15},
        {.ptr = "XDG_CACHE_HOME", .len = 14},
        {.ptr = "XDG_DATA_HOME", .len = 13},
        {.ptr = "GEMINI_CLI_SYSTEM_DEFAULTS_PATH", .len = 31},
        {.ptr = "GEMINI_CLI_SYSTEM_SETTINGS_PATH", .len = 31},
        {.ptr = "NO_COLOR", .len = 8},
    };
    sc_str args[7] = {0};
    size_t arg_count = 0;
    sc_tool_process_request process = {0};

    if (provider == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini_cli.process_invalid_argument");
    }
    if (provider->command.len == 0) {
        args[arg_count++] = sc_str_from_cstr("gemini");
    }
    args[arg_count++] = sc_str_from_cstr("--prompt");
    args[arg_count++] = prompt;
    args[arg_count++] = sc_str_from_cstr("--output-format");
    args[arg_count++] = sc_str_from_cstr("json");
    if (model.len > 0) {
        args[arg_count++] = sc_str_from_cstr("--model");
        args[arg_count++] = model;
    }
    sc_cancel_token cancel_token = {.cancel_requested = true};
    const sc_cancel_token *process_cancel = request != nullptr && request->cancel_requested ? &cancel_token : nullptr;
    sc_str executable = provider->command.len > 0 ? sc_string_as_str(&provider->command) : sc_str_from_cstr("/usr/bin/env");
    process = (sc_tool_process_request){
        .executable = executable,
        .args = args,
        .arg_count = arg_count,
        .env_passthrough = env_passthrough,
        .env_passthrough_count = SC_ARRAY_LEN(env_passthrough),
        .may_use_network = true,
        .timeout_ms = provider->options.timeout_ms == 0 ? 30000 : provider->options.timeout_ms,
        .max_output_bytes = gemini_cli_vtab.default_timeout.response_body_limit_bytes,
        .cancel_token = process_cancel,
    };
    return sc_tool_process_run_ex(alloc, &process, out);
}

static sc_status gemini_cli_parse_response(sc_allocator *alloc,
                                           sc_str json,
                                           sc_str model,
                                           sc_provider_response *out)
{
    sc_json_value *root = nullptr;
    sc_json_parse_error error = {0};
    sc_json_value *response_value = nullptr;
    sc_str response = {0};
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.provider_gemini_cli.parse_invalid_argument");
    }
    status = sc_json_parse(alloc, json, &root, &error);
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(root);
        return sc_status_parse("sc.provider_gemini_cli.invalid_json");
    }
    response_value = sc_json_object_get(root, sc_str_from_cstr("response"));
    if (response_value == nullptr || !sc_json_as_str(response_value, &response) || response.len == 0) {
        sc_json_destroy(root);
        return sc_status_parse("sc.provider_gemini_cli.empty_response");
    }
    sc_provider_response tmp = {0};
    sc_provider_response_init(&tmp, alloc);
    status = sc_string_from_str(alloc, response, &tmp.text);
    if (sc_status_is_ok(status)) {
        status = copy_response_model(alloc, model, &tmp);
    }
    if (sc_status_is_ok(status)) {
        status = gemini_cli_parse_usage(root, &tmp);
    }
    if (sc_status_is_ok(status)) {
        *out = tmp;
        tmp = (sc_provider_response){0};
    }
    sc_provider_response_clear(&tmp);
    sc_json_destroy(root);
    return status;
}

static sc_status gemini_cli_parse_usage(sc_json_value *root, sc_provider_response *out)
{
    sc_json_value *stats = sc_json_object_get(root, sc_str_from_cstr("stats"));
    sc_json_value *models = stats == nullptr ? nullptr : sc_json_object_get(stats, sc_str_from_cstr("models"));
    size_t model_count = sc_json_object_len_internal(models);

    if (out == nullptr || models == nullptr) {
        return sc_status_ok();
    }
    for (size_t i = 0; i < model_count; i += 1) {
        sc_str key = {0};
        sc_json_value *model = nullptr;
        sc_json_value *tokens = nullptr;
        sc_status status = sc_json_object_entry_internal(models, i, &key, &model);
        (void)key;
        if (!sc_status_is_ok(status)) {
            return status;
        }
        tokens = sc_json_object_get(model, sc_str_from_cstr("tokens"));
        out->input_tokens += json_number_i64(tokens, "prompt");
        out->output_tokens += json_number_i64(tokens, "candidates");
        out->total_tokens += json_number_i64(tokens, "total");
    }
    return sc_status_ok();
}

static int64_t json_number_i64(sc_json_value *object, const char *key)
{
    sc_json_value *value = object == nullptr ? nullptr : sc_json_object_get(object, sc_str_from_cstr(key));
    double number = 0.0;

    if (value == nullptr || !sc_json_as_number(value, &number) || number < 0.0 || number > 9223372036854775807.0) {
        return 0;
    }
    return (int64_t)number;
}

static sc_status copy_response_model(sc_allocator *alloc, sc_str model, sc_provider_response *out)
{
    if (out == nullptr || model.len == 0) {
        return sc_status_ok();
    }
    return sc_string_from_str(alloc, model, &out->model);
}

static sc_status map_process_status(sc_status status)
{
    sc_status_code code = status.code;
    sc_status_clear(&status);
    if (code == SC_ERR_TIMEOUT) {
        return sc_status_timeout("sc.provider_gemini_cli.timeout");
    }
    if (code == SC_ERR_CANCELLED) {
        return sc_status_cancelled("sc.provider_gemini_cli.cancelled");
    }
    if (code == SC_ERR_IO) {
        return sc_status_io("sc.provider_gemini_cli.process_failed");
    }
    if (code == SC_ERR_NO_MEMORY) {
        return sc_status_no_memory();
    }
    return sc_status_make(code, "sc.provider_gemini_cli.process_failed", nullptr);
}
