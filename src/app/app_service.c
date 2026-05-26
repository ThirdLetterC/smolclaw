#include "app/app_commands.h"
#include "app/app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *service_platform_name()
{
#if defined(__APPLE__)
    return "launchctl";
#else
    return "systemd";
#endif
}

static sc_status service_descriptor_path(char *unit_dir, size_t unit_dir_capacity, char *unit_path, size_t unit_path_capacity)
{
#if defined(__APPLE__)
    const char *home = getenv("HOME");
    sc_status status = sc_app_join_cstr_path(unit_dir,
                                             unit_dir_capacity,
                                             sc_app_env_truthy(home) ? home : ".",
                                             "Library/LaunchAgents",
                                             "sc.cli.service_dir_invalid");
    if (sc_status_is_ok(status)) {
        status = sc_app_join_cstr_path(unit_path,
                                       unit_path_capacity,
                                       unit_dir,
                                       "com.thirdletterc.smolclaw.plist",
                                       "sc.cli.service_path_invalid");
    }
    return status;
#else
    const char *xdg = getenv("XDG_CONFIG_HOME");
    sc_status status;

    if (sc_app_env_truthy(xdg)) {
        status = sc_app_join_cstr_path(unit_dir, unit_dir_capacity, xdg, "systemd/user", "sc.cli.service_dir_invalid");
    } else {
        const char *home = getenv("HOME");
        char config_dir[4096] = {0};

        status = sc_app_join_cstr_path(config_dir,
                                       sizeof(config_dir),
                                       sc_app_env_truthy(home) ? home : ".",
                                       ".config",
                                       "sc.cli.service_dir_invalid");
        if (sc_status_is_ok(status)) {
            status = sc_app_join_cstr_path(unit_dir, unit_dir_capacity, config_dir, "systemd/user", "sc.cli.service_dir_invalid");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_app_join_cstr_path(unit_path, unit_path_capacity, unit_dir, "smolclaw.service", "sc.cli.service_path_invalid");
    }
    return status;
#endif
}

static sc_status write_service_descriptor(const char *path, const char *argv0)
{
    sc_allocator *alloc = sc_allocator_heap();
    sc_string_builder builder = {0};
    sc_string descriptor = {0};
    sc_status status;

    if (path == nullptr || path[0] == '\0') {
        return sc_status_invalid_argument("sc.cli.service_path_invalid");
    }
    if (argv0 == nullptr || argv0[0] == '\0') {
        argv0 = "smolclaw";
    }

    sc_string_builder_init(&builder, alloc);
#if defined(__APPLE__)
    status = sc_string_builder_append_cstr(&builder,
                                           "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                                           "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
                                           "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
                                           "<plist version=\"1.0\"><dict>\n"
                                           "<key>Label</key><string>com.thirdletterc.smolclaw</string>\n"
                                           "<key>ProgramArguments</key><array><string>");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, argv0);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder,
                                               "</string><string>daemon</string></array>\n"
                                               "<key>EnvironmentVariables</key><dict><key>SMOLCLAW_DAEMON_FOREGROUND</key><string>1</string></dict>\n"
                                               "<key>KeepAlive</key><true/>\n"
                                               "<key>RunAtLoad</key><true/>\n"
                                               "</dict></plist>\n");
    }
#else
    status = sc_string_builder_append_cstr(&builder,
                                           "[Unit]\n"
                                           "Description=SmolClaw local runtime\n"
                                           "After=network-online.target\n"
                                           "\n"
                                           "[Service]\n"
                                           "Type=simple\n"
                                           "Environment=SMOLCLAW_DAEMON_FOREGROUND=1\n"
                                           "ExecStart=");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, argv0);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder,
                                               " daemon\n"
                                               "Restart=on-failure\n"
                                               "RestartSec=5\n"
                                               "\n"
                                               "[Install]\n"
                                               "WantedBy=default.target\n");
    }
#endif
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &descriptor);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_app_write_text_file(path,
                                        sc_string_as_str(&descriptor),
                                        "sc.cli.service_write_open_failed",
                                        "sc.cli.service_write_failed",
                                        "sc.cli.service_write_close_failed");
    }
    sc_string_clear(&descriptor);
    return status;
}

static void print_service_hint(FILE *stream, const char *command)
{
#if defined(__APPLE__)
    if (strcmp(command, "install") == 0) {
        (void)fprintf(stream, "command_hint: launchctl bootstrap gui/$(id -u) <plist>\n");
    } else if (strcmp(command, "uninstall") == 0) {
        (void)fprintf(stream, "command_hint: launchctl bootout gui/$(id -u) <plist>\n");
    } else {
        (void)fprintf(stream, "command_hint: launchctl %s com.thirdletterc.smolclaw\n", command);
    }
#else
    if (strcmp(command, "install") == 0) {
        (void)fprintf(stream, "command_hint: systemctl --user enable --now smolclaw.service\n");
    } else if (strcmp(command, "uninstall") == 0) {
        (void)fprintf(stream, "command_hint: systemctl --user disable --now smolclaw.service\n");
    } else {
        (void)fprintf(stream, "command_hint: systemctl --user %s smolclaw.service\n", command);
    }
#endif
}

int sc_app_run_service_command(const char *command, const char *argv0)
{
    char unit_dir[4096] = {0};
    char unit_path[4096] = {0};
    const char *dry_env = getenv("SMOLCLAW_SERVICE_DRY_RUN");
    bool dry_run = false;
    sc_status status = service_descriptor_path(unit_dir, sizeof(unit_dir), unit_path, sizeof(unit_path));

    if (command == nullptr || command[0] == '\0') {
        command = "status";
    }
    dry_run = strcmp(command, "dry-run") == 0 || sc_app_env_truthy(dry_env);

    (void)fprintf(stdout, "SmolClaw service\n");
    (void)fprintf(stdout, "platform: %s\n", service_platform_name());
    (void)fprintf(stdout, "command: %s\n", command);
    (void)fprintf(stdout, "dry_run: %s\n", dry_run ? "true" : "false");
    (void)fprintf(stdout, "descriptor: %s\n", unit_path[0] == '\0' ? "(unresolved)" : unit_path);

    if (sc_status_is_ok(status) && strcmp(command, "status") == 0) {
        (void)fprintf(stdout, "installed: %s\n", sc_app_file_exists(unit_path) ? "true" : "false");
    } else if (sc_status_is_ok(status) && (strcmp(command, "dry-run") == 0 || strcmp(command, "install") == 0)) {
        print_service_hint(stdout, "install");
        if (dry_run) {
            (void)fprintf(stdout, "would_write: true\n");
        } else {
            status = sc_app_ensure_cli_dir(unit_dir);
            if (sc_status_is_ok(status)) {
                status = write_service_descriptor(unit_path, argv0);
            }
        }
    } else if (sc_status_is_ok(status) && strcmp(command, "uninstall") == 0) {
        print_service_hint(stdout, "uninstall");
        if (dry_run) {
            (void)fprintf(stdout, "would_remove: %s\n", sc_app_file_exists(unit_path) ? "true" : "false");
        } else if (sc_app_file_exists(unit_path) && remove(unit_path) != 0) {
            status = sc_status_io("sc.cli.service_remove_failed");
        }
    } else if (sc_status_is_ok(status) &&
               (strcmp(command, "start") == 0 || strcmp(command, "stop") == 0 || strcmp(command, "restart") == 0)) {
        print_service_hint(stdout, command);
        (void)fprintf(stdout, "managed_externally: true\n");
    } else if (sc_status_is_ok(status)) {
        status = sc_status_invalid_argument("sc.cli.service.unknown_command");
    }

    if (sc_status_is_ok(status)) {
        (void)fprintf(stdout, "status: ok\n");
        return 0;
    }

    (void)fprintf(stdout, "status: failed\n");
    (void)fprintf(stdout, "error: %s\n", status.error_key == nullptr ? "sc.cli.service.failed" : status.error_key);
    sc_status_clear(&status);
    return 1;
}
