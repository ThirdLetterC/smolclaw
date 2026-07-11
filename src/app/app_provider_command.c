#include "app/app_commands.h"
#include "app/app_internal.h"
#include "core/build_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc/config.h"

static const char *provider_status(const char *name)
{
    if (name == nullptr) {
        return "unknown";
    }
    if (strcmp(name, "mock") == 0 || strcmp(name, "openrouter") == 0) {
        return "built-in";
    }
    if (strcmp(name, "openai-compatible") == 0 || strcmp(name, "azure-openai") == 0 ||
        strcmp(name, "llamacpp") == 0 || strcmp(name, "llama.cpp") == 0 ||
        strcmp(name, "sglang") == 0 || strcmp(name, "vllm") == 0 ||
        strcmp(name, "groq") == 0 || strcmp(name, "mistral") == 0 ||
        strcmp(name, "xai") == 0 || strcmp(name, "deepseek") == 0 ||
        strcmp(name, "moonshot") == 0 || strcmp(name, "zai") == 0 ||
        strcmp(name, "minimax") == 0 || strcmp(name, "qianfan") == 0 ||
        strcmp(name, "venice") == 0 || strcmp(name, "vercel-ai-gateway") == 0 ||
        strcmp(name, "cloudflare-gateway") == 0 || strcmp(name, "opencode") == 0 ||
        strcmp(name, "synthetic") == 0) {
        return sc_build_feature_enabled("SC_PROVIDER_OPENAI_COMPATIBLE") != 0 ? "built-in" : "disabled";
    }
    if (strcmp(name, "openai") == 0) {
        return sc_build_feature_enabled("SC_PROVIDER_OPENAI") != 0 ? "built-in" : "disabled";
    }
    if (strcmp(name, "anthropic") == 0) {
        return sc_build_feature_enabled("SC_PROVIDER_ANTHROPIC") != 0 ? "built-in" : "disabled";
    }
    if (strcmp(name, "gemini") == 0) {
        return sc_build_feature_enabled("SC_PROVIDER_GEMINI") != 0 ? "built-in" : "disabled";
    }
    if (strcmp(name, "ollama") == 0) {
        return sc_build_feature_enabled("SC_PROVIDER_OLLAMA") != 0 ? "built-in" : "disabled";
    }
    if (strcmp(name, "bedrock") == 0) {
        return sc_build_feature_enabled("SC_PROVIDER_BEDROCK") != 0 ? "built-in" : "disabled";
    }
    if (strcmp(name, "claude-code") == 0) {
        return sc_build_feature_enabled("SC_PROVIDER_PROCESS_ADAPTERS") != 0 ? "built-in" : "disabled";
    }
    if (strcmp(name, "gemini-cli") == 0 || strcmp(name, "copilot") == 0 || strcmp(name, "kilocli") == 0) {
        return sc_build_feature_enabled("SC_PROVIDER_PROCESS_ADAPTERS") != 0 ? "adapter stub" : "disabled";
    }
    return "not registered";
}

static void print_provider_adapter(FILE *stream, const char *name, const char *display, const char *status)
{
    (void)fprintf(stream, "  %-17s %-35s %s\n", name, display, status);
}

static sc_status get_optional_config_prop(const sc_config *config,
                                          const char *path,
                                          sc_allocator *alloc,
                                          sc_string *out,
                                          bool *found)
{
    sc_status status;

    if (found != nullptr) {
        *found = false;
    }
    if (out != nullptr) {
        *out = (sc_string){0};
    }
    if (config == nullptr || path == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.cli.provider.invalid_argument");
    }
    if (!sc_config_has_prop(config, sc_str_from_cstr(path))) {
        return sc_status_ok();
    }
    status = sc_config_get_prop(config, sc_str_from_cstr(path), alloc, out);
    if (status.code == SC_ERR_INVALID_ARGUMENT &&
        status.error_key != nullptr &&
        strcmp(status.error_key, "sc.config.unknown_field") == 0) {
        sc_status_clear(&status);
        return sc_status_ok();
    }
    if (sc_status_is_ok(status) && found != nullptr) {
        *found = true;
    }
    return status;
}

static sc_status provider_model_path(sc_allocator *alloc,
                                     sc_str provider,
                                     const char *leaf,
                                     sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    if (leaf == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.cli.provider.path_invalid_argument");
    }

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "providers.models.");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, provider);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, ".");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, leaf);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}


static sc_status configured_provider_detail(const sc_config *config,
                                            sc_allocator *alloc,
                                            const sc_string *provider,
                                            const char *leaf,
                                            sc_string *out,
                                            bool *found)
{
    sc_string path = {0};
    sc_status status = provider_model_path(alloc, sc_string_as_str(provider), leaf, &path);

    if (sc_status_is_ok(status)) {
        status = get_optional_config_prop(config, path.ptr, alloc, out, found);
    }
    sc_string_clear(&path);
    return status;
}

static const char *cli_provider_default_credential_env(sc_str kind)
{
    if (sc_str_equal(kind, sc_str_from_cstr("anthropic"))) {
        return "ANTHROPIC_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("openai")) || sc_str_equal(kind, sc_str_from_cstr("azure-openai"))) {
        return "OPENAI_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("openrouter"))) {
        return "OPENROUTER_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("gemini"))) {
        return "GEMINI_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("copilot"))) {
        return "GITHUB_COPILOT_TOKEN";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("bedrock"))) {
        return "AWS_ACCESS_KEY_ID";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("groq"))) {
        return "GROQ_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("mistral"))) {
        return "MISTRAL_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("xai")) || sc_str_equal(kind, sc_str_from_cstr("grok"))) {
        return "XAI_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("deepseek"))) {
        return "DEEPSEEK_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("moonshot"))) {
        return "MOONSHOT_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("zai")) ||
        sc_str_equal(kind, sc_str_from_cstr("z.ai")) ||
        sc_str_equal(kind, sc_str_from_cstr("glm"))) {
        return "ZAI_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("minimax"))) {
        return "MINIMAX_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("qianfan"))) {
        return "QIANFAN_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("venice"))) {
        return "VENICE_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("vercel-ai-gateway")) || sc_str_equal(kind, sc_str_from_cstr("vercel"))) {
        return "VERCEL_AI_GATEWAY_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("cloudflare-gateway")) || sc_str_equal(kind, sc_str_from_cstr("cloudflare"))) {
        return "CLOUDFLARE_API_TOKEN";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("opencode"))) {
        return "OPENCODE_API_KEY";
    }
    if (sc_str_equal(kind, sc_str_from_cstr("synthetic"))) {
        return "SYNTHETIC_API_KEY";
    }
    return "";
}

static sc_status source_from_env_name(sc_allocator *alloc, sc_str env_name, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "env:");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, env_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static bool secret_store_has_value(const sc_config *config, sc_str path)
{
    sc_string value = {0};
    sc_status status;
    bool found = false;

    if (config == nullptr || !config->secret_store_enabled || config->secret_store == nullptr) {
        return false;
    }
    status = sc_secret_store_get(config->secret_store, path, sc_allocator_heap(), &value);
    found = sc_status_is_ok(status) && value.len > 0;
    sc_status_clear(&status);
    sc_string_secure_clear(&value);
    return found;
}

static sc_status configured_provider_credential_source(const sc_config *config,
                                                       sc_allocator *alloc,
                                                       const sc_string *provider,
                                                       sc_string *out)
{
    sc_string kind = {0};
    sc_string path = {0};
    sc_string value = {0};
    bool found = false;
    sc_status status = configured_provider_detail(config, alloc, provider, "kind", &kind, &found);

    if (sc_status_is_ok(status) && !found) {
        status = sc_string_from_str(alloc, sc_string_as_str(provider), &kind);
    }
    if (sc_status_is_ok(status)) {
        status = configured_provider_detail(config, alloc, provider, "api_key", &value, &found);
    }
    if (sc_status_is_ok(status) && found && value.len > 0) {
        status = sc_string_from_cstr(alloc, "inline:providers.models.<provider>.api_key", out);
        goto cleanup;
    }
    sc_string_secure_clear(&value);
    if (sc_status_is_ok(status)) {
        status = get_optional_config_prop(config, "provider.api_key", alloc, &value, &found);
    }
    if (sc_status_is_ok(status) && found && value.len > 0) {
        status = sc_string_from_cstr(alloc, "inline:provider.api_key", out);
        goto cleanup;
    }
    sc_string_secure_clear(&value);
    if (sc_status_is_ok(status)) {
        status = provider_model_path(alloc, sc_string_as_str(provider), "api_key", &path);
    }
    if (sc_status_is_ok(status) && secret_store_has_value(config, sc_string_as_str(&path))) {
        status = sc_string_from_cstr(alloc, "secret_store:providers.models.<provider>.api_key", out);
        goto cleanup;
    }
    sc_string_clear(&path);
    if (sc_status_is_ok(status) && secret_store_has_value(config, sc_str_from_cstr("provider.api_key"))) {
        status = sc_string_from_cstr(alloc, "secret_store:provider.api_key", out);
        goto cleanup;
    }
    if (sc_status_is_ok(status)) {
        status = configured_provider_detail(config, alloc, provider, "credential_env", &value, &found);
    }
    if (sc_status_is_ok(status) && (!found || value.len == 0)) {
        sc_string_clear(&value);
        status = sc_string_from_cstr(alloc, cli_provider_default_credential_env(sc_string_as_str(&kind)), &value);
    }
    if (sc_status_is_ok(status) && value.len > 0) {
        status = source_from_env_name(alloc, sc_string_as_str(&value), out);
        goto cleanup;
    }
    sc_string_clear(&value);
    if (sc_status_is_ok(status)) {
        status = sc_config_provider_credential_source(config, alloc, out);
    }

cleanup:
    sc_string_secure_clear(&value);
    sc_string_clear(&path);
    sc_string_clear(&kind);
    return status;
}

int sc_app_run_provider_set_key()
{
    sc_allocator *alloc = sc_allocator_heap();
    const char *config_path = getenv("SMOLCLAW_CONFIG");
    const char *env_value = nullptr;
    sc_string config_body = {0};
    sc_string provider = {0};
    sc_string kind = {0};
    sc_string credential_env = {0};
    sc_string secret_path = {0};
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_config_load_options load = {0};
    sc_secret_store *secret_store = nullptr;
    bool found = false;
    sc_status status;
    int exit_code = 1;

    if (config_path == nullptr || config_path[0] == '\0') {
        config_path = "smolclaw.toml";
    }
    status = sc_app_read_text_file(alloc, config_path, &config_body);
    if (sc_status_is_ok(status)) {
        load.explicit_file = (sc_config_source){
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = sc_str_from_cstr(config_path),
            .body = sc_string_as_str(&config_body),
            .present = true,
        };
        status = sc_config_load(alloc, &load, &config, &diag);
    }
    if (sc_status_is_ok(status)) {
        status = get_optional_config_prop(&config, "providers.fallback", alloc, &provider, &found);
    }
    if (sc_status_is_ok(status) && !found) {
        status = sc_string_from_str(alloc, sc_string_as_str(&config.provider_default), &provider);
    }
    if (sc_status_is_ok(status)) {
        status = configured_provider_detail(&config, alloc, &provider, "kind", &kind, &found);
    }
    if (sc_status_is_ok(status) && !found) {
        status = sc_string_from_str(alloc, sc_string_as_str(&provider), &kind);
    }
    if (sc_status_is_ok(status)) {
        status = configured_provider_detail(&config, alloc, &provider, "credential_env", &credential_env, &found);
    }
    if (sc_status_is_ok(status) && (!found || credential_env.len == 0)) {
        sc_string_clear(&credential_env);
        status = sc_string_from_cstr(alloc, cli_provider_default_credential_env(sc_string_as_str(&kind)), &credential_env);
    }
    if (sc_status_is_ok(status) && credential_env.len == 0) {
        status = sc_status_invalid_argument("sc.cli.provider.credential_env_missing");
    }
    if (sc_status_is_ok(status)) {
        env_value = getenv(credential_env.ptr);
        if (env_value == nullptr || env_value[0] == '\0') {
            status = sc_status_invalid_argument("sc.cli.provider.credential_env_empty");
        }
    }
    if (sc_status_is_ok(status)) {
        status = provider_model_path(alloc, sc_string_as_str(&provider), "api_key", &secret_path);
    }
    if (sc_status_is_ok(status)) {
        status = sc_secret_store_file_new(alloc, sc_str_from_parts(nullptr, 0), &secret_store);
    }
    if (sc_status_is_ok(status)) {
        status = sc_secret_store_put(secret_store, sc_string_as_str(&secret_path), sc_str_from_cstr(env_value));
    }
    if (sc_status_is_ok(status)) {
        (void)fprintf(stdout,
                      "Updated encrypted API key for provider '%s' from environment variable %s.\n",
                      provider.ptr,
                      credential_env.ptr);
        exit_code = 0;
    } else {
        sc_app_print_bootstrap_failure(stderr, "provider set-key", &status);
    }

    sc_status_clear(&status);
    sc_secret_store_destroy(secret_store);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    sc_string_clear(&secret_path);
    sc_string_clear(&credential_env);
    sc_string_clear(&kind);
    sc_string_clear(&provider);
    sc_string_clear(&config_body);
    return exit_code;
}

int sc_app_run_provider()
{
    sc_allocator *alloc = sc_allocator_heap();
    const char *config_path = getenv("SMOLCLAW_CONFIG");
    bool explicit_config = config_path != nullptr && config_path[0] != '\0';
    bool config_present = false;
    sc_string config_body = {0};
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_config_load_options load = {0};
    sc_status status;
    sc_string configured = {0};
    sc_string model = {0};
    sc_string credential = {0};
    bool found = false;
    bool detail_found = false;

    if (!explicit_config) {
        config_path = "smolclaw.toml";
    }
    config_present = sc_app_file_exists(config_path);

    if (config_present) {
        status = sc_app_read_text_file(alloc, config_path, &config_body);
        if (sc_status_is_ok(status)) {
            load.explicit_file = (sc_config_source){
                .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
                .source_path = sc_str_from_cstr(config_path),
                .body = sc_string_as_str(&config_body),
                .present = true,
            };
            status = sc_config_load(alloc, &load, &config, &diag);
        }
    } else {
        status = sc_config_load(alloc, nullptr, &config, &diag);
    }

    if (!sc_status_is_ok(status)) {
        (void)fprintf(stdout, "SmolClaw providers\n");
        (void)fprintf(stdout, "config: %s (%s)\n", config_path, config_present ? "failed" : "missing");
        (void)fprintf(stdout,
                      "config.error: %s",
                      status.error_key == nullptr ? "sc.cli.provider.config_failed" : status.error_key);
        if (diag.path.ptr != nullptr || diag.source_path.ptr != nullptr || diag.line != 0U || diag.column != 0U) {
            (void)fprintf(stdout,
                          " (%s:%zu:%zu %s)",
                          diag.source_path.ptr == nullptr ? config_path : diag.source_path.ptr,
                          diag.line,
                          diag.column,
                          diag.path.ptr == nullptr ? "" : diag.path.ptr);
        }
        (void)fprintf(stdout, "\nstatus: failed\n");
        sc_config_diag_clear(&diag);
        sc_config_clear(&config);
        sc_string_clear(&config_body);
        sc_status_clear(&status);
        return 1;
    }

    status = get_optional_config_prop(&config, "providers.fallback", alloc, &configured, &found);
    if (sc_status_is_ok(status) && !found) {
        status = sc_string_from_str(alloc, sc_string_as_str(&config.provider_default), &configured);
    }
    if (sc_status_is_ok(status)) {
        status = configured_provider_detail(&config, alloc, &configured, "model", &model, &detail_found);
    }
    if (sc_status_is_ok(status) && !detail_found) {
        status = sc_string_from_str(alloc, sc_string_as_str(&config.provider_default_model), &model);
    }
    if (sc_status_is_ok(status)) {
        status = configured_provider_credential_source(&config, alloc, &configured, &credential);
    }

    if (!sc_status_is_ok(status)) {
        (void)fprintf(stdout, "SmolClaw providers\n");
        (void)fprintf(stdout, "config: %s (%s)\n", config_path, config_present ? "found" : "defaults");
        (void)fprintf(stdout,
                      "provider.error: %s\n",
                      status.error_key == nullptr ? "sc.cli.provider.summary_failed" : status.error_key);
        (void)fprintf(stdout, "status: failed\n");
        sc_config_diag_clear(&diag);
        sc_config_clear(&config);
        sc_string_clear(&config_body);
        sc_string_clear(&configured);
        sc_string_clear(&model);
        sc_string_clear(&credential);
        sc_status_clear(&status);
        return 1;
    }

    (void)fprintf(stdout, "SmolClaw providers\n");
    (void)fprintf(stdout, "config: %s (%s)\n", config_path, config_present ? "found" : "defaults");
    (void)fprintf(stdout,
                  "configured_provider: %s (%s)\n",
                  configured.ptr == nullptr ? "" : configured.ptr,
                  provider_status(configured.ptr));
    (void)fprintf(stdout, "configured_model: %s\n", model.ptr == nullptr ? "" : model.ptr);
    (void)fprintf(stdout, "credential_source: %s\n", credential.ptr == nullptr ? "" : credential.ptr);
    (void)fprintf(stdout, "reliability:\n");
    (void)fprintf(stdout, "  max_retries: %lld\n", (long long)config.reliability_max_retries);
    (void)fprintf(stdout, "  retry_backoff_ms: %lld\n", (long long)config.reliability_retry_backoff_ms);
    (void)fprintf(stdout, "  timeout_ms: %lld\n", (long long)config.reliability_timeout_ms);
    (void)fprintf(stdout, "available_providers:\n");
    print_provider_adapter(stdout, "mock", "Deterministic test provider", provider_status("mock"));
    print_provider_adapter(stdout, "openai", "OpenAI chat completions", provider_status("openai"));
    print_provider_adapter(stdout, "openai-compatible", "OpenAI-compatible HTTP adapter", provider_status("openai-compatible"));
    print_provider_adapter(stdout, "anthropic", "Anthropic Messages adapter", provider_status("anthropic"));
    print_provider_adapter(stdout, "gemini", "Gemini generateContent adapter", provider_status("gemini"));
    print_provider_adapter(stdout, "ollama", "Ollama local chat adapter", provider_status("ollama"));
    print_provider_adapter(stdout, "openrouter", "OpenRouter via OpenAI-compatible adapter", provider_status("openrouter"));
    print_provider_adapter(stdout, "bedrock", "Amazon Bedrock Converse adapter", provider_status("bedrock"));
    print_provider_adapter(stdout, "azure-openai", "Azure OpenAI adapter", provider_status("azure-openai"));
    print_provider_adapter(stdout, "llamacpp", "llama.cpp local HTTP adapter", provider_status("llamacpp"));
    print_provider_adapter(stdout, "sglang", "SGLang local HTTP adapter", provider_status("sglang"));
    print_provider_adapter(stdout, "vllm", "vLLM local HTTP adapter", provider_status("vllm"));
    print_provider_adapter(stdout, "gemini-cli", "Gemini CLI process adapter", provider_status("gemini-cli"));
    print_provider_adapter(stdout, "copilot", "GitHub Copilot process adapter", provider_status("copilot"));
    print_provider_adapter(stdout, "claude-code", "Claude Code MCP adapter", provider_status("claude-code"));
    print_provider_adapter(stdout, "kilocli", "KiloCLI process adapter", provider_status("kilocli"));
    (void)fprintf(stdout, "status: ok\n");

    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    sc_string_clear(&config_body);
    sc_string_clear(&configured);
    sc_string_clear(&model);
    sc_string_clear(&credential);
    sc_status_clear(&status);
    return 0;
}
