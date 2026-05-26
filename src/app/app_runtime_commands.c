#include "app/app_commands.h"
#include "app/app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include "sc/acp.h"
#include "sc/allocator.h"
#include "sc/bootstrap.h"
#include "sc/config.h"
#include "sc/log.h"
#include "sc/provider.h"
#include "sc/runtime.h"

typedef struct daemon_log_config {
    bool file_enabled;
    sc_log_level level;
    sc_log_format format;
    char path[4096];
} daemon_log_config;

static bool daemon_log_env_configured();
static sc_status daemon_load_log_config(const char *config_path, daemon_log_config *out);
static sc_status daemon_resolve_log_path(const sc_config *config, const char *config_path, sc_allocator *alloc, char *out, size_t capacity);
static sc_status daemon_open_log_file(const daemon_log_config *config, int *out_fd);
static int daemon_redirect_stdio(const daemon_log_config *config, int log_fd, bool background);
static int daemon_background_if_requested(const char *config_path, const daemon_log_config *log_config, int log_fd)
{
    const char *foreground = getenv("SMOLCLAW_DAEMON_FOREGROUND");
    const char *once = getenv("SMOLCLAW_ONCE");
    pid_t pid = 0;

    if (sc_app_env_truthy(foreground) || (once != nullptr && strcmp(once, "0") != 0)) {
        return 0;
    }
    if (config_path == nullptr || access(config_path, R_OK) != 0) {
        (void)fprintf(stderr, "smolclaw: daemon failed: sc.bootstrap.read_open_failed\n");
        return 1;
    }

    pid = fork();
    if (pid < 0) {
        (void)fprintf(stderr, "smolclaw: daemon failed: sc.cli.daemon.fork_failed\n");
        return 1;
    }
    if (pid > 0) {
        if (log_config != nullptr && log_config->file_enabled && log_config->path[0] != '\0') {
            (void)fprintf(stdout, "smolclaw daemon started: pid=%ld log=%s\n", (long)pid, log_config->path);
        } else {
            (void)fprintf(stdout, "smolclaw daemon started: pid=%ld\n", (long)pid);
        }
        return 2;
    }
    if (setsid() < 0) {
        _exit(1);
    }
    if (daemon_redirect_stdio(log_config, log_fd, true) != 0) {
        _exit(1);
    }
    return 3;
}

static bool daemon_log_env_configured()
{
    const char *keys[] = {
        "SMOLCLAW_LOG_LEVEL",
        "SC_LOG",
        "SMOLCLAW_LOG_FORMAT",
        "SC_LOG_FORMAT",
    };

    for (size_t i = 0; i < SC_ARRAY_LEN(keys); i += 1) {
        const char *value = getenv(keys[i]);
        if (value != nullptr && value[0] != '\0') {
            return true;
        }
    }
    return false;
}

static sc_status daemon_load_log_config(const char *config_path, daemon_log_config *out)
{
    sc_allocator *alloc = sc_allocator_heap();
    sc_string body = {0};
    sc_string level_text = {0};
    sc_string format_text = {0};
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_config_load_options load = {0};
    sc_status status;

    if (config_path == nullptr || config_path[0] == '\0' || out == nullptr) {
        return sc_status_invalid_argument("sc.cli.daemon_log.invalid_argument");
    }
    *out = (daemon_log_config){
        .file_enabled = true,
        .level = SC_LOG_INFO,
        .format = SC_LOG_FORMAT_CONSOLE,
    };

    status = sc_app_read_text_file(alloc, config_path, &body);
    if (sc_status_is_ok(status)) {
        load.explicit_file = (sc_config_source){
            .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
            .source_path = sc_str_from_cstr(config_path),
            .body = sc_string_as_str(&body),
            .present = true,
        };
        status = sc_config_load(alloc, &load, &config, &diag);
    }
    if (sc_status_is_ok(status)) {
        out->file_enabled = sc_config_get_bool(&config, sc_str_from_cstr("logging.daemon.file_enabled"), true);
        status = sc_config_get_prop(&config, sc_str_from_cstr("logging.level"), alloc, &level_text);
    }
    if (sc_status_is_ok(status) && level_text.ptr != nullptr && !daemon_log_env_configured()) {
        sc_log_level parsed = SC_LOG_INFO;
        if (sc_log_level_from_cstr(level_text.ptr, &parsed)) {
            out->level = parsed;
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_config_get_prop(&config, sc_str_from_cstr("logging.format"), alloc, &format_text);
    }
    if (sc_status_is_ok(status) && format_text.ptr != nullptr && !daemon_log_env_configured()) {
        sc_log_format parsed = SC_LOG_FORMAT_CONSOLE;
        if (sc_log_format_from_cstr(format_text.ptr, &parsed)) {
            out->format = parsed;
        }
    }
    if (sc_status_is_ok(status)) {
        status = daemon_resolve_log_path(&config, config_path, alloc, out->path, sizeof(out->path));
    }

    sc_string_clear(&format_text);
    sc_string_clear(&level_text);
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    sc_string_clear(&body);
    return status;
}

static sc_status daemon_resolve_log_path(const sc_config *config, const char *config_path, sc_allocator *alloc, char *out, size_t capacity)
{
    sc_string configured = {0};
    char workspace[4096] = {0};
    sc_status status;

    if (config == nullptr || config_path == nullptr || alloc == nullptr || out == nullptr || capacity == 0U) {
        return sc_status_invalid_argument("sc.cli.daemon_log.invalid_argument");
    }
    status = sc_config_get_prop(config, sc_str_from_cstr("logging.daemon.file_path"), alloc, &configured);
    if (sc_status_is_ok(status)) {
        status = sc_app_cli_workspace_path(workspace, sizeof(workspace), config_path);
    }
    if (sc_status_is_ok(status)) {
        status = sc_app_join_cstr_path(out,
                                       capacity,
                                       workspace,
                                       configured.ptr == nullptr || configured.ptr[0] == '\0' ? "logs/smolclaw-daemon.log" : configured.ptr,
                                       "sc.cli.daemon_log.path_invalid");
    }

    sc_string_clear(&configured);
    return status;
}

static sc_status daemon_open_log_file(const daemon_log_config *config, int *out_fd)
{
    size_t len = 0;
    int fd = -1;

    if (out_fd == nullptr) {
        return sc_status_invalid_argument("sc.cli.daemon_log.invalid_argument");
    }
    *out_fd = -1;
    if (config == nullptr || !config->file_enabled) {
        return sc_status_ok();
    }
    if (config->path[0] == '\0') {
        return sc_status_invalid_argument("sc.cli.daemon_log.path_invalid");
    }
    len = strlen(config->path);
    while (len > 0U && config->path[len - 1U] != '/') {
        len -= 1U;
    }
    if (len > 1U) {
        char dir[4096] = {0};

        if (len >= sizeof(dir)) {
            return sc_status_invalid_argument("sc.cli.daemon_log.path_invalid");
        }
        memcpy(dir, config->path, len - 1U);
        dir[len - 1U] = '\0';
        sc_status status = sc_app_ensure_cli_dir(dir);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }

    fd = open(config->path, O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) {
        return sc_status_io("sc.cli.daemon_log.open_failed");
    }
    *out_fd = fd;
    return sc_status_ok();
}

static int daemon_redirect_stdio(const daemon_log_config *config, int log_fd, bool background)
{
    int null_fd = -1;
    int output_fd = -1;

    if (!background && (config == nullptr || !config->file_enabled)) {
        return 0;
    }
    if (background) {
        null_fd = open("/dev/null", O_RDWR);
        if (null_fd < 0) {
            return 1;
        }
        if (dup2(null_fd, STDIN_FILENO) < 0) {
            (void)close(null_fd);
            return 1;
        }
    }

    if (config != nullptr && config->file_enabled && log_fd >= 0) {
        output_fd = log_fd;
    } else {
        output_fd = null_fd >= 0 ? null_fd : open("/dev/null", O_RDWR);
        if (output_fd < 0) {
            return 1;
        }
    }
    if (dup2(output_fd, STDOUT_FILENO) < 0 || dup2(output_fd, STDERR_FILENO) < 0) {
        if (null_fd >= 0) {
            (void)close(null_fd);
        }
        if (output_fd != log_fd && output_fd >= 0) {
            (void)close(output_fd);
        }
        return 1;
    }
    if (null_fd > STDERR_FILENO) {
        (void)close(null_fd);
    }
    if (log_fd > STDERR_FILENO) {
        (void)close(log_fd);
    } else if (output_fd != log_fd && output_fd > STDERR_FILENO) {
        (void)close(output_fd);
    }
    return 0;
}

int sc_app_run_gateway(bool hard_shutdown, const char *bind)
{
    const char *config_path = getenv("SMOLCLAW_CONFIG");
    const char *workspace_path = getenv("SMOLCLAW_WORKSPACE");
    const char *once = getenv("SMOLCLAW_ONCE");
    const char *max_polls = getenv("SMOLCLAW_MAX_POLLS");
    size_t parsed_max_polls = max_polls == nullptr ? 0U : (size_t)strtoull(max_polls, nullptr, 10);
    sc_boot_options boot_options = {0};
    sc_status boot_status;

    if (config_path == nullptr || config_path[0] == '\0') {
        config_path = "smolclaw.toml";
    }

    boot_options = (sc_boot_options){
        .struct_size = sizeof(sc_boot_options),
        .config_path = sc_str_from_cstr(config_path),
        .workspace_path = sc_str_from_cstr(workspace_path),
        .once = once != nullptr && strcmp(once, "0") != 0,
        .gateway_enabled = true,
        .gateway_listener_enabled = true,
        .max_polls = parsed_max_polls,
        .hard_shutdown = hard_shutdown,
        .gateway_bind = sc_str_from_cstr(bind),
    };
    boot_status = sc_runtime_boot(sc_allocator_heap(), &boot_options);
    if (!sc_status_is_ok(boot_status)) {
        sc_app_log_bootstrap_failure(&boot_status);
        sc_app_print_bootstrap_failure(stderr, "gateway", &boot_status);
        sc_status_clear(&boot_status);
        return 1;
    }
    return 0;
}

int sc_app_run_daemon(bool hard_shutdown)
{
    const char *config_path = getenv("SMOLCLAW_CONFIG");
    const char *workspace_path = getenv("SMOLCLAW_WORKSPACE");
    const char *once = getenv("SMOLCLAW_ONCE");
    const char *max_polls = getenv("SMOLCLAW_MAX_POLLS");
    size_t parsed_max_polls = max_polls == nullptr ? 0U : (size_t)strtoull(max_polls, nullptr, 10);
    sc_boot_options boot_options = {0};
    sc_status boot_status;
    daemon_log_config log_config = {0};
    int log_fd = -1;

    if (config_path == nullptr || config_path[0] == '\0') {
        config_path = "smolclaw.toml";
    }
    boot_status = daemon_load_log_config(config_path, &log_config);
    if (!sc_status_is_ok(boot_status)) {
        sc_app_log_bootstrap_failure(&boot_status);
        sc_app_print_bootstrap_failure(stderr, "daemon", &boot_status);
        sc_status_clear(&boot_status);
        return 1;
    }
    if (!daemon_log_env_configured()) {
        sc_log_set_level(log_config.level);
        sc_log_set_format(log_config.format);
    }
    boot_status = daemon_open_log_file(&log_config, &log_fd);
    if (!sc_status_is_ok(boot_status)) {
        sc_app_log_bootstrap_failure(&boot_status);
        sc_app_print_bootstrap_failure(stderr, "daemon", &boot_status);
        sc_status_clear(&boot_status);
        return 1;
    }

    int background = daemon_background_if_requested(config_path, &log_config, log_fd);
    if (background == 1) {
        if (log_fd >= 0) {
            (void)close(log_fd);
        }
        return 1;
    }
    if (background == 2) {
        return 0;
    }
    if (background != 3 && daemon_redirect_stdio(&log_config, log_fd, false) != 0) {
        (void)fprintf(stderr, "smolclaw: daemon failed: sc.cli.daemon_log.redirect_failed\n");
        return 1;
    }
    sc_log_write(SC_LOG_INFO, "sc.cli", "cli.command", (sc_log_field[]){{.key = "command", .value = sc_str_from_cstr("daemon"), .secret = false}}, 1);

    boot_options = (sc_boot_options){
        .struct_size = sizeof(sc_boot_options),
        .config_path = sc_str_from_cstr(config_path),
        .workspace_path = sc_str_from_cstr(workspace_path),
        .once = once != nullptr && strcmp(once, "0") != 0,
        .max_polls = parsed_max_polls,
        .hard_shutdown = hard_shutdown,
    };
    boot_status = sc_runtime_boot(sc_allocator_heap(), &boot_options);
    if (!sc_status_is_ok(boot_status)) {
        sc_app_log_bootstrap_failure(&boot_status);
        sc_app_print_bootstrap_failure(stderr, "daemon", &boot_status);
        sc_status_clear(&boot_status);
        return 1;
    }
    return 0;
}

int sc_app_run_acp()
{
    sc_allocator *alloc = sc_allocator_heap();
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_acp_server *server = nullptr;
    sc_status status;
    char line[65'537] = {0};
    int exit_code = 0;

    status = sc_provider_mock_new(alloc, SC_PROVIDER_MOCK_TEXT, sc_str_from_cstr("mock assistant"), &provider);
    if (sc_status_is_ok(status)) {
        status = sc_agent_new(alloc,
                              &(sc_agent_options){
                                  .struct_size = sizeof(sc_agent_options),
                                  .provider = provider,
                                  .model = sc_str_from_cstr("mock-model"),
                                  .use_streaming = true,
                                  .emit_stream_deltas = true,
                              },
                              &agent);
    }
    if (sc_status_is_ok(status)) {
        status = sc_acp_server_new(alloc,
                                   &(sc_acp_options){
                                       .struct_size = sizeof(sc_acp_options),
                                       .agent = agent,
                                       .default_model = sc_str_from_cstr("mock-model"),
                                       .max_sessions = 16,
                                       .idle_timeout_secs = 3'600,
                                       .approval_requests_enabled = true,
                                   },
                                   &server);
    }
    if (!sc_status_is_ok(status)) {
        sc_app_print_bootstrap_failure(stderr, "acp", &status);
        sc_status_clear(&status);
        sc_agent_destroy(agent);
        sc_provider_destroy(provider);
        return 1;
    }

    while (fgets(line, sizeof(line), stdin) != nullptr) {
        sc_string output = {0};
        status = sc_acp_handle_line(server, sc_str_from_cstr(line), alloc, &output);
        if (!sc_status_is_ok(status)) {
            sc_app_print_bootstrap_failure(stderr, "acp", &status);
            sc_status_clear(&status);
            exit_code = 1;
            break;
        }
        if (output.len > 0 && fwrite(output.ptr, 1, output.len, stdout) != output.len) {
            (void)fprintf(stderr, "smolclaw: acp failed: sc.cli.acp.write_failed\n");
            exit_code = 1;
            sc_string_clear(&output);
            break;
        }
        sc_string_clear(&output);
        (void)fflush(stdout);
    }

    sc_acp_server_destroy(server);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    return exit_code;
}
