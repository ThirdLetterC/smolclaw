#include "app/app_commands.h"
#include "core/build_info.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc/api.h"
#include "sc/bootstrap.h"
#include "sc/cli.h"
#include "sc/i18n.h"
#include "sc/log.h"
#include "sc/result.h"

static sc_string format_key(sc_i18n_catalog *catalog,
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

static void print_parse_error(int argc, char **argv, const sc_cli_parse_result *parsed, const sc_status *status)
{
    sc_i18n_catalog catalog = {0};
    sc_string error_text = {0};
    sc_string message = {0};
    sc_i18n_arg args[2] = {0};
    sc_log_field fields[] = {
        {.key = "error_key",
         .value = sc_str_from_cstr(status == nullptr || status->error_key == nullptr ? "sc.cli.parse_failed"
                                                                                      : status->error_key),
         .secret = false},
    };

    sc_log_write(SC_LOG_WARN, "sc.cli", "cli.parse_failed", fields, SC_ARRAY_LEN(fields));

    sc_i18n_catalog_init(&catalog, sc_allocator_heap(), sc_str_from_cstr("en"));
    (void)sc_i18n_catalog_load_default_en(&catalog);
    error_text = format_key(&catalog, parsed == nullptr || parsed->error == nullptr ? "cli.error.parse" : parsed->error, nullptr, 0);
    args[0] = (sc_i18n_arg){.name = sc_str_from_cstr("error"), .value = sc_string_as_str(&error_text)};
    if (argc > 1) {
        args[1] = (sc_i18n_arg){.name = sc_str_from_cstr("arg"), .value = sc_str_from_cstr(argv[argc - 1])};
        message = format_key(&catalog, "cli.error.with_arg", args, 2);
    } else {
        message = format_key(&catalog, "cli.error", args, 1);
    }
    (void)fprintf(stderr, "%s\n", message.ptr == nullptr ? "" : message.ptr);

    sc_string_clear(&message);
    sc_string_clear(&error_text);
    sc_i18n_catalog_clear(&catalog);
}

static int print_registered_command(const sc_cli_command *command)
{
    sc_i18n_catalog catalog = {0};
    sc_i18n_arg arg = {0};
    sc_string message = {0};

    sc_i18n_catalog_init(&catalog, sc_allocator_heap(), sc_str_from_cstr("en"));
    (void)sc_i18n_catalog_load_default_en(&catalog);
    arg = (sc_i18n_arg){.name = sc_str_from_cstr("command"),
                        .value = sc_str_from_cstr(command == nullptr ? "" : command->name)};
    message = format_key(&catalog, "cli.command.registered", &arg, 1);
    (void)fprintf(stdout, "%s\n", message.ptr == nullptr ? "" : message.ptr);

    sc_string_clear(&message);
    sc_i18n_catalog_clear(&catalog);
    return 0;
}

static bool log_env_configured()
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

static int dispatch_command(int argc, char **argv, const sc_cli_parse_result *parsed)
{
    const char *name = parsed == nullptr || parsed->command == nullptr ? "" : parsed->command->name;

    if (argc > 1 && strcmp(argv[1], "service") == 0) {
        return sc_app_run_service_command(argc > 2 ? argv[2] : "status", argv[0]);
    }
    if (argc > 1 && strcmp(argv[1], "estop") == 0) {
        return sc_app_run_estop_command(argc > 2 ? argv[2] : "status");
    }
    if (argc > 2 && strcmp(argv[1], "provider") == 0 && strcmp(argv[2], "set-key") == 0) {
        return sc_app_run_provider_set_key();
    }
    if (argc > 2 && strcmp(argv[1], "config") == 0 && strcmp(argv[2], "preset") == 0) {
        return sc_app_run_config_preset(argc > 3 ? argv[3] : "default-posture");
    }
    if (argc > 1 && strcmp(argv[1], "cron") == 0) {
        return sc_app_run_cron_command(argc, argv);
    }
    if (strcmp(name, "acp") == 0) {
        return sc_app_run_acp();
    }
    if (strcmp(name, "init-config") == 0) {
        return sc_app_run_init_config();
    }
    if (strcmp(name, "onboard") == 0) {
        return sc_app_run_onboard(argc, argv);
    }
    if (strcmp(name, "config") == 0 || strcmp(name, "show") == 0) {
        return sc_app_run_config_show();
    }
    if (strcmp(name, "doctor") == 0) {
        return sc_app_run_doctor();
    }
    if (strcmp(name, "memory") == 0) {
        return sc_app_run_memory();
    }
    if (strcmp(name, "provider") == 0) {
        return sc_app_run_provider();
    }
    if (strcmp(name, "gateway") == 0 || strcmp(name, "serve") == 0) {
        return sc_app_run_gateway(parsed != nullptr && parsed->hard_shutdown,
                                  parsed == nullptr ? nullptr : parsed->gateway_bind);
    }
    if (strcmp(name, "daemon") == 0) {
        return sc_app_run_daemon(parsed != nullptr && parsed->hard_shutdown);
    }
    if (strcmp(name, "chat") == 0) {
        (void)parsed;
        return sc_app_run_chat();
    }
    return print_registered_command(parsed == nullptr ? nullptr : parsed->command);
}

int main(int argc, char **argv)
{
    const sc_cli_command *root = sc_cli_default_root();
    sc_cli_parse_result parsed = {0};
    sc_status status;
    char argc_text[32] = {0};
    sc_log_field startup_fields[] = {
        {.key = "argc", .value = sc_str_from_cstr(argc_text), .secret = false},
    };

    sc_log_configure_from_env();
    (void)snprintf(argc_text, sizeof(argc_text), "%d", argc);
    sc_log_write(SC_LOG_TRACE, "sc.cli", "cli.start", startup_fields, SC_ARRAY_LEN(startup_fields));

    status = sc_cli_parse(root, argc, argv, &parsed);
    if (!sc_status_is_ok(status)) {
        int exit_code = parsed.exit_code == 0 ? 2 : parsed.exit_code;
        print_parse_error(argc, argv, &parsed, &status);
        sc_status_clear(&status);
        return exit_code;
    }

    if (parsed.kind == SC_CLI_PARSE_VERSION) {
        sc_log_write(SC_LOG_INFO, "sc.cli", "cli.version", nullptr, 0);
        sc_app_print_version(stdout);
        return 0;
    }

    if (parsed.kind == SC_CLI_PARSE_FEATURES) {
        sc_log_write(SC_LOG_INFO, "sc.cli", "cli.features", nullptr, 0);
        sc_build_features_write(stdout);
        return 0;
    }

    if (parsed.kind == SC_CLI_PARSE_COMMAND) {
        sc_log_field fields[] = {
            {.key = "command", .value = sc_str_from_cstr(parsed.command->name), .secret = false},
        };

        if (strcmp(parsed.command->name, "chat") == 0 && !log_env_configured()) {
            sc_log_set_level(SC_LOG_OFF);
        }

        if (strcmp(parsed.command->name, "onboard") != 0 && strcmp(parsed.command->name, "chat") != 0 &&
            strcmp(parsed.command->name, "daemon") != 0) {
            sc_log_write(SC_LOG_INFO, "sc.cli", "cli.command", fields, SC_ARRAY_LEN(fields));
        }
        return dispatch_command(argc, argv, &parsed);
    }

    sc_log_write(SC_LOG_DEBUG, "sc.cli", "cli.help", nullptr, 0);
    sc_cli_print_help(parsed.command == nullptr ? root : parsed.command, "smolclaw", stdout);
    return 0;
}
