#include "app/app_commands.h"
#include "app/app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc/allocator.h"
#include "sc/api.h"
#include "sc/i18n.h"
#include "sc/log.h"

static const char starter_config[] =
    "schema_version = 2\n"
    "\n"
    "[runtime]\n"
    "autonomy_level = \"supervised\"\n"
    "workspace_path = \"\"\n"
    "\n"
    "[logging]\n"
    "level = \"info\"\n"
    "format = \"console\"\n"
    "\n"
    "[logging.daemon]\n"
    "file_enabled = true\n"
    "file_path = \"logs/smolclaw-daemon.log\"\n"
    "\n"
    "[autonomy]\n"
    "level = \"supervised\"\n"
    "shell_enabled = false\n"
    "workspace_only = true\n"
    "auto_approve = []\n"
    "always_ask = []\n"
    "never_allow = []\n"
    "\n"
    "[security.sandbox]\n"
    "backend = \"auto\"\n"
    "network = \"full\"\n"
    "\n"
    "[security.otp]\n"
    "enabled = false\n"
    "actions = [\"shell\", \"browser\", \"file_write\"]\n"
    "code = \"\"\n"
    "\n"
    "[agent.tool_receipts]\n"
    "enabled = true\n"
    "show_in_response = false\n"
    "inject_system_prompt = true\n"
    "\n"
    "[agent]\n"
    "max_history_messages = 50\n"
    "max_tool_iterations = 10\n"
    "max_tool_result_chars = 50000\n"
    "max_system_prompt_chars = 0\n"
    "\n"
    "[gateway]\n"
    "enabled = false\n"
    "listener_enabled = false\n"
    "bind = \"127.0.0.1\"\n"
    "port = 8080\n"
    "public_bind_enabled = false\n"
    "\n"
    "[providers]\n"
    "fallback = \"gemini\"\n"
    "\n"
    "[providers.models.gemini]\n"
    "model = \"gemini-3.1-flash-lite-preview\"\n"
    "credential_env = \"GEMINI_API_KEY\"\n"
    "merge_system_into_user = false\n"
    "thinking_level = \"default\"\n"
    "validate_model = true\n"
    "\n"
    "[reliability]\n"
    "max_retries = 2\n"
    "retry_backoff_ms = 250\n"
    "timeout_ms = 30000\n"
    "fallback_providers = []\n"
    "\n"
    "[channels]\n"
    "session_persistence = true\n"
    "session_backend = \"sqlite\"\n"
    "ack_reactions = false\n"
    "show_tool_calls = false\n"
    "max_seen_message_ids = 1024\n"
    "\n"
    "[channels.telegram]\n"
    "enabled = false\n"
    "bot_token_env = \"SMOLCLAW_TELEGRAM_BOT_TOKEN\"\n"
    "bot_token = \"\"\n"
    "allowed_users = [\"your_telegram_username_or_sender_id\"]\n"
    "mention_only = false\n"
    "interrupt_on_new_message = false\n"
    "post_reactions = false\n"
    "stream_mode = \"off\"\n"
    "poll_timeout_seconds = 30\n"
    "message_split_bytes = 3900\n"
    "draft_update_interval_ms = 1000\n"
    "approval_timeout_secs = 120\n"
    "\n"
    "[heartbeat]\n"
    "enabled = false\n"
    "state_path = \"heartbeat.state\"\n"
    "\n"
    "[memory]\n"
    "backend = \"sqlite\"\n";

static sc_status write_starter_config(const char *path)
{
    if (path == nullptr || path[0] == '\0') {
        return sc_status_invalid_argument("sc.cli.init_config.invalid_path");
    }
    if (sc_app_file_exists(path)) {
        return sc_status_io("sc.cli.init_config.exists");
    }

    return sc_app_write_text_file(path,
                                  sc_str_from_cstr(starter_config),
                                  "sc.cli.init_config.open_failed",
                                  "sc.cli.init_config.write_failed",
                                  "sc.cli.init_config.close_failed");
}

int sc_app_run_init_config()
{
    const char *config_path = getenv("SMOLCLAW_CONFIG");
    sc_i18n_catalog catalog = {0};
    sc_i18n_arg path_arg = {0};
    sc_string message = {0};
    sc_status status;

    if (config_path == nullptr || config_path[0] == '\0') {
        config_path = "smolclaw.toml";
    }

    sc_i18n_catalog_init(&catalog, sc_allocator_heap(), sc_str_from_cstr("en"));
    (void)sc_i18n_catalog_load_default_en(&catalog);
    path_arg = (sc_i18n_arg){.name = sc_str_from_cstr("path"), .value = sc_str_from_cstr(config_path)};

    status = write_starter_config(config_path);
    if (sc_status_is_ok(status)) {
        message = sc_app_format_key(&catalog, "cli.init_config.created", &path_arg, 1);
        (void)fprintf(stdout, "%s\n", message.ptr == nullptr ? "" : message.ptr);
        sc_string_clear(&message);
        sc_i18n_catalog_clear(&catalog);
        return 0;
    }

    message = sc_app_format_key(&catalog,
                                status.error_key != nullptr && strcmp(status.error_key, "sc.cli.init_config.exists") == 0
                                    ? "cli.init_config.exists"
                                    : "cli.init_config.failed",
                                &path_arg,
                                1);
    (void)fprintf(stderr, "%s\n", message.ptr == nullptr ? "" : message.ptr);
    sc_string_clear(&message);
    sc_i18n_catalog_clear(&catalog);
    sc_status_clear(&status);
    return 1;
}

int sc_app_run_config_preset(const char *preset)
{
    if (preset == nullptr || preset[0] == '\0' || strcmp(preset, "default-posture") == 0) {
        (void)fprintf(stdout, "SmolClaw config preset: default-posture\n\n");
        (void)fputs("[runtime]\n"
                    "autonomy_level = \"supervised\"\n"
                    "\n"
                    "[autonomy]\n"
                    "level = \"supervised\"\n"
                    "shell_enabled = false\n"
                    "workspace_only = true\n"
                    "auto_approve = []\n"
                    "always_ask = []\n"
                    "never_allow = []\n"
                    "\n"
                    "[agent.tool_receipts]\n"
                    "enabled = true\n"
                    "show_in_response = false\n"
                    "inject_system_prompt = true\n"
                    "\n"
                    "[gateway]\n"
                    "enabled = false\n"
                    "listener_enabled = false\n"
                    "bind = \"127.0.0.1\"\n"
                    "public_bind_enabled = false\n",
                    stdout);
        (void)fprintf(stdout, "status: ok\n");
        return 0;
    }
    if (strcmp(preset, "yolo") == 0) {
        sc_log_field fields[] = {
            {.key = "preset", .value = sc_str_from_cstr("yolo"), .secret = false},
            {.key = "disabled_guardrails",
             .value = sc_str_from_cstr("workspace_only,shell_denial,otp,sandbox_backend"),
             .secret = false},
        };

        sc_log_write(SC_LOG_WARN, "sc.security", "security.yolo_guardrails_disabled", fields, SC_ARRAY_LEN(fields));
        (void)fprintf(stdout, "SmolClaw config preset: yolo\n");
        (void)fprintf(stdout, "# WARNING: YOLO disables core guardrails. Use only in isolated disposable workspaces.\n\n");
        (void)fputs("[runtime]\n"
                    "autonomy_level = \"full\"\n"
                    "\n"
                    "[autonomy]\n"
                    "level = \"full\"\n"
                    "shell_enabled = true\n"
                    "workspace_only = false\n"
                    "auto_approve = [\"shell\", \"file_write\", \"browser\", \"http\", \"memory_store\", \"memory_pin\", \"memory_forget\", \"cron_upsert\", \"cron_remove\"]\n"
                    "always_ask = []\n"
                    "never_allow = []\n"
                    "allowed_commands = []\n"
                    "forbidden_commands = []\n"
                    "\n"
                    "[security.otp]\n"
                    "enabled = false\n"
                    "actions = []\n"
                    "code = \"\"\n"
                    "\n"
                    "[security.sandbox]\n"
                    "backend = \"noop\"\n"
                    "network = \"full\"\n"
                    "\n"
                    "[agent.tool_receipts]\n"
                    "enabled = true\n"
                    "show_in_response = true\n"
                    "inject_system_prompt = true\n"
                    "\n"
                    "[gateway]\n"
                    "public_bind_enabled = false\n",
                    stdout);
        (void)fprintf(stdout, "disabled_guardrails: workspace_only shell_denial otp sandbox_backend\n");
        (void)fprintf(stdout, "status: ok\n");
        return 0;
    }

    (void)fprintf(stderr, "smolclaw: unknown config preset: %s\n", preset);
    return 2;
}
