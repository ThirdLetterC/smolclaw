// cppcheck-suppress-file redundantInitialization
#include "config/config_internal.h"

#include "sc/url.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static sc_status set_diag(sc_config_diag *diag,
                          sc_allocator *alloc,
                          sc_str path,
                          sc_str source_path,
                          size_t line,
                          size_t column,
                          const char *error_key);
static sc_status set_prop_from_text(sc_config *config,
                                    sc_str path,
                                    sc_str value,
                                    sc_config_source_kind kind,
                                    sc_config_diag *diag);
static sc_status set_source(sc_config *config, sc_str path, sc_config_source_kind kind, sc_str source_path);
static sc_status set_value(sc_config *config, sc_str path, sc_str value, bool secret);
static sc_str normalize_path(sc_str path);
static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out, bool secret);
static sc_status int_from_text(sc_str input, int64_t *out);
static sc_status bool_from_text(sc_str input, bool *out);
static sc_status string_from_int(sc_allocator *alloc, int64_t value, sc_string *out);
static sc_status string_from_bool(sc_allocator *alloc, bool value, sc_string *out);
static sc_str source_kind_name(sc_config_source_kind kind);
static bool is_empty(sc_string string);
static bool path_looks_secret(sc_str path);
static sc_status validate_int_at_least(const sc_config *config, sc_config_diag *diag, const char *path, int64_t min_value);
static sc_status validate_int_between(const sc_config *config,
                                      sc_config_diag *diag,
                                      const char *path,
                                      int64_t min_value,
                                      int64_t max_value);
static sc_status validate_lightpanda_url(const sc_config *config,
                                         sc_config_diag *diag,
                                         const char *path,
                                         const char *scheme_a,
                                         const char *scheme_b);
static bool config_url_is_loopback(const sc_url *url);
static void clear_string_map(sc_map *map, sc_allocator *alloc, bool secure);
static void provider_alias_clear(sc_provider_alias *alias);
static void proxy_rule_clear(sc_proxy_rule *rule);
static void mcp_server_clear(sc_mcp_server *server);
static sc_status apply_mcp_server_prop(sc_config *config, sc_str path, sc_str value);
static sc_mcp_server *mcp_server_for_index(sc_config *config, size_t index);
static bool parse_mcp_server_path(sc_str path, size_t *index, sc_str *field);
static sc_mcp_server_view mcp_server_view(const sc_mcp_server *server);
void sc_config_init(sc_config *config, sc_allocator *alloc)
{
    if (config == nullptr) {
        return;
    }

    *config = (sc_config){.alloc = alloc == nullptr ? sc_allocator_heap() : alloc};
    sc_vec_init(&config->provider_aliases, config->alloc, sizeof(sc_provider_alias));
    sc_vec_init(&config->proxy_rules, config->alloc, sizeof(sc_proxy_rule));
    sc_vec_init(&config->mcp_servers, config->alloc, sizeof(sc_mcp_server));
    sc_map_init(&config->values, config->alloc);
    sc_map_init(&config->sources, config->alloc);
}

sc_status sc_config_init_defaults(sc_config *config, sc_allocator *alloc)
{
    sc_status status = sc_status_ok();

    if (config == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }

    sc_config_init(config, alloc);
    for (size_t i = 0; i < sc_config_field_count; ++i) {
        if (strstr(sc_config_fields[i].path, "<name>") != nullptr) {
            continue;
        }
        status = set_prop_from_text(config,
                                    sc_str_from_cstr(sc_config_fields[i].path),
                                    sc_str_from_cstr(sc_config_fields[i].default_value),
                                    SC_CONFIG_SOURCE_BUILTIN_DEFAULTS,
                                    nullptr);
        if (!sc_status_is_ok(status)) {
            sc_config_clear(config);
            return status;
        }
    }

    status = sc_config_validate(config, nullptr);
    if (!sc_status_is_ok(status)) {
        sc_config_clear(config);
    }
    return status;
}

sc_status sc_config_load(sc_allocator *alloc,
                         const sc_config_load_options *options,
                         sc_config *out,
                         sc_config_diag *diag)
{
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    if (diag != nullptr) {
        *diag = (sc_config_diag){0};
    }

    status = sc_config_init_defaults(out, alloc);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (options == nullptr) {
        return sc_config_validate(out, diag);
    }

#define APPLY_SOURCE(source_field, source_kind)                                                            \
    do {                                                                                                   \
        if (options->source_field.present) {                                                               \
            sc_toml_source source = {0};                                                                   \
            sc_toml_diag toml_diag = {0};                                                                  \
            status = sc_toml_parse_source(out->alloc,                                                      \
                                          options->source_field.source_path,                                \
                                          options->source_field.body,                                       \
                                          &source,                                                         \
                                          &toml_diag);                                                     \
            if (!sc_status_is_ok(status)) {                                                                \
                (void)set_diag(diag,                                                                       \
                               out->alloc,                                                                \
                               sc_str_from_parts(nullptr, 0),                                                 \
                               sc_string_as_str(&toml_diag.source_path),                                   \
                               toml_diag.line,                                                            \
                               toml_diag.column,                                                          \
                               toml_diag.error_key);                                                       \
                sc_toml_diag_clear(&toml_diag);                                                           \
                sc_config_clear(out);                                                                      \
                return status;                                                                            \
            }                                                                                              \
            status = sc_config_apply_toml_source(out, &source, source_kind, diag);                         \
            sc_toml_source_clear(&source);                                                                \
            if (!sc_status_is_ok(status)) {                                                                \
                sc_config_clear(out);                                                                      \
                return status;                                                                            \
            }                                                                                              \
        }                                                                                                  \
    } while (0)

    APPLY_SOURCE(default_file, SC_CONFIG_SOURCE_DEFAULT_FILE);
    APPLY_SOURCE(workspace_marker, SC_CONFIG_SOURCE_WORKSPACE_MARKER);
    APPLY_SOURCE(explicit_file, SC_CONFIG_SOURCE_EXPLICIT_FILE);

#undef APPLY_SOURCE

    for (size_t i = 0; i < options->env_override_count; ++i) {
        status = sc_config_apply_override(out,
                                          SC_CONFIG_SOURCE_ENVIRONMENT,
                                          options->env_overrides[i].path,
                                          options->env_overrides[i].value,
                                          diag);
        if (!sc_status_is_ok(status)) {
            sc_config_clear(out);
            return status;
        }
    }
    for (size_t i = 0; i < options->runtime_override_count; ++i) {
        status = sc_config_apply_override(out,
                                          SC_CONFIG_SOURCE_RUNTIME,
                                          options->runtime_overrides[i].path,
                                          options->runtime_overrides[i].value,
                                          diag);
        if (!sc_status_is_ok(status)) {
            sc_config_clear(out);
            return status;
        }
    }

    status = sc_config_validate(out, diag);
    if (!sc_status_is_ok(status)) {
        sc_config_clear(out);
    }
    return status;
}

sc_status sc_config_apply_toml_source(sc_config *config,
                                      const sc_toml_source *source,
                                      sc_config_source_kind kind,
                                      sc_config_diag *diag)
{
    if (config == nullptr || source == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }

    for (size_t i = 0; i < source->values.cap; ++i) {
        const sc_map_entry *entry = &source->values.entries[i];
        const sc_config_field *field = nullptr;
        const sc_toml_value *value = nullptr;
        sc_str path = {0};
        sc_string text = {0};
        sc_status status = sc_status_ok();

        if (!entry->occupied) {
            continue;
        }

        path = normalize_path(sc_string_as_str(&entry->key));
        value = entry->value;
        field = sc_config_field_lookup(path);
        if (field != nullptr && ((field->type == SC_CONFIG_FIELD_STRING && value->type != SC_TOML_STRING && value->type != SC_TOML_RAW &&
                               value->type != SC_TOML_ARRAY) ||
                              (field->type == SC_CONFIG_FIELD_INT && value->type != SC_TOML_INTEGER) ||
                              (field->type == SC_CONFIG_FIELD_BOOL && value->type != SC_TOML_BOOL))) {
            return set_diag(diag,
                            config->alloc,
                            path,
                            sc_string_as_str(&source->source_path),
                            value->line,
                            value->column,
                            "sc.config.invalid_type");
        }

        if (value->type == SC_TOML_STRING) {
            status = sc_string_from_str(config->alloc, sc_string_as_str(&value->string_value), &text);
        } else if (value->type == SC_TOML_INTEGER) {
            status = string_from_int(config->alloc, value->integer_value, &text);
        } else if (value->type == SC_TOML_BOOL) {
            status = string_from_bool(config->alloc, value->bool_value, &text);
        } else {
            status = sc_string_from_str(config->alloc, sc_string_as_str(&value->string_value), &text);
        }
        if (!sc_status_is_ok(status)) {
            return status;
        }

        status = set_prop_from_text(config, path, sc_string_as_str(&text), kind, diag);
        if (sc_status_is_ok(status)) {
            status = set_source(config, path, kind, sc_string_as_str(&source->source_path));
        }
        sc_string_clear(&text);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }

    return sc_status_ok();
}

sc_status sc_config_apply_override(sc_config *config,
                                   sc_config_source_kind kind,
                                   sc_str path,
                                   sc_str value,
                                   sc_config_diag *diag)
{
    return set_prop_from_text(config, normalize_path(path), value, kind, diag);
}

sc_status sc_config_get_prop(const sc_config *config, sc_str path, sc_allocator *alloc, sc_string *out)
{
    const sc_string *stored = nullptr;
    if (config == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    path = normalize_path(path);

    if (sc_str_equal(path, sc_str_from_cstr("schema.version"))) {
        return string_from_int(alloc, (int64_t)config->schema_version, out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("runtime.autonomy_level"))) {
        return sc_string_from_str(alloc, sc_string_as_str(&config->runtime_autonomy_level), out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("runtime.workspace_path"))) {
        return sc_string_from_str(alloc, sc_string_as_str(&config->runtime_workspace_path), out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("provider.default"))) {
        return sc_string_from_str(alloc, sc_string_as_str(&config->provider_default), out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("provider.default_model"))) {
        return sc_string_from_str(alloc, sc_string_as_str(&config->provider_default_model), out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("provider.api_key"))) {
        return sc_string_from_str(alloc, sc_string_as_str(&config->provider_api_key), out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("provider.api_key_env"))) {
        return sc_string_from_str(alloc, sc_string_as_str(&config->provider_api_key_env), out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("model.route_default"))) {
        return sc_string_from_str(alloc, sc_string_as_str(&config->model_route_default), out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("reliability.max_retries"))) {
        return string_from_int(alloc, config->reliability_max_retries, out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("reliability.retry_backoff_ms"))) {
        return string_from_int(alloc, config->reliability_retry_backoff_ms, out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("reliability.timeout_ms"))) {
        return string_from_int(alloc, config->reliability_timeout_ms, out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("memory.backend"))) {
        return sc_string_from_str(alloc, sc_string_as_str(&config->memory_backend), out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("gateway.bind"))) {
        return sc_string_from_str(alloc, sc_string_as_str(&config->gateway_bind), out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("gateway.port"))) {
        return string_from_int(alloc, config->gateway_port, out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("proxy.enabled"))) {
        return string_from_bool(alloc, config->proxy_enabled, out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("proxy.default_url"))) {
        return sc_string_from_str(alloc, sc_string_as_str(&config->proxy_default_url), out);
    }
    if (sc_str_equal(path, sc_str_from_cstr("mcp.deferred_loading"))) {
        return string_from_bool(alloc, config->mcp_deferred_loading, out);
    }

    stored = sc_map_get(&config->values, path);
    if (stored != nullptr) {
        return sc_string_from_str(alloc, sc_string_as_str(stored), out);
    }

    return sc_status_invalid_argument("sc.config.unknown_field");
}

bool sc_config_has_prop(const sc_config *config, sc_str path)
{
    if (config == nullptr) {
        return false;
    }
    path = normalize_path(path);
    return sc_config_field_lookup(path) != nullptr || sc_map_contains(&config->values, path);
}

bool sc_config_get_bool(const sc_config *config, sc_str path, bool fallback)
{
    sc_string value = {0};
    bool parsed = fallback;
    if (sc_status_is_ok(sc_config_get_prop(config, path, nullptr, &value))) {
        bool tmp = false;
        if (bool_from_text(sc_string_as_str(&value), &tmp).code == SC_OK) {
            parsed = tmp;
        }
    }
    sc_string_clear(&value);
    return parsed;
}

int64_t sc_config_get_int(const sc_config *config, sc_str path, int64_t fallback)
{
    sc_string value = {0};
    int64_t parsed = fallback;
    if (sc_status_is_ok(sc_config_get_prop(config, path, nullptr, &value))) {
        int64_t tmp = 0;
        if (int_from_text(sc_string_as_str(&value), &tmp).code == SC_OK) {
            parsed = tmp;
        }
    }
    sc_string_clear(&value);
    return parsed;
}

sc_status sc_config_get_prop_redacted(const sc_config *config,
                                      sc_str path,
                                      sc_allocator *alloc,
                                      sc_string *out)
{
    path = normalize_path(path);
    if (sc_config_field_is_secret(path) || path_looks_secret(path)) {
        return sc_string_redacted(alloc, out);
    }
    return sc_config_get_prop(config, path, alloc, out);
}

sc_status sc_config_export_redacted(const sc_config *config, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (config == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.config.export_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < sc_config_field_count; i += 1) {
        sc_string value = {0};
        sc_str path = sc_str_from_cstr(sc_config_fields[i].path);
        if (strstr(sc_config_fields[i].path, "<name>") != nullptr) {
            continue;
        }
        status = sc_config_get_prop_redacted(config, path, alloc, &value);
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, path);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " = ");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&value));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\n");
        }
        sc_string_clear(&value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

sc_status sc_config_attach_secret_store(sc_config *config, sc_secret_store *store, bool enabled)
{
    if (config == nullptr) {
        return sc_status_invalid_argument("sc.config.secret_store_invalid_argument");
    }
    config->secret_store = store;
    config->secret_store_enabled = enabled;
    return sc_status_ok();
}

sc_status sc_config_get_prop_source(const sc_config *config,
                                    sc_str path,
                                    sc_allocator *alloc,
                                    sc_string *out)
{
    const sc_string *source = nullptr;

    if (config == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    source = sc_map_get(&config->sources, normalize_path(path));
    if (source == nullptr) {
        return sc_status_invalid_argument("sc.config.unknown_field");
    }
    return sc_string_from_str(alloc, sc_string_as_str(source), out);
}

sc_status sc_config_set_prop(sc_config *config, sc_str path, sc_str value)
{
    return set_prop_from_text(config, normalize_path(path), value, SC_CONFIG_SOURCE_RUNTIME, nullptr);
}

sc_status sc_config_validate(const sc_config *config, sc_config_diag *diag)
{
    sc_status status = sc_status_ok();

    if (config == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    if (config->schema_version == 0 || config->schema_version > SC_CONFIG_SCHEMA_VERSION_CURRENT) {
        return set_diag(diag,
                        config->alloc,
                        sc_str_from_cstr("schema.version"),
                        sc_str_from_parts(nullptr, 0),
                        0,
                        0,
                        "sc.config.unsupported_schema_version");
    }
    if (is_empty(config->runtime_autonomy_level) || is_empty(config->provider_default) ||
        is_empty(config->provider_default_model) || is_empty(config->memory_backend) ||
        is_empty(config->gateway_bind)) {
        return set_diag(diag,
                        config->alloc,
                        sc_str_from_parts(nullptr, 0),
                        sc_str_from_parts(nullptr, 0),
                        0,
                        0,
                        "sc.config.required_field_empty");
    }
    if (config->gateway_port <= 0 || config->gateway_port > 65535) {
        return set_diag(diag,
                        config->alloc,
                        sc_str_from_cstr("gateway.port"),
                        sc_str_from_parts(nullptr, 0),
                        0,
                        0,
                        "sc.config.invalid_port");
    }
    if (config->reliability_max_retries < 0 ||
        config->reliability_retry_backoff_ms < 0 ||
        config->reliability_timeout_ms <= 0) {
        return set_diag(diag,
                        config->alloc,
                        sc_str_from_cstr("reliability"),
                        sc_str_from_parts(nullptr, 0),
                        0,
                        0,
                        "sc.config.invalid_reliability");
    }
    status = validate_int_at_least(config, diag, "agent.max_history_messages", 0);
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "agent.consecutive_message_limit", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "agent.max_tool_iterations", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "agent.max_tool_result_chars", 0);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "agent.max_system_prompt_chars", 0);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "security.sandbox.memory_limit_mb", 0);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "security.sandbox.max_subprocesses", 0);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "gateway.max_body_bytes", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "gateway.rate_limit", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "gateway.timeout_ms", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "tools.http.max_body_bytes", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "tools.web_search.max_response_bytes", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "browser.lightpanda.command_timeout_ms", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "browser.lightpanda.navigation_timeout_ms", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "browser.lightpanda.max_message_bytes", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "browser.screenshot.max_bytes", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_lightpanda_url(config,
                                         diag,
                                         "browser.lightpanda.cdp_url",
                                         "ws",
                                         "wss");
    }
    if (sc_status_is_ok(status)) {
        status = validate_lightpanda_url(config,
                                         diag,
                                         "browser.lightpanda.version_url",
                                         "http",
                                         "https");
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "media.asr.whisper.timeout_ms", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "media.asr.whisper.max_audio_bytes", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "media.asr.whisper.max_response_bytes", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "media.tts.piper.timeout_ms", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "media.tts.piper.max_audio_bytes", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "channels.max_seen_message_ids", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "channels.telegram.approval_timeout_secs", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "channels.telegram.draft_update_interval_ms", 0);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "channels.telegram.poll_timeout_seconds", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "channels.telegram.message_split_bytes", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "channels.webhook.max_body_bytes", 1);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_between(config, diag, "channels.webhook.port", 1, 65535);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_between(config, diag, "channels.rabbitmq.prefetch", 1, 65535);
    }
    if (sc_status_is_ok(status)) {
        status = validate_int_at_least(config, diag, "channels.mail.max_message_bytes", 1);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    for (size_t i = 0; i < config->provider_aliases.len; ++i) {
        const sc_provider_alias *alias = sc_vec_at_const(&config->provider_aliases, i);
        if (alias == nullptr || is_empty(alias->alias) || is_empty(alias->provider) || is_empty(alias->model)) {
            return set_diag(diag,
                            config->alloc,
                            sc_str_from_cstr("provider.aliases"),
                            sc_str_from_parts(nullptr, 0),
                            0,
                            0,
                            "sc.config.invalid_provider_alias");
        }
    }
    for (size_t i = 0; i < config->mcp_servers.len; ++i) {
        const sc_mcp_server *server = sc_vec_at_const(&config->mcp_servers, i);
        if (server == nullptr || !server->enabled) {
            continue;
        }
        if (is_empty(server->name) || server->transport == SC_MCP_TRANSPORT_UNKNOWN) {
            return set_diag(diag,
                            config->alloc,
                            sc_str_from_cstr("mcp.servers"),
                            sc_str_from_parts(nullptr, 0),
                            0,
                            0,
                            "sc.config.invalid_mcp_server");
        }
        if (server->transport == SC_MCP_TRANSPORT_STDIO && is_empty(server->command)) {
            return set_diag(diag,
                            config->alloc,
                            sc_str_from_cstr("mcp.servers.command"),
                            sc_str_from_parts(nullptr, 0),
                            0,
                            0,
                            "sc.config.invalid_mcp_stdio_server");
        }
        if ((server->transport == SC_MCP_TRANSPORT_SSE || server->transport == SC_MCP_TRANSPORT_HTTP) &&
            is_empty(server->url)) {
            return set_diag(diag,
                            config->alloc,
                            sc_str_from_cstr("mcp.servers.url"),
                            sc_str_from_parts(nullptr, 0),
                            0,
                            0,
                            "sc.config.invalid_mcp_remote_server");
        }
        for (size_t j = i + 1; j < config->mcp_servers.len; ++j) {
            const sc_mcp_server *other = sc_vec_at_const(&config->mcp_servers, j);
            if (other != nullptr && other->enabled &&
                sc_str_equal(sc_string_as_str(&server->name), sc_string_as_str(&other->name))) {
                return set_diag(diag,
                                config->alloc,
                                sc_str_from_cstr("mcp.servers.name"),
                                sc_str_from_parts(nullptr, 0),
                                0,
                                0,
                                "sc.config.duplicate_mcp_server");
            }
        }
    }
    return sc_status_ok();
}

sc_status sc_config_migrate(sc_config *config, uint32_t from_version, sc_config_diag *diag)
{
    (void)diag;
    if (config == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    if (from_version > SC_CONFIG_SCHEMA_VERSION_CURRENT) {
        return sc_status_unsupported("sc.config.future_schema_version");
    }
    config->schema_version = SC_CONFIG_SCHEMA_VERSION_CURRENT;
    return sc_config_validate(config, diag);
}

sc_status sc_config_describe_fields(sc_allocator *alloc, bool include_secret_paths, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; i < sc_config_field_count; ++i) {
        const sc_config_field *field = &sc_config_fields[i];
        if (field->secret && !include_secret_paths) {
            continue;
        }
        status = sc_string_builder_append_cstr(&builder, field->path);
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " - ");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, field->doc);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, " default=");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, field->secret ? "[REDACTED]" : field->default_value);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\n");
        }
        if (!sc_status_is_ok(status)) {
            sc_string_builder_clear(&builder);
            return status;
        }
    }
    return sc_string_builder_finish(&builder, out);
}

sc_status sc_config_add_provider_alias(sc_config *config,
                                       sc_str alias,
                                       sc_str provider,
                                       sc_str model,
                                       sc_str credential_env)
{
    sc_provider_alias item = {0};
    sc_status status = sc_status_ok();

    if (config == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    status = sc_string_from_str(config->alloc, alias, &item.alias);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(config->alloc, provider, &item.provider);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(config->alloc, model, &item.model);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(config->alloc, credential_env, &item.credential_env);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&config->provider_aliases, &item);
    }
    if (!sc_status_is_ok(status)) {
        provider_alias_clear(&item);
    }
    return status;
}

bool sc_config_resolve_provider_alias(const sc_config *config, sc_str alias, sc_provider_resolved *out)
{
    if (config == nullptr || out == nullptr) {
        return false;
    }
    for (size_t i = 0; i < config->provider_aliases.len; ++i) {
        const sc_provider_alias *item = sc_vec_at_const(&config->provider_aliases, i);
        if (item != nullptr && sc_str_equal(sc_string_as_str(&item->alias), alias)) {
            *out = (sc_provider_resolved){
                .provider = sc_string_as_str(&item->provider),
                .model = sc_string_as_str(&item->model),
                .credential_env = sc_string_as_str(&item->credential_env),
            };
            return true;
        }
    }

    if (sc_str_equal(sc_string_as_str(&config->provider_default), alias)) {
        *out = (sc_provider_resolved){
            .provider = sc_string_as_str(&config->provider_default),
            .model = sc_string_as_str(&config->provider_default_model),
            .credential_env = sc_string_as_str(&config->provider_api_key_env),
        };
        return true;
    }
    return false;
}

sc_status sc_config_provider_credential_source(const sc_config *config, sc_allocator *alloc, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (config == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    if (!is_empty(config->provider_api_key)) {
        status = sc_string_builder_append_cstr(&builder, "inline:provider.api_key");
    } else if (!is_empty(config->provider_api_key_env)) {
        status = sc_string_builder_append_cstr(&builder, "env:");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&config->provider_api_key_env));
        }
    } else {
        status = sc_string_builder_append_cstr(&builder, "none");
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
        return status;
    }
    return sc_string_builder_finish(&builder, out);
}

sc_status sc_config_add_proxy_rule(sc_config *config, sc_str service, sc_str proxy_url, bool enabled)
{
    sc_proxy_rule item = {0};
    sc_status status = sc_status_ok();

    if (config == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    status = sc_string_from_str(config->alloc, service, &item.service);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(config->alloc, proxy_url, &item.proxy_url);
    }
    item.enabled = enabled;
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&config->proxy_rules, &item);
    }
    if (!sc_status_is_ok(status)) {
        proxy_rule_clear(&item);
    }
    return status;
}

sc_status sc_config_match_proxy(const sc_config *config, sc_str service, sc_allocator *alloc, sc_string *out)
{
    if (config == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    if (!config->proxy_enabled) {
        return sc_string_from_cstr(alloc, "", out);
    }
    for (size_t i = 0; i < config->proxy_rules.len; ++i) {
        const sc_proxy_rule *rule = sc_vec_at_const(&config->proxy_rules, i);
        if (rule != nullptr && rule->enabled && sc_str_equal(sc_string_as_str(&rule->service), service)) {
            return sc_string_from_str(alloc, sc_string_as_str(&rule->proxy_url), out);
        }
    }
    return sc_string_from_str(alloc, sc_string_as_str(&config->proxy_default_url), out);
}

sc_mcp_transport sc_mcp_transport_from_str(sc_str transport)
{
    if (sc_str_equal(transport, sc_str_from_cstr("stdio"))) {
        return SC_MCP_TRANSPORT_STDIO;
    }
    if (sc_str_equal(transport, sc_str_from_cstr("sse"))) {
        return SC_MCP_TRANSPORT_SSE;
    }
    if (sc_str_equal(transport, sc_str_from_cstr("http"))) {
        return SC_MCP_TRANSPORT_HTTP;
    }
    return SC_MCP_TRANSPORT_UNKNOWN;
}

sc_str sc_mcp_transport_to_str(sc_mcp_transport transport)
{
    switch (transport) {
    case SC_MCP_TRANSPORT_STDIO:
        return sc_str_from_cstr("stdio");
    case SC_MCP_TRANSPORT_SSE:
        return sc_str_from_cstr("sse");
    case SC_MCP_TRANSPORT_HTTP:
        return sc_str_from_cstr("http");
    case SC_MCP_TRANSPORT_UNKNOWN:
        return sc_str_from_cstr("unknown");
    }
    return sc_str_from_cstr("unknown");
}

sc_status sc_config_add_mcp_server(sc_config *config,
                                   sc_str name,
                                   sc_mcp_transport transport,
                                   sc_str command,
                                   sc_str args,
                                   sc_str url,
                                   sc_str headers,
                                   bool enabled,
                                   bool deferred_loading)
{
    sc_mcp_server item = {0};
    sc_status status = sc_status_ok();

    if (config == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    item.transport = transport;
    item.enabled = enabled;
    item.deferred_loading = deferred_loading;
    item.deferred_loading_set = true;
    status = sc_string_from_str(config->alloc, name, &item.name);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(config->alloc, command, &item.command);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(config->alloc, args, &item.args);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(config->alloc, url, &item.url);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(config->alloc, headers, &item.headers);
    }
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&config->mcp_servers, &item);
    }
    if (!sc_status_is_ok(status)) {
        mcp_server_clear(&item);
    }
    return status;
}

size_t sc_config_mcp_server_count(const sc_config *config)
{
    return config == nullptr ? 0 : config->mcp_servers.len;
}

bool sc_config_mcp_server_at(const sc_config *config, size_t index, sc_mcp_server_view *out)
{
    const sc_mcp_server *server = nullptr;
    if (config == nullptr || out == nullptr) {
        return false;
    }
    server = sc_vec_at_const(&config->mcp_servers, index);
    if (server == nullptr) {
        return false;
    }
    *out = mcp_server_view(server);
    return true;
}

bool sc_config_find_mcp_server(const sc_config *config, sc_str name, sc_mcp_server_view *out)
{
    if (config == nullptr || out == nullptr) {
        return false;
    }
    for (size_t i = 0; i < config->mcp_servers.len; ++i) {
        const sc_mcp_server *server = sc_vec_at_const(&config->mcp_servers, i);
        if (server != nullptr && sc_str_equal(sc_string_as_str(&server->name), name)) {
            *out = mcp_server_view(server);
            return true;
        }
    }
    return false;
}

void sc_config_diag_clear(sc_config_diag *diag)
{
    if (diag == nullptr) {
        return;
    }
    sc_string_clear(&diag->path);
    sc_string_clear(&diag->source_path);
    *diag = (sc_config_diag){0};
}

void sc_config_clear(sc_config *config)
{
    if (config == nullptr) {
        return;
    }

    sc_string_clear(&config->runtime_autonomy_level);
    sc_string_clear(&config->runtime_workspace_path);
    sc_string_clear(&config->provider_default);
    sc_string_clear(&config->provider_default_model);
    sc_string_secure_clear(&config->provider_api_key);
    sc_string_clear(&config->provider_api_key_env);
    sc_string_clear(&config->model_route_default);
    sc_string_clear(&config->memory_backend);
    sc_string_clear(&config->gateway_bind);
    sc_string_clear(&config->proxy_default_url);

    for (size_t i = 0; i < config->provider_aliases.len; ++i) {
        sc_provider_alias *alias = sc_vec_at(&config->provider_aliases, i);
        provider_alias_clear(alias);
    }
    sc_vec_clear(&config->provider_aliases);

    for (size_t i = 0; i < config->proxy_rules.len; ++i) {
        sc_proxy_rule *rule = sc_vec_at(&config->proxy_rules, i);
        proxy_rule_clear(rule);
    }
    sc_vec_clear(&config->proxy_rules);

    for (size_t i = 0; i < config->mcp_servers.len; ++i) {
        sc_mcp_server *server = sc_vec_at(&config->mcp_servers, i);
        mcp_server_clear(server);
    }
    sc_vec_clear(&config->mcp_servers);

    clear_string_map(&config->values, config->alloc, true);
    clear_string_map(&config->sources, config->alloc, false);
    *config = (sc_config){0};
}

static void clear_string_map(sc_map *map, sc_allocator *alloc, bool secure)
{
    if (map == nullptr) {
        return;
    }
    for (size_t i = 0; i < map->cap; ++i) {
        if (map->entries != nullptr && map->entries[i].occupied) {
            sc_string *value = map->entries[i].value;
            if (value != nullptr) {
                if (secure) {
                    sc_string_secure_clear(value);
                } else {
                    sc_string_clear(value);
                }
                sc_free(alloc, value, sizeof(*value), _Alignof(sc_string));
            }
        }
    }
    sc_map_clear(map);
}

static sc_status validate_int_at_least(const sc_config *config, sc_config_diag *diag, const char *path, int64_t min_value)
{
    int64_t value = 0;

    if (config == nullptr || path == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    value = sc_config_get_int(config, sc_str_from_cstr(path), min_value);
    if (value < min_value) {
        return set_diag(diag,
                        config->alloc,
                        sc_str_from_cstr(path),
                        sc_str_from_parts(nullptr, 0),
                        0,
                        0,
                        "sc.config.invalid_range");
    }
    return sc_status_ok();
}

static sc_status validate_int_between(const sc_config *config,
                                      sc_config_diag *diag,
                                      const char *path,
                                      int64_t min_value,
                                      int64_t max_value)
{
    int64_t value = 0;

    if (config == nullptr || path == nullptr || min_value > max_value) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    value = sc_config_get_int(config, sc_str_from_cstr(path), min_value);
    if (value < min_value || value > max_value) {
        return set_diag(diag,
                        config->alloc,
                        sc_str_from_cstr(path),
                        sc_str_from_parts(nullptr, 0),
                        0,
                        0,
                        "sc.config.invalid_range");
    }
    return sc_status_ok();
}

static sc_status validate_lightpanda_url(const sc_config *config,
                                         sc_config_diag *diag,
                                         const char *path,
                                         const char *scheme_a,
                                         const char *scheme_b)
{
    sc_string value = {0};
    sc_url parsed = {0};
    sc_status status;
    sc_str scheme = {0};

    if (config == nullptr || path == nullptr || scheme_a == nullptr || scheme_b == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    status = sc_config_get_prop(config, sc_str_from_cstr(path), config->alloc, &value);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        return sc_status_ok();
    }
    status = sc_url_parse(config->alloc, sc_string_as_str(&value), &parsed);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        sc_string_clear(&value);
        return set_diag(diag,
                        config->alloc,
                        sc_str_from_cstr(path),
                        sc_str_from_parts(nullptr, 0),
                        0,
                        0,
                        "sc.config.invalid_url");
    }
    scheme = sc_string_as_str(&parsed.scheme);
    if (!sc_str_equal(scheme, sc_str_from_cstr(scheme_a)) &&
        !sc_str_equal(scheme, sc_str_from_cstr(scheme_b))) {
        sc_url_clear(&parsed);
        sc_string_clear(&value);
        return set_diag(diag,
                        config->alloc,
                        sc_str_from_cstr(path),
                        sc_str_from_parts(nullptr, 0),
                        0,
                        0,
                        "sc.config.invalid_url_scheme");
    }
    if (sc_config_get_bool(config, sc_str_from_cstr("browser.lightpanda.require_loopback"), true) &&
        !config_url_is_loopback(&parsed)) {
        sc_url_clear(&parsed);
        sc_string_clear(&value);
        return set_diag(diag,
                        config->alloc,
                        sc_str_from_cstr(path),
                        sc_str_from_parts(nullptr, 0),
                        0,
                        0,
                        "sc.config.lightpanda_endpoint_not_loopback");
    }
    sc_url_clear(&parsed);
    sc_string_clear(&value);
    return sc_status_ok();
}

static bool config_url_is_loopback(const sc_url *url)
{
    sc_str host = url == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&url->host);
    if (sc_str_equal(host, sc_str_from_cstr("localhost")) ||
        sc_str_equal(host, sc_str_from_cstr("127.0.0.1"))) {
        return true;
    }
    return host.len > 4 && memcmp(host.ptr, "127.", 4) == 0;
}

static sc_status set_diag(sc_config_diag *diag,
                          sc_allocator *alloc,
                          sc_str path,
                          sc_str source_path,
                          size_t line,
                          size_t column,
                          const char *error_key)
{
    if (diag != nullptr) {
        *diag = (sc_config_diag){0};
        diag->line = line;
        diag->column = column;
        diag->error_key = error_key;
        (void)sc_string_from_str(alloc, path, &diag->path);
        (void)sc_string_from_str(alloc, source_path, &diag->source_path);
    }
    return sc_status_parse(error_key);
}

static sc_status set_prop_from_text(sc_config *config,
                                    sc_str path,
                                    sc_str value,
                                    sc_config_source_kind kind,
                                    sc_config_diag *diag)
{
    const sc_config_field *field = nullptr;
    int64_t parsed_int = 0;
    bool parsed_bool = false;
    sc_status status = sc_status_ok();

    if (config == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    path = normalize_path(path);
    field = sc_config_field_lookup(path);
    if (field == nullptr) {
        status = set_value(config, path, value, path_looks_secret(path));
        if (sc_status_is_ok(status)) {
            status = apply_mcp_server_prop(config, path, value);
        }
        if (sc_status_is_ok(status)) {
            status = set_source(config, path, kind, source_kind_name(kind));
        }
        return status;
    }
    if (field->type == SC_CONFIG_FIELD_INT) {
        status = int_from_text(value, &parsed_int);
        if (!sc_status_is_ok(status)) {
            return set_diag(diag, config->alloc, path, source_kind_name(kind), 0, 0, "sc.config.invalid_type");
        }
    } else if (field->type == SC_CONFIG_FIELD_BOOL) {
        status = bool_from_text(value, &parsed_bool);
        if (!sc_status_is_ok(status)) {
            return set_diag(diag, config->alloc, path, source_kind_name(kind), 0, 0, "sc.config.invalid_type");
        }
    }

    if (sc_str_equal(path, sc_str_from_cstr("schema.version"))) {
        config->schema_version = (uint32_t)parsed_int;
    } else if (sc_str_equal(path, sc_str_from_cstr("runtime.autonomy_level"))) {
        status = copy_string(config->alloc, value, &config->runtime_autonomy_level, false);
    } else if (sc_str_equal(path, sc_str_from_cstr("runtime.workspace_path"))) {
        status = copy_string(config->alloc, value, &config->runtime_workspace_path, false);
    } else if (path.len > strlen("logging.") &&
               memcmp(path.ptr, "logging.", strlen("logging.")) == 0) {
        status = sc_status_ok();
    } else if (sc_str_equal(path, sc_str_from_cstr("provider.default"))) {
        status = copy_string(config->alloc, value, &config->provider_default, false);
    } else if (sc_str_equal(path, sc_str_from_cstr("provider.default_model"))) {
        status = copy_string(config->alloc, value, &config->provider_default_model, false);
    } else if (sc_str_equal(path, sc_str_from_cstr("provider.api_key"))) {
        status = copy_string(config->alloc, value, &config->provider_api_key, true);
    } else if (sc_str_equal(path, sc_str_from_cstr("provider.api_key_env"))) {
        status = copy_string(config->alloc, value, &config->provider_api_key_env, false);
    } else if (sc_str_equal(path, sc_str_from_cstr("model.route_default"))) {
        status = copy_string(config->alloc, value, &config->model_route_default, false);
    } else if (sc_str_equal(path, sc_str_from_cstr("reliability.max_retries"))) {
        config->reliability_max_retries = parsed_int;
    } else if (sc_str_equal(path, sc_str_from_cstr("reliability.retry_backoff_ms"))) {
        config->reliability_retry_backoff_ms = parsed_int;
    } else if (sc_str_equal(path, sc_str_from_cstr("reliability.timeout_ms"))) {
        config->reliability_timeout_ms = parsed_int;
    } else if (sc_str_equal(path, sc_str_from_cstr("memory.backend"))) {
        status = copy_string(config->alloc, value, &config->memory_backend, false);
    } else if (sc_str_equal(path, sc_str_from_cstr("gateway.bind"))) {
        status = copy_string(config->alloc, value, &config->gateway_bind, false);
    } else if (sc_str_equal(path, sc_str_from_cstr("gateway.port"))) {
        config->gateway_port = parsed_int;
    } else if (path.len > strlen("gateway.") &&
               memcmp(path.ptr, "gateway.", strlen("gateway.")) == 0) {
        status = sc_status_ok();
    } else if (sc_str_equal(path, sc_str_from_cstr("proxy.enabled"))) {
        config->proxy_enabled = parsed_bool;
    } else if (sc_str_equal(path, sc_str_from_cstr("proxy.default_url"))) {
        status = copy_string(config->alloc, value, &config->proxy_default_url, false);
    } else if (sc_str_equal(path, sc_str_from_cstr("mcp.deferred_loading"))) {
        config->mcp_deferred_loading = parsed_bool;
        for (size_t i = 0; i < config->mcp_servers.len; ++i) {
            sc_mcp_server *server = sc_vec_at(&config->mcp_servers, i);
            if (server != nullptr && !server->deferred_loading_set) {
                server->deferred_loading = parsed_bool;
            }
        }
    } else if (sc_str_equal(path, sc_str_from_cstr("autonomy.level")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.shell_enabled")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.workspace_only")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.forbidden_paths")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.shell_env_passthrough")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.auto_approve")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.always_ask")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.never_allow")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.auto_approve.tools")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.always_ask.tools")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.never_allow.tools")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.allowed_commands")) ||
               sc_str_equal(path, sc_str_from_cstr("autonomy.forbidden_commands")) ||
               (path.len > strlen("security.") && memcmp(path.ptr, "security.", strlen("security.")) == 0) ||
               (path.len > strlen("agent.") && memcmp(path.ptr, "agent.", strlen("agent.")) == 0) ||
               sc_str_equal(path, sc_str_from_cstr("tool_filter_groups")) ||
               (path.len > strlen("tools.") && memcmp(path.ptr, "tools.", strlen("tools.")) == 0) ||
               (path.len > strlen("media.") && memcmp(path.ptr, "media.", strlen("media.")) == 0) ||
               (path.len > strlen("browser.") && memcmp(path.ptr, "browser.", strlen("browser.")) == 0) ||
               (path.len > strlen("hardware.") && memcmp(path.ptr, "hardware.", strlen("hardware.")) == 0) ||
               (path.len > strlen("sop.") && memcmp(path.ptr, "sop.", strlen("sop.")) == 0) ||
               (path.len > strlen("heartbeat.") && memcmp(path.ptr, "heartbeat.", strlen("heartbeat.")) == 0) ||
               (path.len > strlen("cron.") && memcmp(path.ptr, "cron.", strlen("cron.")) == 0)) {
        status = sc_status_ok();
    } else if (path.len > strlen("channels.") &&
               memcmp(path.ptr, "channels.", strlen("channels.")) == 0) {
        status = sc_status_ok();
    } else if (path.len > strlen("providers.models.") &&
               memcmp(path.ptr, "providers.models.", strlen("providers.models.")) == 0) {
        status = sc_status_ok();
    } else {
        return set_diag(diag, config->alloc, path, source_kind_name(kind), 0, 0, "sc.config.unknown_field");
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }

    status = set_value(config, path, value, field->secret || path_looks_secret(path));
    if (sc_status_is_ok(status)) {
        status = set_source(config, path, kind, source_kind_name(kind));
    }
    return status;
}

static sc_status set_value(sc_config *config, sc_str path, sc_str value, bool secret)
{
    sc_string *existing = nullptr;
    sc_string *stored = nullptr;
    sc_status status = sc_status_ok();

    if (config == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    if (secret && config->secret_store_enabled && config->secret_store != nullptr) {
        status = sc_secret_store_put(config->secret_store, path, value);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    existing = sc_map_get(&config->values, path);
    stored = sc_alloc(config->alloc, sizeof(*stored), _Alignof(sc_string));
    if (stored == nullptr) {
        return sc_status_no_memory();
    }
    *stored = (sc_string){0};
    status = sc_string_from_str(config->alloc, value, stored);
    if (sc_status_is_ok(status)) {
        status = sc_map_put(&config->values, path, stored);
    }
    if (sc_status_is_ok(status) && existing != nullptr) {
        if (secret) {
            sc_string_secure_clear(existing);
        } else {
            sc_string_clear(existing);
        }
        sc_free(config->alloc, existing, sizeof(*existing), _Alignof(sc_string));
    }
    if (!sc_status_is_ok(status)) {
        sc_string_secure_clear(stored);
        sc_free(config->alloc, stored, sizeof(*stored), _Alignof(sc_string));
    }
    return status;
}

static sc_str normalize_path(sc_str path)
{
    if (sc_str_equal(path, sc_str_from_cstr("schema_version"))) {
        return sc_str_from_cstr("schema.version");
    }
    return path;
}

static sc_status set_source(sc_config *config, sc_str path, sc_config_source_kind kind, sc_str source_path)
{
    sc_string *existing = nullptr;
    sc_string *source = nullptr;
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    existing = sc_map_get(&config->sources, path);
    if (existing != nullptr) {
        sc_string_clear(existing);
        sc_free(config->alloc, existing, sizeof(*existing), _Alignof(sc_string));
    }

    source = sc_alloc(config->alloc, sizeof(*source), _Alignof(sc_string));
    if (source == nullptr) {
        return sc_status_no_memory();
    }
    *source = (sc_string){0};

    sc_string_builder_init(&builder, config->alloc);
    status = sc_string_builder_append(&builder, source_kind_name(kind));
    if (sc_status_is_ok(status) && source_path.len > 0) {
        status = sc_string_builder_append_cstr(&builder, ":");
    }
    if (sc_status_is_ok(status) && source_path.len > 0) {
        status = sc_string_builder_append(&builder, source_path);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, source);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_map_put(&config->sources, path, source);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(source);
        sc_free(config->alloc, source, sizeof(*source), _Alignof(sc_string));
    }
    return status;
}

static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out, bool secret)
{
    if (secret) {
        sc_string_secure_clear(out);
    } else {
        sc_string_clear(out);
    }
    return sc_string_from_str(alloc, input, out);
}

static sc_status int_from_text(sc_str input, int64_t *out)
{
    char buffer[64] = {0};
    char *end = nullptr;
    long long parsed = 0;

    if (out == nullptr || input.len == 0 || input.len >= sizeof(buffer) || input.ptr == nullptr) {
        return sc_status_parse("sc.config.invalid_int");
    }
    (void)memcpy(buffer, input.ptr, input.len);
    parsed = strtoll(buffer, &end, 10);
    if (end == buffer || *end != '\0') {
        return sc_status_parse("sc.config.invalid_int");
    }
    *out = (int64_t)parsed;
    return sc_status_ok();
}

static sc_status bool_from_text(sc_str input, bool *out)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.config.invalid_argument");
    }
    if (sc_str_equal(input, sc_str_from_cstr("true"))) {
        *out = true;
        return sc_status_ok();
    }
    if (sc_str_equal(input, sc_str_from_cstr("false"))) {
        *out = false;
        return sc_status_ok();
    }
    return sc_status_parse("sc.config.invalid_bool");
}

static sc_status string_from_int(sc_allocator *alloc, int64_t value, sc_string *out)
{
    char buffer[64] = {0};
    int written = snprintf(buffer, sizeof(buffer), "%lld", (long long)value);

    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return sc_status_no_memory();
    }
    return sc_string_from_cstr(alloc, buffer, out);
}

static sc_status string_from_bool(sc_allocator *alloc, bool value, sc_string *out)
{
    return sc_string_from_cstr(alloc, value ? "true" : "false", out);
}

static sc_str source_kind_name(sc_config_source_kind kind)
{
    switch (kind) {
    case SC_CONFIG_SOURCE_BUILTIN_DEFAULTS:
        return sc_str_from_cstr("builtin");
    case SC_CONFIG_SOURCE_DEFAULT_FILE:
        return sc_str_from_cstr("default-file");
    case SC_CONFIG_SOURCE_WORKSPACE_MARKER:
        return sc_str_from_cstr("workspace-marker");
    case SC_CONFIG_SOURCE_EXPLICIT_FILE:
        return sc_str_from_cstr("explicit-file");
    case SC_CONFIG_SOURCE_ENVIRONMENT:
        return sc_str_from_cstr("environment");
    case SC_CONFIG_SOURCE_RUNTIME:
        return sc_str_from_cstr("runtime");
    }
    return sc_str_from_cstr("unknown");
}

static bool is_empty(sc_string string)
{
    return string.len == 0;
}

static bool path_looks_secret(sc_str path)
{
    static const char *needles[] = {"api_key", "apikey", "bot_token", "token", "secret", "password", "authorization", "cookie"};

    if (path.ptr == nullptr) {
        return false;
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(needles); i += 1) {
        const char *needle = needles[i];
        size_t needle_len = strlen(needle);
        if (needle_len <= path.len) {
            for (size_t offset = 0; offset + needle_len <= path.len; offset += 1) {
                if (memcmp(path.ptr + offset, needle, needle_len) == 0) {
                    return true;
                }
            }
        }
    }
    return false;
}

static void provider_alias_clear(sc_provider_alias *alias)
{
    if (alias == nullptr) {
        return;
    }
    sc_string_clear(&alias->alias);
    sc_string_clear(&alias->provider);
    sc_string_clear(&alias->model);
    sc_string_clear(&alias->credential_env);
    *alias = (sc_provider_alias){0};
}

static void proxy_rule_clear(sc_proxy_rule *rule)
{
    if (rule == nullptr) {
        return;
    }
    sc_string_clear(&rule->service);
    sc_string_clear(&rule->proxy_url);
    *rule = (sc_proxy_rule){0};
}

static void mcp_server_clear(sc_mcp_server *server)
{
    if (server == nullptr) {
        return;
    }
    sc_string_clear(&server->name);
    sc_string_clear(&server->command);
    sc_string_clear(&server->args);
    sc_string_clear(&server->url);
    sc_string_secure_clear(&server->headers);
    *server = (sc_mcp_server){0};
}

static sc_status apply_mcp_server_prop(sc_config *config, sc_str path, sc_str value)
{
    size_t index = 0;
    sc_str field = {0};
    sc_mcp_server *server = nullptr;

    if (!parse_mcp_server_path(path, &index, &field)) {
        return sc_status_ok();
    }
    server = mcp_server_for_index(config, index);
    if (server == nullptr) {
        return sc_status_no_memory();
    }
    if (sc_str_equal(field, sc_str_from_cstr("name"))) {
        return copy_string(config->alloc, value, &server->name, false);
    }
    if (sc_str_equal(field, sc_str_from_cstr("transport"))) {
        server->transport = sc_mcp_transport_from_str(value);
        return sc_status_ok();
    }
    if (sc_str_equal(field, sc_str_from_cstr("command"))) {
        return copy_string(config->alloc, value, &server->command, false);
    }
    if (sc_str_equal(field, sc_str_from_cstr("args"))) {
        return copy_string(config->alloc, value, &server->args, false);
    }
    if (sc_str_equal(field, sc_str_from_cstr("url"))) {
        return copy_string(config->alloc, value, &server->url, false);
    }
    if (sc_str_equal(field, sc_str_from_cstr("headers"))) {
        return copy_string(config->alloc, value, &server->headers, true);
    }
    if (sc_str_equal(field, sc_str_from_cstr("enabled"))) {
        return bool_from_text(value, &server->enabled);
    }
    if (sc_str_equal(field, sc_str_from_cstr("deferred_loading"))) {
        sc_status status = bool_from_text(value, &server->deferred_loading);
        if (sc_status_is_ok(status)) {
            server->deferred_loading_set = true;
        }
        return status;
    }
    return sc_status_ok();
}

static sc_mcp_server *mcp_server_for_index(sc_config *config, size_t index)
{
    if (config == nullptr) {
        return nullptr;
    }
    while (config->mcp_servers.len <= index) {
        sc_mcp_server server = {
            .transport = SC_MCP_TRANSPORT_UNKNOWN,
            .enabled = true,
            .deferred_loading = config->mcp_deferred_loading,
        };
        sc_status status = sc_vec_push(&config->mcp_servers, &server);
        if (!sc_status_is_ok(status)) {
            return nullptr;
        }
    }
    return sc_vec_at(&config->mcp_servers, index);
}

static bool parse_mcp_server_path(sc_str path, size_t *index, sc_str *field)
{
    constexpr size_t prefix_len = 12;
    size_t cursor = prefix_len;
    size_t parsed = 0;

    if (index == nullptr || field == nullptr || path.len <= prefix_len ||
        memcmp(path.ptr, "mcp.servers.", prefix_len) != 0) {
        return false;
    }
    if (path.ptr[cursor] < '0' || path.ptr[cursor] > '9') {
        return false;
    }
    while (cursor < path.len && path.ptr[cursor] >= '0' && path.ptr[cursor] <= '9') {
        size_t digit = (size_t)(path.ptr[cursor] - '0');
        if (parsed > (SIZE_MAX - digit) / 10) {
            return false;
        }
        parsed = (parsed * 10) + digit;
        cursor += 1;
    }
    if (cursor >= path.len || path.ptr[cursor] != '.' || cursor + 1 >= path.len) {
        return false;
    }
    *index = parsed;
    *field = sc_str_from_parts(&path.ptr[cursor + 1], path.len - cursor - 1);
    return true;
}

static sc_mcp_server_view mcp_server_view(const sc_mcp_server *server)
{
    if (server == nullptr) {
        return (sc_mcp_server_view){0};
    }
    return (sc_mcp_server_view){
        .name = sc_string_as_str(&server->name),
        .transport = server->transport,
        .command = sc_string_as_str(&server->command),
        .args = sc_string_as_str(&server->args),
        .url = sc_string_as_str(&server->url),
        .headers = sc_string_as_str(&server->headers),
        .enabled = server->enabled,
        .deferred_loading = server->deferred_loading,
    };
}
