#include "app/app_commands.h"
#include "app/app_internal.h"

#include "core/build_info.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdckdint.h>
#include <string.h>
#include <sys/stat.h>

#include "sc/cli.h"
#include "sc/config.h"
#include "sc/i18n.h"
#include "sc/log.h"
#include "sc/result.h"
#include "sc/security.h"

sc_string sc_app_format_key(sc_i18n_catalog *catalog,
                            const char *key,
                            const sc_i18n_arg *args,
                            size_t arg_count)
{
    sc_string out = {0};
    if (!sc_status_is_ok(sc_i18n_format(catalog,
                                        sc_str_from_cstr(key),
                                        args,
                                        arg_count,
                                        sc_allocator_heap(),
                                        &out))) {
        (void)sc_string_from_cstr(sc_allocator_heap(), key == nullptr ? "" : key, &out);
    }
    return out;
}

void sc_app_print_version(FILE *stream)
{
    sc_i18n_catalog catalog = {0};
    char abi_text[32] = {0};
    sc_i18n_arg version_arg = {.name = sc_str_from_cstr("version"), .value = sc_str_from_cstr(sc_build_version())};
    sc_i18n_arg abi_arg = {0};
    sc_string version = {0};
    sc_string abi = {0};

    (void)snprintf(abi_text, sizeof(abi_text), "%u", sc_build_abi_version());
    abi_arg = (sc_i18n_arg){.name = sc_str_from_cstr("abi"), .value = sc_str_from_cstr(abi_text)};
    sc_i18n_catalog_init(&catalog, sc_allocator_heap(), sc_str_from_cstr("en"));
    (void)sc_i18n_catalog_load_default_en(&catalog);
    version = sc_app_format_key(&catalog, "cli.version", &version_arg, 1);
    abi = sc_app_format_key(&catalog, "cli.abi", &abi_arg, 1);
    (void)fprintf(stream, "%s\n", version.ptr == nullptr ? "" : version.ptr);
    (void)fprintf(stream, "%s\n", abi.ptr == nullptr ? "" : abi.ptr);
    sc_string_clear(&abi);
    sc_string_clear(&version);
    sc_i18n_catalog_clear(&catalog);
}

static const char *bootstrap_hint_for_error_key(const char *error_key)
{
    if (error_key == nullptr) {
        return nullptr;
    }
    if (strcmp(error_key, "sc.channel_telegram.token_missing") == 0 ||
        strcmp(error_key, "sc.channel_telegram.invalid_argument") == 0) {
        return "set SMOLCLAW_TELEGRAM_BOT_TOKEN or channels.telegram.bot_token, or disable channels.telegram.enabled";
    }
    if (strcmp(error_key, "sc.provider_credential.missing_env") == 0) {
        return "export the configured provider credential environment variable, such as OPENAI_API_KEY";
    }
    if (strcmp(error_key, "sc.bootstrap.read_open_failed") == 0) {
        return "set SMOLCLAW_CONFIG to an existing config file or run `smolclaw init-config`";
    }
    return nullptr;
}

void sc_app_log_bootstrap_failure(const sc_status *status)
{
    sc_log_field error_fields[] = {
        {.key = "error_key",
         .value = sc_str_from_cstr(status == nullptr || status->error_key == nullptr ? "sc.bootstrap.failed"
                                                                                      : status->error_key),
         .secret = false},
    };

    sc_log_write(SC_LOG_ERROR, "sc.bootstrap", "bootstrap.failed", error_fields, SC_ARRAY_LEN(error_fields));
}

void sc_app_print_bootstrap_failure(FILE *stream, const char *command, const sc_status *status)
{
    const char *error_key = status == nullptr || status->error_key == nullptr ? "sc.bootstrap.failed" : status->error_key;
    const char *hint = bootstrap_hint_for_error_key(error_key);

    (void)fprintf(stream, "smolclaw: %s failed: %s\n", command, error_key);
    if (status != nullptr && status->message != nullptr && status->message[0] != '\0') {
        (void)fprintf(stream, "detail: %s\n", status->message);
    }
    if (hint != nullptr) {
        (void)fprintf(stream, "hint: %s\n", hint);
    }
}

bool sc_app_file_exists(const char *path)
{
    FILE *file = nullptr;
    bool exists = false;

    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    file = fopen(path, "rb");
    if (file != nullptr) {
        exists = true;
        (void)fclose(file);
    }
    return exists;
}

bool sc_app_env_truthy(const char *value)
{
    return value != nullptr && value[0] != '\0' && strcmp(value, "0") != 0 && strcmp(value, "false") != 0;
}

sc_status sc_app_copy_cstr_to_buffer(char *out, size_t capacity, const char *value, const char *error_key)
{
    int written = 0;

    if (out == nullptr || capacity == 0U || value == nullptr) {
        return sc_status_invalid_argument(error_key);
    }

    written = snprintf(out, capacity, "%s", value);
    if (written < 0 || (size_t)written >= capacity) {
        return sc_status_invalid_argument(error_key);
    }
    return sc_status_ok();
}

static bool path_is_absolute_cstr(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return false;
    }
    if (path[0] == '/') {
        return true;
    }
    return false;
}

sc_status sc_app_join_cstr_path(char *out, size_t capacity, const char *left, const char *right, const char *error_key)
{
    const char *separator = "/";
    int written = 0;
    size_t left_len = 0;

    if (out == nullptr || capacity == 0U || left == nullptr || right == nullptr) {
        return sc_status_invalid_argument(error_key);
    }
    if (path_is_absolute_cstr(right)) {
        return sc_app_copy_cstr_to_buffer(out, capacity, right, error_key);
    }
    left_len = strlen(left);
    if (left_len == 0U || left[left_len - 1U] == '/') {
        separator = "";
    }
    written = snprintf(out, capacity, "%s%s%s", left_len == 0U ? "." : left, separator, right);
    if (written < 0 || (size_t)written >= capacity) {
        return sc_status_invalid_argument(error_key);
    }
    return sc_status_ok();
}

static sc_status dirname_cstr(char *out, size_t capacity, const char *path, const char *error_key)
{
    size_t len = 0;

    if (out == nullptr || capacity == 0U || path == nullptr || path[0] == '\0') {
        return sc_status_invalid_argument(error_key);
    }
    len = strlen(path);
    while (len > 0U && path[len - 1U] != '/') {
        len -= 1U;
    }
    if (len == 0U) {
        return sc_app_copy_cstr_to_buffer(out, capacity, ".", error_key);
    }
    if (len == 1U) {
        return sc_app_copy_cstr_to_buffer(out, capacity, "/", error_key);
    }
    if (len > capacity) {
        return sc_status_invalid_argument(error_key);
    }
    memcpy(out, path, len - 1U);
    out[len - 1U] = '\0';
    return sc_status_ok();
}

sc_status sc_app_cli_workspace_path(char *out, size_t capacity, const char *config_path)
{
    char config_dir[4096] = {0};
    const char *workspace_env = getenv("SMOLCLAW_WORKSPACE");
    sc_status status;

    if (sc_app_env_truthy(workspace_env)) {
        return sc_app_copy_cstr_to_buffer(out, capacity, workspace_env, "sc.cli.workspace_path_invalid");
    }

    status = dirname_cstr(config_dir, sizeof(config_dir), config_path, "sc.cli.config_dir_invalid");
    if (sc_status_is_ok(status)) {
        status = sc_app_join_cstr_path(out, capacity, config_dir, "workspace", "sc.cli.workspace_path_invalid");
    }
    return status;
}

sc_status sc_app_cli_estop_paths(char *state_dir, size_t state_dir_capacity, char *state_path, size_t state_path_capacity)
{
    char workspace[4096] = {0};
    const char *config_path = getenv("SMOLCLAW_CONFIG");
    sc_status status;

    if (config_path == nullptr || config_path[0] == '\0') {
        config_path = "smolclaw.toml";
    }
    status = sc_app_cli_workspace_path(workspace, sizeof(workspace), config_path);
    if (sc_status_is_ok(status)) {
        status = sc_app_join_cstr_path(state_dir, state_dir_capacity, workspace, "state", "sc.cli.estop_state_dir_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = sc_app_join_cstr_path(state_path,
                                state_path_capacity,
                                state_dir,
                                "emergency_stop.state",
                                "sc.cli.estop_state_path_invalid");
    }
    return status;
}

static sc_status init_cli_local_state(const char *config_path)
{
    const char *dirs[] = {"memory", "sessions", "receipts", "cache", "state", "logs"};
    char workspace[4096] = {0};
    char child[4096] = {0};
    char state_dir[4096] = {0};
    char state_path[4096] = {0};
    sc_estop_state estop = {0};
    sc_status status = sc_app_cli_workspace_path(workspace, sizeof(workspace), config_path);

    if (sc_status_is_ok(status)) {
        status = sc_app_ensure_cli_dir(workspace);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(dirs); i += 1U) {
        status = sc_app_join_cstr_path(child, sizeof(child), workspace, dirs[i], "sc.cli.local_state_path_invalid");
        if (sc_status_is_ok(status)) {
            status = sc_app_ensure_cli_dir(child);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_app_join_cstr_path(state_dir, sizeof(state_dir), workspace, "state", "sc.cli.estop_state_dir_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = sc_app_join_cstr_path(state_path,
                                       sizeof(state_path),
                                       state_dir,
                                       "emergency_stop.state",
                                       "sc.cli.estop_state_path_invalid");
    }
    if (sc_status_is_ok(status) && !sc_app_file_exists(state_path)) {
        sc_estop_init(&estop, sc_allocator_heap());
        sc_estop_reset(&estop);
        status = sc_estop_write_file(&estop, sc_str_from_cstr(state_path));
    }

    sc_estop_clear(&estop);
    return status;
}

static int cli_mkdir(const char *path)
{
    return mkdir(path, 0700);
}

sc_status sc_app_ensure_cli_dir(const char *path)
{
    char mutable_path[4096] = {0};
    size_t len = 0;

    if (path == nullptr || path[0] == '\0') {
        return sc_status_invalid_argument("sc.cli.mkdir_invalid_argument");
    }
    len = strlen(path);
    if (len >= sizeof(mutable_path)) {
        return sc_status_invalid_argument("sc.cli.mkdir_invalid_argument");
    }
    memcpy(mutable_path, path, len + 1U);

    for (size_t i = 1U; i < len; i += 1U) {
        if (mutable_path[i] != '/') {
            continue;
        }
        mutable_path[i] = '\0';
        if (mutable_path[0] != '\0' && cli_mkdir(mutable_path) != 0 && errno != EEXIST) {
            return sc_status_io("sc.cli.mkdir_failed");
        }
        mutable_path[i] = '/';
    }
    if (cli_mkdir(mutable_path) != 0 && errno != EEXIST) {
        return sc_status_io("sc.cli.mkdir_failed");
    }
    return sc_status_ok();
}

static void print_local_state(FILE *stream, const char *config_path)
{
    char workspace[4096] = {0};
    char state_dir[4096] = {0};
    char estop_path[4096] = {0};
    sc_status status = sc_app_cli_workspace_path(workspace, sizeof(workspace), config_path);

    if (!sc_status_is_ok(status)) {
        (void)snprintf(workspace, sizeof(workspace), "(invalid)");
        sc_status_clear(&status);
    }
    status = sc_app_cli_estop_paths(state_dir, sizeof(state_dir), estop_path, sizeof(estop_path));
    if (!sc_status_is_ok(status)) {
        (void)snprintf(estop_path, sizeof(estop_path), "(invalid)");
        sc_status_clear(&status);
    }

    (void)fprintf(stream, "local_state:\n");
    (void)fprintf(stream, "  config: %s\n", config_path == nullptr ? "smolclaw.toml" : config_path);
    (void)fprintf(stream, "  secrets: environment variables or configured secret fields, redacted from reports\n");
    (void)fprintf(stream, "  workspace: %s\n", workspace);
    (void)fprintf(stream, "  memory: %s/memory/brain.db when sqlite memory is enabled\n", workspace);
    (void)fprintf(stream, "  sessions: %s/sessions/\n", workspace);
    (void)fprintf(stream, "  receipts: %s/receipts/\n", workspace);
    (void)fprintf(stream, "  logs: %s/logs/smolclaw-daemon.log for daemon; stderr for foreground commands\n", workspace);
    (void)fprintf(stream, "  caches: %s/cache/\n", workspace);
    (void)fprintf(stream, "  emergency_stop: %s\n", estop_path);
}

sc_status sc_app_read_text_file(sc_allocator *alloc, const char *path, sc_string *out)
{
    FILE *file = nullptr;
    long size = 0;
    char *buffer = nullptr;
    size_t alloc_size = 0;
    size_t file_size = 0;
    size_t read_len = 0;
    sc_status status = sc_status_ok();

    if (alloc == nullptr || path == nullptr || path[0] == '\0' || out == nullptr) {
        return sc_status_invalid_argument("sc.cli.read_file.invalid_argument");
    }

    file = fopen(path, "rb");
    if (file == nullptr) {
        status = sc_status_io("sc.cli.read_file.open_failed");
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        status = sc_status_io("sc.cli.read_file.seek_failed");
        goto cleanup;
    }
    size = ftell(file);
    if (size < 0) {
        status = sc_status_io("sc.cli.read_file.tell_failed");
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        status = sc_status_io("sc.cli.read_file.seek_failed");
        goto cleanup;
    }

    file_size = (size_t)size;
    if (ckd_add(&alloc_size, file_size, 1U)) {
        status = sc_status_invalid_argument("sc.cli.read_file.too_large");
        goto cleanup;
    }

    buffer = sc_alloc(alloc, alloc_size, _Alignof(char));
    if (buffer == nullptr) {
        status = sc_status_no_memory();
        goto cleanup;
    }
    read_len = fread(buffer, 1, file_size, file);
    if (read_len != file_size) {
        status = sc_status_io("sc.cli.read_file.read_failed");
        goto cleanup;
    }
    if (fclose(file) != 0) {
        file = nullptr;
        status = sc_status_io("sc.cli.read_file.close_failed");
        goto cleanup;
    }
    file = nullptr;
    buffer[read_len] = '\0';
    *out = (sc_string){.ptr = buffer, .len = read_len, .alloc = alloc};
    buffer = nullptr;

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    if (buffer != nullptr) {
        sc_free(alloc, buffer, alloc_size, _Alignof(char));
    }
    return status;
}

sc_status sc_app_write_text_file(const char *path,
                                 sc_str body,
                                 const char *open_error_key,
                                 const char *write_error_key,
                                 const char *close_error_key)
{
    FILE *file = nullptr;
    sc_status status = sc_status_ok();

    if (path == nullptr || path[0] == '\0' || (body.len > 0U && body.ptr == nullptr)) {
        return sc_status_invalid_argument(open_error_key);
    }

    file = fopen(path, "wb");
    if (file == nullptr) {
        status = sc_status_io(open_error_key);
        goto cleanup;
    }
    if (body.len > 0U && fwrite(body.ptr, 1, body.len, file) != body.len) {
        status = sc_status_io(write_error_key);
    }
    if (fclose(file) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io(close_error_key);
    }
    file = nullptr;

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    return status;
}

static void print_feature_check(FILE *stream, const char *name)
{
    (void)fprintf(stream, "  %s: %s\n", name, sc_build_feature_enabled(name) != 0 ? "enabled" : "disabled");
}

static bool print_doctor_config_status(sc_allocator *alloc, const char *config_path)
{
    sc_string config_body = {0};
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_config_load_options load = {0};
    sc_status status = sc_app_read_text_file(alloc, config_path, &config_body);
    bool failed = false;

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
        (void)fprintf(stdout, "config.status: ok\n");
        (void)fprintf(stdout, "config.schema_version: %u\n", config.schema_version);
        (void)fprintf(stdout,
                      "config.provider_default: %s\n",
                      config.provider_default.ptr == nullptr ? "" : config.provider_default.ptr);
        (void)fprintf(stdout,
                      "config.memory_backend: %s\n",
                      config.memory_backend.ptr == nullptr ? "" : config.memory_backend.ptr);
    } else {
        failed = true;
        (void)fprintf(stdout, "config.status: failed\n");
        (void)fprintf(stdout, "config.error: %s", status.error_key == nullptr ? "sc.cli.doctor.config_failed" : status.error_key);
        if (diag.path.ptr != nullptr || diag.source_path.ptr != nullptr || diag.line != 0U || diag.column != 0U) {
            (void)fprintf(stdout,
                          " (%s:%zu:%zu %s)",
                          diag.source_path.ptr == nullptr ? config_path : diag.source_path.ptr,
                          diag.line,
                          diag.column,
                          diag.path.ptr == nullptr ? "" : diag.path.ptr);
        }
        (void)fprintf(stdout, "\n");
    }

    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    sc_string_clear(&config_body);
    sc_status_clear(&status);
    return failed;
}

int sc_app_run_doctor()
{
    sc_allocator *alloc = sc_allocator_heap();
    const char *config_path = getenv("SMOLCLAW_CONFIG");
    const char *workspace_path = getenv("SMOLCLAW_WORKSPACE");
    bool explicit_config = config_path != nullptr && config_path[0] != '\0';
    bool config_present = false;
    bool failed = false;
    sc_status local_state_status = sc_status_ok();

    if (!explicit_config) {
        config_path = "smolclaw.toml";
    }
    config_present = sc_app_file_exists(config_path);
    if (config_present) {
        local_state_status = init_cli_local_state(config_path);
        if (!sc_status_is_ok(local_state_status)) {
            failed = true;
        }
    }

    (void)fprintf(stdout, "SmolClaw doctor\n");
    (void)fprintf(stdout, "version: %s\n", sc_build_version());
    (void)fprintf(stdout, "abi: %u\n", sc_build_abi_version());
    (void)fprintf(stdout, "config: %s (%s)\n", config_path, config_present ? "found" : "missing");
    (void)fprintf(stdout,
                  "workspace: %s\n",
                  workspace_path == nullptr || workspace_path[0] == '\0' ? "(default from config path)" : workspace_path);
    (void)fprintf(stdout, "features:\n");
    print_feature_check(stdout, "SC_ENABLE_RUNTIME");
    print_feature_check(stdout, "SC_ENABLE_GATEWAY");
    print_feature_check(stdout, "SC_ENABLE_PLUGINS");
    print_feature_check(stdout, "SC_ENABLE_HARDWARE");
    print_feature_check(stdout, "SC_ENABLE_MIMALLOC");
    print_feature_check(stdout, "SC_ENABLE_JEMALLOC");
    print_feature_check(stdout, "SC_SANITIZERS");
    print_feature_check(stdout, "SC_HARDENING");
    (void)fprintf(stdout, "dependency_capabilities:\n");
    sc_build_capabilities_write(stdout);
    print_local_state(stdout, config_path);
    if (config_present) {
        (void)fprintf(stdout, "local_state.status: %s\n", sc_status_is_ok(local_state_status) ? "initialized" : "failed");
        if (!sc_status_is_ok(local_state_status)) {
            (void)fprintf(stdout,
                          "local_state.error: %s\n",
                          local_state_status.error_key == nullptr ? "sc.cli.local_state_init_failed" : local_state_status.error_key);
        }
        if (print_doctor_config_status(alloc, config_path)) {
            failed = true;
        }
    } else if (explicit_config) {
        failed = true;
        (void)fprintf(stdout, "config.status: failed\n");
        (void)fprintf(stdout, "config.error: explicit config file is missing\n");
    } else {
        (void)fprintf(stdout, "config.status: skipped\n");
    }

    (void)fprintf(stdout, "status: %s\n", failed ? "failed" : "ok");
    sc_status_clear(&local_state_status);
    return failed ? 1 : 0;
}

static const char *memory_backend_status(const char *name)
{
    if (name == nullptr) {
        return "unknown";
    }
    if (strcmp(name, "none") == 0 || strcmp(name, "markdown") == 0 || strcmp(name, "sqlite") == 0) {
        return "built-in";
    }
    return "not registered";
}

static void print_memory_backend(FILE *stream, const char *name, const char *display, const char *status)
{
    (void)fprintf(stream, "  %-8s %-18s %s\n", name, display, status);
}

static void print_config_load_error(const char *command,
                                    const char *config_path,
                                    bool config_present,
                                    const sc_status *status,
                                    const sc_config_diag *diag)
{
    (void)fprintf(stdout, "%s\n", command);
    (void)fprintf(stdout, "config: %s (%s)\n", config_path, config_present ? "failed" : "missing");
    (void)fprintf(stdout,
                  "config.error: %s",
                  status == nullptr || status->error_key == nullptr ? "sc.cli.config.load_failed" : status->error_key);
    if (diag != nullptr && (diag->path.ptr != nullptr || diag->source_path.ptr != nullptr || diag->line != 0U || diag->column != 0U)) {
        (void)fprintf(stdout,
                      " (%s:%zu:%zu %s)",
                      diag->source_path.ptr == nullptr ? config_path : diag->source_path.ptr,
                      diag->line,
                      diag->column,
                      diag->path.ptr == nullptr ? "" : diag->path.ptr);
    }
    (void)fprintf(stdout, "\nstatus: failed\n");
}

static sc_status load_cli_config(sc_allocator *alloc,
                                 const char **config_path_out,
                                 bool *config_present_out,
                                 sc_string *config_body,
                                 sc_config *config,
                                 sc_config_diag *diag)
{
    const char *config_path = getenv("SMOLCLAW_CONFIG");
    bool config_present = false;
    sc_config_load_options load = {0};
    sc_status status;

    if (alloc == nullptr || config_path_out == nullptr || config_present_out == nullptr ||
        config_body == nullptr || config == nullptr || diag == nullptr) {
        return sc_status_invalid_argument("sc.cli.config.invalid_argument");
    }

    if (config_path == nullptr || config_path[0] == '\0') {
        config_path = "smolclaw.toml";
    }
    config_present = sc_app_file_exists(config_path);

    if (config_present) {
        status = sc_app_read_text_file(alloc, config_path, config_body);
        if (sc_status_is_ok(status)) {
            load.explicit_file = (sc_config_source){
                .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
                .source_path = sc_str_from_cstr(config_path),
                .body = sc_string_as_str(config_body),
                .present = true,
            };
            status = sc_config_load(alloc, &load, config, diag);
        }
    } else {
        status = sc_config_load(alloc, nullptr, config, diag);
    }

    *config_path_out = config_path;
    *config_present_out = config_present;
    return status;
}

int sc_app_run_config_show()
{
    sc_allocator *alloc = sc_allocator_heap();
    const char *config_path = nullptr;
    bool config_present = false;
    sc_string config_body = {0};
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_string exported = {0};
    sc_status status = load_cli_config(alloc, &config_path, &config_present, &config_body, &config, &diag);

    if (sc_status_is_ok(status)) {
        status = sc_config_export_redacted(&config, alloc, &exported);
    }

    if (!sc_status_is_ok(status)) {
        print_config_load_error("SmolClaw config", config_path == nullptr ? "smolclaw.toml" : config_path, config_present, &status, &diag);
        sc_config_diag_clear(&diag);
        sc_config_clear(&config);
        sc_string_clear(&config_body);
        sc_string_clear(&exported);
        sc_status_clear(&status);
        return 1;
    }

    (void)fprintf(stdout, "SmolClaw config\n");
    (void)fprintf(stdout, "config: %s (%s)\n", config_path, config_present ? "found" : "defaults");
    (void)fprintf(stdout, "values:\n");
    (void)fprintf(stdout, "%s", exported.ptr == nullptr ? "" : exported.ptr);
    (void)fprintf(stdout, "status: ok\n");

    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    sc_string_clear(&config_body);
    sc_string_clear(&exported);
    sc_status_clear(&status);
    return 0;
}

int sc_app_run_memory()
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
    const char *configured = nullptr;

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
        (void)fprintf(stdout, "SmolClaw memory\n");
        (void)fprintf(stdout, "config: %s (%s)\n", config_path, config_present ? "failed" : "missing");
        (void)fprintf(stdout,
                      "config.error: %s",
                      status.error_key == nullptr ? "sc.cli.memory.config_failed" : status.error_key);
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

    configured = config.memory_backend.ptr == nullptr ? "" : config.memory_backend.ptr;

    (void)fprintf(stdout, "SmolClaw memory\n");
    (void)fprintf(stdout, "config: %s (%s)\n", config_path, config_present ? "found" : "defaults");
    (void)fprintf(stdout, "configured_backend: %s (%s)\n", configured, memory_backend_status(configured));
    (void)fprintf(stdout, "available_backends:\n");
    print_memory_backend(stdout, "none", "No-op memory", "built-in");
    print_memory_backend(stdout, "markdown", "Markdown memory", "built-in");
    print_memory_backend(stdout, "sqlite", "SQLite memory", "built-in");
    (void)fprintf(stdout, "status: ok\n");

    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    sc_string_clear(&config_body);
    sc_status_clear(&status);
    return 0;
}
