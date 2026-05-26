#include "app/app_commands.h"
#include "app/app_internal.h"
#include "app/app_onboard_internal.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include "sc/allocator.h"
#include "sc/api.h"
#include "sc/config.h"
#include "sc/string.h"

typedef enum onboard_provider_kind {
    ONBOARD_PROVIDER_OPENAI = 1,
    ONBOARD_PROVIDER_ANTHROPIC,
    ONBOARD_PROVIDER_GEMINI,
    ONBOARD_PROVIDER_OPENROUTER,
    ONBOARD_PROVIDER_AZURE_OPENAI,
    ONBOARD_PROVIDER_BEDROCK,
    ONBOARD_PROVIDER_OLLAMA,
    ONBOARD_PROVIDER_OPENAI_COMPATIBLE,
    ONBOARD_PROVIDER_GEMINI_CLI,
    ONBOARD_PROVIDER_CLAUDE_CODE,
    ONBOARD_PROVIDER_COPILOT
} onboard_provider_kind;

typedef enum onboard_channel_kind {
    ONBOARD_CHANNEL_NONE = 1,
    ONBOARD_CHANNEL_TELEGRAM,
    ONBOARD_CHANNEL_WEBHOOK,
    ONBOARD_CHANNEL_RABBITMQ,
    ONBOARD_CHANNEL_MAIL
} onboard_channel_kind;

typedef enum onboard_sandbox_kind {
    ONBOARD_SANDBOX_AUTO = 1,
    ONBOARD_SANDBOX_DOCKER,
    ONBOARD_SANDBOX_PODMAN,
    ONBOARD_SANDBOX_CONTAINER,
    ONBOARD_SANDBOX_NOOP
} onboard_sandbox_kind;

typedef struct onboard_options {
    bool providers_only;
    bool channels_only;
    bool dry_run;
    bool force;
} onboard_options;

typedef struct onboard_state {
    sc_allocator *alloc;
    onboard_options options;
    sc_string config_path;
    sc_string secret_path;
    sc_string provider_alias;
    sc_string user_name;
    sc_string user_timezone;
    sc_string user_languages;
    sc_string provider_kind;
    sc_string provider_model;
    sc_string provider_base_url;
    sc_string provider_deployment;
    sc_string provider_api_version;
    sc_string provider_region;
    sc_string provider_credential_env;
    sc_string provider_secret;
    sc_string provider_secret_path;
    sc_string channel_kind;
    sc_string channel_secret;
    sc_string channel_secret_path;
    sc_string telegram_allowed_users;
    sc_string webhook_bind;
    sc_string webhook_port;
    sc_string webhook_path;
    sc_string rabbitmq_exchange;
    sc_string rabbitmq_routing_key;
    sc_string rabbitmq_queue;
    sc_string mail_inbox_url;
    sc_string mail_smtp_url;
    sc_string mail_username;
    sc_string mail_from;
    sc_string mail_to;
    sc_string sandbox_backend;
    sc_string sandbox_docker_path;
    sc_string sandbox_podman_path;
    sc_string sandbox_container_runtime;
    sc_string sandbox_image_name;
    sc_string agent_max_history_messages;
    sc_string agent_max_tool_iterations;
    sc_string agent_max_tool_result_chars;
    sc_string agent_max_system_prompt_chars;
    sc_string channels_ack_reactions;
    sc_string channels_show_tool_calls;
    sc_string channels_max_seen_message_ids;
    sc_string telegram_post_reactions;
    sc_string telegram_poll_timeout_seconds;
    sc_string telegram_message_split_bytes;
    sc_string heartbeat_enabled;
    sc_string heartbeat_state_path;
} onboard_state;

typedef struct onboard_workspace_file {
    const char *name;
    const char *body;
} onboard_workspace_file;

static const onboard_workspace_file ONBOARD_WORKSPACE_FILES[] = {
    {
        .name = "AGENTS.md",
        .body = "# AGENTS.md - SmolClaw Personal Assistant\n"
                "\n"
                "## Every Session (required)\n"
                "\n"
                "Before doing anything else:\n"
                "\n"
                "1. Read `SOUL.md` - this is who you are\n"
                "2. Read `USER.md` - this is who you're helping\n"
                "3. Use memory recall for recent context when available\n"
                "4. If in main session, use `MEMORY.md` for durable context\n"
                "\n"
                "Don't ask permission. Just do it.\n"
                "\n"
                "## Memory System\n"
                "\n"
                "You wake up fresh each session. These files are your continuity:\n"
                "\n"
                "- Daily notes: `memory/YYYY-MM-DD.md` - raw logs\n"
                "- Long-term: `MEMORY.md` - curated memories\n"
                "\n"
                "Capture what matters: decisions, context, and things to remember.\n"
                "Skip secrets unless explicitly asked to keep them.\n"
                "\n"
                "## Safety\n"
                "\n"
                "- Don't exfiltrate private data.\n"
                "- Don't run destructive commands without asking.\n"
                "- Prefer recoverable file operations where possible.\n"
                "- When in doubt, ask.\n"
                "\n"
                "## External vs Internal\n"
                "\n"
                "Safe to do freely: read files, explore, organize, learn, and search.\n"
                "\n"
                "Ask first: sending emails, posts, messages, or anything that leaves the machine.\n"
                "\n"
                "## Crash Recovery\n"
                "\n"
                "- If a run stops unexpectedly, recover context before acting.\n"
                "- Check `MEMORY.md` and recent notes to avoid duplicate work.\n"
                "- Resume from the last confirmed step, not from scratch.\n",
    },
    {
        .name = "SOUL.md",
        .body = "# SOUL.md - Who You Are\n"
                "\n"
                "You are SmolClaw.\n"
                "\n"
                "## Core Truths\n"
                "\n"
                "**Be genuinely helpful, not performatively helpful.**\n"
                "Skip canned enthusiasm and get to the useful part.\n"
                "\n"
                "**Be resourceful before asking.**\n"
                "Read the files, check the context, and try the obvious local diagnostics first.\n"
                "\n"
                "**Earn trust through competence.**\n"
                "The user gave you access to their machine. Treat that access carefully.\n"
                "\n"
                "## Identity\n"
                "\n"
                "You are SmolClaw. That is your name.\n"
                "\n"
                "- Do not say \"As an AI\".\n"
                "- Do not present yourself as another product.\n"
                "- Introduce yourself as SmolClaw if asked.\n"
                "\n"
                "## Communication\n"
                "\n"
                "Be direct and concise. Skip pleasantries. Get to the point.\n"
                "\n"
                "- Sound like a real person, not a support script.\n"
                "- Mirror the user's energy.\n"
                "- Prefer specific, grounded phrasing over generic filler.\n"
                "\n"
                "## Boundaries\n"
                "\n"
                "- Private things stay private.\n"
                "- Ask before acting externally when the action has side effects.\n"
                "- Be careful in group chats; you are not the user's voice.\n",
    },
    {
        .name = "USER.md",
        .body = "# USER.md - Who You're Helping\n"
                "\n"
                "SmolClaw reads this file every session to understand the user.\n"
                "\n"
                "## About You\n"
                "\n"
                "- Name: Yehor\n"
                "- Timezone: Europe/Kyiv\n"
                "- Languages: English\n"
                "\n"
                "## Communication Style\n"
                "\n"
                "- Be direct and concise. Skip pleasantries. Get to the point.\n"
                "\n"
                "## Preferences\n"
                "\n"
                "- Add durable preferences here.\n"
                "\n"
                "## Work Context\n"
                "\n"
                "- Add durable work context here.\n",
    },
    {
        .name = "IDENTITY.md",
        .body = "# IDENTITY.md - Who Am I?\n"
                "\n"
                "- Name: SmolClaw\n"
                "- Vibe: Sharp, direct, resourceful. Not corporate. Not a chatbot.\n"
                "\n"
                "Update this file as the assistant's identity evolves.\n",
    },
    {
        .name = "MEMORY.md",
        .body = "# MEMORY.md - Long-Term Memory\n"
                "\n"
                "Curated memories belong here. Keep this distilled, not noisy.\n"
                "\n"
                "## How This Works\n"
                "\n"
                "- Daily files (`memory/YYYY-MM-DD.md`) capture raw events.\n"
                "- This file captures what is worth keeping long-term.\n"
                "- Keep it concise.\n"
                "\n"
                "## Security\n"
                "\n"
                "- Do not store secrets unless explicitly requested.\n"
                "- Do not include private data that does not need to persist.\n"
                "\n"
                "## Key Facts\n"
                "\n"
                "Add important facts here.\n"
                "\n"
                "## Decisions & Preferences\n"
                "\n"
                "Record durable decisions and preferences here.\n"
                "\n"
                "## Lessons Learned\n"
                "\n"
                "Document mistakes and insights here.\n"
                "\n"
                "## Open Loops\n"
                "\n"
                "Track unfinished tasks and follow-ups here.\n",
    },
    {
        .name = "TOOLS.md",
        .body = "# TOOLS.md - Local Notes\n"
                "\n"
                "Skills define how tools work. This file is for setup-specific notes.\n"
                "\n"
                "## What Goes Here\n"
                "\n"
                "- SSH hosts and aliases\n"
                "- Device nicknames\n"
                "- Local service URLs\n"
                "- Environment-specific commands\n"
                "\n"
                "## Built-in Tools\n"
                "\n"
                "- shell: run local checks, builds, tests, and diagnostics.\n"
                "- file read: inspect project files, configs, and logs.\n"
                "- file write: apply focused edits or update docs/code.\n"
                "- memory store: preserve durable preferences, decisions, or context.\n"
                "- memory recall: retrieve prior decisions, preferences, or context.\n"
                "- memory forget: remove stale or incorrect memory.\n",
    },
    {
        .name = "HEARTBEAT.md",
        .body = "# HEARTBEAT.md\n"
                "\n"
                "# Keep this file empty, or with only comments, to skip heartbeat work.\n"
                "# Add tasks below when SmolClaw should check something periodically.\n"
                "#\n"
                "# Examples:\n"
                "# - Check important messages\n"
                "# - Review upcoming events\n"
                "# - Run `git status` on active projects\n",
    },
};

static const char ONBOARD_DEFAULT_SANDBOX_IMAGE[] = "alpine:3.23.4";
static const char ONBOARD_DEFAULT_DOCKER_PATH[] = "/usr/bin/docker";
static const char ONBOARD_DEFAULT_PODMAN_PATH[] = "/usr/bin/podman";
static const char ONBOARD_DEFAULT_CONTAINER_RUNTIME[] = "/usr/bin/nerdctl";

static sc_status parse_onboard_options(int argc, char **argv, onboard_options *out);
static sc_status resolve_config_path(sc_allocator *alloc, sc_string *out);
static sc_status resolve_secret_path(sc_allocator *alloc, sc_string *out);
static sc_status prompt_line(prompt_context *ctx, const char *label, const char *fallback, sc_allocator *alloc, sc_string *out);
static sc_status prompt_secret(prompt_context *ctx, const char *label, sc_allocator *alloc, sc_string *out);
static sc_status prompt_choice(prompt_context *ctx, const char *label, int min, int max, int fallback, int *out);
static sc_status collect_provider(prompt_context *ctx, onboard_state *state);
static sc_status collect_channel(prompt_context *ctx, onboard_state *state);
static sc_status collect_sandbox(prompt_context *ctx, onboard_state *state);
static sc_status collect_advanced_config(prompt_context *ctx, onboard_state *state);
static sc_status collect_user_profile(prompt_context *ctx, onboard_state *state);
static sc_status build_managed_config(onboard_state *state, sc_string *out);
static sc_status dirname_to_buffer(char *out, size_t capacity, const char *path);
static sc_status write_onboard_workspace_files(onboard_state *state);
static sc_status build_user_workspace_file(onboard_state *state, sc_string *out);
static sc_status write_secret_values(onboard_state *state);
static sc_status validate_written_config(onboard_state *state);
static void onboard_state_clear(onboard_state *state);
static const char *provider_kind_name(onboard_provider_kind kind);
static const char *provider_default_alias(onboard_provider_kind kind);
static const char *provider_default_model(onboard_provider_kind kind);
static const char *provider_default_env(onboard_provider_kind kind);
static bool provider_needs_secret(onboard_provider_kind kind);
static bool provider_uses_env_fallback(onboard_provider_kind kind);
static const char *sandbox_backend_name(onboard_sandbox_kind kind);
static bool string_empty(const sc_string *value);
static sc_str string_or_default(const sc_string *value, const char *fallback);
static sc_status copy_cstr(sc_allocator *alloc, const char *value, sc_string *out);
static sc_status normalize_telegram_allowed_users(sc_allocator *alloc, sc_str input, sc_string *out);
static sc_status append_toml_string(sc_string_builder *builder, sc_str value);
int sc_app_run_onboard(int argc, char **argv)
{
    sc_allocator *alloc = sc_allocator_heap();
    prompt_context ctx = {.in = stdin, .out = stdout};
    onboard_state state = {.alloc = alloc};
    sc_string config = {0};
    sc_status status;
    int exit_code = 1;

    status = parse_onboard_options(argc, argv, &state.options);
    if (sc_status_is_ok(status)) {
        status = resolve_config_path(alloc, &state.config_path);
    }
    if (sc_status_is_ok(status)) {
        status = resolve_secret_path(alloc, &state.secret_path);
    }
    if (sc_status_is_ok(status) && !state.options.dry_run) {
        status = collect_user_profile(&ctx, &state);
    }
    if (sc_status_is_ok(status) && !state.options.channels_only) {
        status = collect_provider(&ctx, &state);
    }
    if (sc_status_is_ok(status) && !state.options.providers_only) {
        status = collect_channel(&ctx, &state);
    }
    if (sc_status_is_ok(status) && !state.options.providers_only && !state.options.channels_only) {
        status = collect_sandbox(&ctx, &state);
    }
    if (sc_status_is_ok(status) && !state.options.providers_only && !state.options.channels_only) {
        status = collect_advanced_config(&ctx, &state);
    }
    if (sc_status_is_ok(status)) {
        status = build_managed_config(&state, &config);
    }
    if (sc_status_is_ok(status) && state.options.dry_run) {
        (void)fprintf(stdout, "%s", config.ptr == nullptr ? "" : config.ptr);
        (void)fprintf(stdout, "status: dry-run\n");
        exit_code = 0;
        goto cleanup;
    }
    if (sc_status_is_ok(status) && sc_app_file_exists(state.config_path.ptr) && !state.options.force) {
        (void)fprintf(stderr, "smolclaw: onboard failed: config exists at %s; pass --force to replace it\n", state.config_path.ptr);
        exit_code = 2;
        goto cleanup;
    }
    if (sc_status_is_ok(status)) {
        char config_dir[4096] = {0};
        status = dirname_to_buffer(config_dir, sizeof(config_dir), state.config_path.ptr);
        if (sc_status_is_ok(status)) {
            status = sc_app_ensure_cli_dir(config_dir);
        }
    }
    if (sc_status_is_ok(status)) {
        status = write_onboard_workspace_files(&state);
    }
    if (sc_status_is_ok(status)) {
        status = write_secret_values(&state);
    }
    if (sc_status_is_ok(status)) {
        status = sc_app_write_text_file(state.config_path.ptr,
                                        sc_string_as_str(&config),
                                        "sc.onboard.config_open_failed",
                                        "sc.onboard.config_write_failed",
                                        "sc.onboard.config_close_failed");
    }
    if (sc_status_is_ok(status)) {
        status = validate_written_config(&state);
    }
    if (sc_status_is_ok(status)) {
        (void)fprintf(stdout, "Wrote SmolClaw config to %s\n", state.config_path.ptr);
        (void)fprintf(stdout, "Initialized workspace identity files\n");
        if (!string_empty(&state.provider_secret) || !string_empty(&state.channel_secret)) {
            (void)fprintf(stdout, "Stored secrets in %s\n", state.secret_path.ptr);
        }
        (void)fprintf(stdout, "Next: SMOLCLAW_CONFIG=%s smolclaw doctor\n", state.config_path.ptr);
        exit_code = 0;
    }

cleanup:
    if (!sc_status_is_ok(status)) {
        (void)fprintf(stderr, "smolclaw: onboard failed: %s\n", status.error_key == nullptr ? "sc.onboard.failed" : status.error_key);
        sc_status_clear(&status);
    }
    sc_string_clear(&config);
    onboard_state_clear(&state);
    return exit_code;
}

static sc_status parse_onboard_options(int argc, char **argv, onboard_options *out)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.onboard.invalid_argument");
    }
    *out = (onboard_options){0};
    for (int i = 2; i < argc; i += 1) {
        if (strcmp(argv[i], "--providers-only") == 0) {
            out->providers_only = true;
        } else if (strcmp(argv[i], "--channels-only") == 0) {
            out->channels_only = true;
        } else if (strcmp(argv[i], "--dry-run") == 0) {
            out->dry_run = true;
        } else if (strcmp(argv[i], "--force") == 0) {
            out->force = true;
        } else {
            return sc_status_invalid_argument("sc.onboard.unknown_argument");
        }
    }
    if (out->providers_only && out->channels_only) {
        return sc_status_invalid_argument("sc.onboard.conflicting_scope");
    }
    return sc_status_ok();
}

static sc_status resolve_config_path(sc_allocator *alloc, sc_string *out)
{
    const char *env = getenv("SMOLCLAW_CONFIG");
    const char *home = getenv("HOME");
    sc_string_builder builder = {0};
    sc_status status;

    if (sc_app_env_truthy(env)) {
        return sc_string_from_cstr(alloc, env, out);
    }
    if (sc_app_file_exists("smolclaw.toml")) {
        return sc_string_from_cstr(alloc, "smolclaw.toml", out);
    }
    if (home == nullptr || home[0] == '\0') {
        return sc_string_from_cstr(alloc, "smolclaw.toml", out);
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, home);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/.smolclaw/config.toml");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status resolve_secret_path(sc_allocator *alloc, sc_string *out)
{
    const char *home = getenv("HOME");
    sc_string_builder builder = {0};
    sc_status status;

    if (home == nullptr || home[0] == '\0') {
        return sc_string_from_cstr(alloc, "smolclaw.secrets", out);
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, home);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/.smolclaw/secrets");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status prompt_line(prompt_context *ctx, const char *label, const char *fallback, sc_allocator *alloc, sc_string *out)
{
    char line[4096] = {0};
    size_t len = 0;

    (void)fprintf(ctx->out, "%s", label);
    if (fallback != nullptr && fallback[0] != '\0') {
        (void)fprintf(ctx->out, " [%s]", fallback);
    }
    (void)fprintf(ctx->out, ": ");
    (void)fflush(ctx->out);
    if (fgets(line, sizeof(line), ctx->in) == nullptr) {
        if (fallback != nullptr) {
            return sc_string_from_cstr(alloc, fallback, out);
        }
        return sc_status_io("sc.onboard.input_failed");
    }
    len = strlen(line);
    while (len > 0 && (line[len - 1U] == '\n' || line[len - 1U] == '\r')) {
        line[--len] = '\0';
    }
    if (len == 0 && fallback != nullptr) {
        return sc_string_from_cstr(alloc, fallback, out);
    }
    return sc_string_from_cstr(alloc, line, out);
}

static sc_status prompt_secret(prompt_context *ctx, const char *label, sc_allocator *alloc, sc_string *out)
{
    sc_status status;
    struct termios old_term = {0};
    bool masked = false;

    if (isatty(fileno(ctx->in)) && tcgetattr(fileno(ctx->in), &old_term) == 0) {
        struct termios quiet_term = old_term;
        quiet_term.c_lflag &= (tcflag_t)~ECHO;
        masked = tcsetattr(fileno(ctx->in), TCSAFLUSH, &quiet_term) == 0;
    }
    status = prompt_line(ctx, label, "", alloc, out);
    if (masked) {
        (void)tcsetattr(fileno(ctx->in), TCSAFLUSH, &old_term);
        (void)fprintf(ctx->out, "\n");
    }
    return status;
}

static sc_status prompt_choice(prompt_context *ctx, const char *label, int min, int max, int fallback, int *out)
{
    sc_string answer = {0};
    sc_status status = prompt_line(ctx, label, "", sc_allocator_heap(), &answer);
    char *end = nullptr;
    long parsed = 0;

    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (answer.len == 0) {
        *out = fallback;
        sc_string_clear(&answer);
        return sc_status_ok();
    }
    parsed = strtol(answer.ptr, &end, 10);
    if (end == answer.ptr || *end != '\0' || parsed < min || parsed > max) {
        sc_string_clear(&answer);
        return sc_status_invalid_argument("sc.onboard.invalid_choice");
    }
    *out = (int)parsed;
    sc_string_clear(&answer);
    return sc_status_ok();
}

static sc_status collect_provider(prompt_context *ctx, onboard_state *state)
{
    int choice = 0;
    onboard_provider_kind kind;
    sc_status status;

    (void)fprintf(ctx->out,
                  "Provider:\n"
                  "  1 OpenAI\n  2 Anthropic\n  3 Gemini\n  4 OpenRouter\n"
                  "  5 Azure OpenAI\n  6 Bedrock\n  7 Ollama\n  8 OpenAI-compatible\n"
                  "  9 Gemini CLI\n  10 Claude Code\n  11 GitHub Copilot\n");
    status = prompt_choice(ctx, "Choose provider", 1, 11, 1, &choice);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    kind = (onboard_provider_kind)choice;
    status = copy_cstr(state->alloc, provider_kind_name(kind), &state->provider_kind);
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Provider alias", provider_default_alias(kind), state->alloc, &state->provider_alias);
    }
    if (sc_status_is_ok(status) && kind != ONBOARD_PROVIDER_CLAUDE_CODE) {
        status = prompt_line(ctx, "Model", provider_default_model(kind), state->alloc, &state->provider_model);
    }
    if (sc_status_is_ok(status) && (kind == ONBOARD_PROVIDER_OPENAI_COMPATIBLE || kind == ONBOARD_PROVIDER_AZURE_OPENAI || kind == ONBOARD_PROVIDER_OLLAMA)) {
        status = prompt_line(ctx, "Base URL", kind == ONBOARD_PROVIDER_OLLAMA ? "http://localhost:11434" : "", state->alloc, &state->provider_base_url);
    }
    if (sc_status_is_ok(status) && kind == ONBOARD_PROVIDER_AZURE_OPENAI) {
        status = prompt_line(ctx, "Deployment", state->provider_model.ptr, state->alloc, &state->provider_deployment);
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "API version", "2024-10-01-preview", state->alloc, &state->provider_api_version);
        }
    }
    if (sc_status_is_ok(status) && kind == ONBOARD_PROVIDER_BEDROCK) {
        status = prompt_line(ctx, "AWS region", "us-east-1", state->alloc, &state->provider_region);
    }
    if (sc_status_is_ok(status) && provider_uses_env_fallback(kind)) {
        status = copy_cstr(state->alloc, provider_default_env(kind), &state->provider_credential_env);
    }
    if (sc_status_is_ok(status) && provider_needs_secret(kind) && kind == ONBOARD_PROVIDER_COPILOT) {
            status = sc_app_onboard_copilot_device_flow(ctx, state->alloc, &state->provider_secret);
        if (!sc_status_is_ok(status)) {
            sc_status_clear(&status);
            (void)fprintf(ctx->out, "Copilot OAuth is unavailable in this build or failed; paste an existing token instead.\n");
            status = prompt_secret(ctx, "Copilot token", state->alloc, &state->provider_secret);
        }
    } else if (sc_status_is_ok(status) && provider_needs_secret(kind)) {
        status = prompt_secret(ctx, "API key or token", state->alloc, &state->provider_secret);
    }
    if (sc_status_is_ok(status) && provider_needs_secret(kind)) {
        sc_string_builder builder = {0};
        sc_string_builder_init(&builder, state->alloc);
        status = sc_string_builder_append_cstr(&builder, "providers.models.");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, sc_string_as_str(&state->provider_alias));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, kind == ONBOARD_PROVIDER_BEDROCK ? ".secret_access_key" : ".api_key");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, &state->provider_secret_path);
        } else {
            sc_string_builder_clear(&builder);
        }
    }
    return status;
}

static sc_status collect_channel(prompt_context *ctx, onboard_state *state)
{
    int choice = 0;
    sc_status status;

    (void)fprintf(ctx->out,
                  "Channel:\n"
                  "  1 None\n  2 Telegram\n  3 Webhook\n  4 RabbitMQ\n  5 Mail\n"
                  "Matrix onboarding is deferred until Matrix config is wired into C bootstrap.\n");
    status = prompt_choice(ctx, "Choose channel", 1, 5, 1, &choice);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (choice == ONBOARD_CHANNEL_NONE) {
        return copy_cstr(state->alloc, "none", &state->channel_kind);
    }
    if (choice == ONBOARD_CHANNEL_TELEGRAM) {
        status = copy_cstr(state->alloc, "telegram", &state->channel_kind);
        if (sc_status_is_ok(status)) {
            status = prompt_secret(ctx, "Telegram bot token", state->alloc, &state->channel_secret);
        }
        if (sc_status_is_ok(status)) {
            status = copy_cstr(state->alloc, "channels.telegram.bot_token", &state->channel_secret_path);
        }
        if (sc_status_is_ok(status)) {
            sc_string raw_users = {0};
            status = prompt_line(ctx, "Allowed Telegram usernames", "*", state->alloc, &raw_users);
            if (sc_status_is_ok(status)) {
                status = normalize_telegram_allowed_users(state->alloc,
                                                          sc_string_as_str(&raw_users),
                                                          &state->telegram_allowed_users);
            }
            sc_string_clear(&raw_users);
        }
    } else if (choice == ONBOARD_CHANNEL_WEBHOOK) {
        status = copy_cstr(state->alloc, "webhook", &state->channel_kind);
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "Webhook bind", "127.0.0.1", state->alloc, &state->webhook_bind);
        }
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "Webhook port", "8081", state->alloc, &state->webhook_port);
        }
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "Webhook path", "/webhook/smolclaw", state->alloc, &state->webhook_path);
        }
        if (sc_status_is_ok(status)) {
            status = prompt_secret(ctx, "Webhook auth token", state->alloc, &state->channel_secret);
        }
        if (sc_status_is_ok(status)) {
            status = copy_cstr(state->alloc, "channels.webhook.auth_token", &state->channel_secret_path);
        }
    } else if (choice == ONBOARD_CHANNEL_RABBITMQ) {
        status = copy_cstr(state->alloc, "rabbitmq", &state->channel_kind);
        if (sc_status_is_ok(status)) {
            status = prompt_secret(ctx, "RabbitMQ AMQP URL", state->alloc, &state->channel_secret);
        }
        if (sc_status_is_ok(status)) {
            status = copy_cstr(state->alloc, "channels.rabbitmq.url", &state->channel_secret_path);
        }
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "Exchange", "", state->alloc, &state->rabbitmq_exchange);
        }
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "Routing key", "smolclaw.inbound", state->alloc, &state->rabbitmq_routing_key);
        }
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "Queue", "smolclaw", state->alloc, &state->rabbitmq_queue);
        }
    } else {
        status = copy_cstr(state->alloc, "mail", &state->channel_kind);
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "Inbox URL", "", state->alloc, &state->mail_inbox_url);
        }
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "SMTP URL", "", state->alloc, &state->mail_smtp_url);
        }
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "Username", "", state->alloc, &state->mail_username);
        }
        if (sc_status_is_ok(status)) {
            status = prompt_secret(ctx, "Mail password or app token", state->alloc, &state->channel_secret);
        }
        if (sc_status_is_ok(status)) {
            status = copy_cstr(state->alloc, "channels.mail.password", &state->channel_secret_path);
        }
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "From address", "", state->alloc, &state->mail_from);
        }
        if (sc_status_is_ok(status)) {
            status = prompt_line(ctx, "Default recipient", "", state->alloc, &state->mail_to);
        }
    }
    return status;
}

static sc_status collect_sandbox(prompt_context *ctx, onboard_state *state)
{
    int choice = 0;
    onboard_sandbox_kind kind;
    sc_status status;

    (void)fprintf(ctx->out,
                  "Sandbox:\n"
                  "  1 Auto\n  2 Docker\n  3 Podman\n  4 Container-compatible runtime\n  5 Noop\n");
    status = prompt_choice(ctx, "Choose command sandbox", 1, 5, 1, &choice);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    kind = (onboard_sandbox_kind)choice;
    status = copy_cstr(state->alloc, sandbox_backend_name(kind), &state->sandbox_backend);
    if (sc_status_is_ok(status) && kind == ONBOARD_SANDBOX_DOCKER) {
        status = prompt_line(ctx, "Docker binary", ONBOARD_DEFAULT_DOCKER_PATH, state->alloc, &state->sandbox_docker_path);
    }
    if (sc_status_is_ok(status) && kind == ONBOARD_SANDBOX_PODMAN) {
        status = prompt_line(ctx, "Podman binary", ONBOARD_DEFAULT_PODMAN_PATH, state->alloc, &state->sandbox_podman_path);
    }
    if (sc_status_is_ok(status) && kind == ONBOARD_SANDBOX_CONTAINER) {
        status = prompt_line(ctx,
                             "Container runtime binary",
                             ONBOARD_DEFAULT_CONTAINER_RUNTIME,
                             state->alloc,
                             &state->sandbox_container_runtime);
    }
    if (sc_status_is_ok(status) && kind != ONBOARD_SANDBOX_NOOP) {
        status = prompt_line(ctx, "Sandbox container image", ONBOARD_DEFAULT_SANDBOX_IMAGE, state->alloc, &state->sandbox_image_name);
    }
    return status;
}

static sc_status collect_advanced_config(prompt_context *ctx, onboard_state *state)
{
    int choice = 0;
    sc_status status;

    (void)fprintf(ctx->out, "Advanced runtime tuning:\n  1 Keep defaults\n  2 Configure\n");
    status = prompt_choice(ctx, "Choose advanced config mode", 1, 2, 1, &choice);
    if (!sc_status_is_ok(status) || choice == 1) {
        return status;
    }
    status = prompt_line(ctx, "Max history messages", "50", state->alloc, &state->agent_max_history_messages);
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Max tool iterations", "10", state->alloc, &state->agent_max_tool_iterations);
    }
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Max tool result chars", "50000", state->alloc, &state->agent_max_tool_result_chars);
    }
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Max system prompt chars", "0", state->alloc, &state->agent_max_system_prompt_chars);
    }
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Channel ack reactions", "false", state->alloc, &state->channels_ack_reactions);
    }
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Show tool calls in channels", "false", state->alloc, &state->channels_show_tool_calls);
    }
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Max seen channel message ids", "1024", state->alloc, &state->channels_max_seen_message_ids);
    }
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Telegram post reactions", "false", state->alloc, &state->telegram_post_reactions);
    }
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Telegram poll timeout seconds", "30", state->alloc, &state->telegram_poll_timeout_seconds);
    }
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Telegram message split bytes", "3900", state->alloc, &state->telegram_message_split_bytes);
    }
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Enable heartbeat", "false", state->alloc, &state->heartbeat_enabled);
    }
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Heartbeat state path", "heartbeat.state", state->alloc, &state->heartbeat_state_path);
    }
    return status;
}

static sc_status collect_user_profile(prompt_context *ctx, onboard_state *state)
{
    sc_status status;

    if (ctx == nullptr || state == nullptr) {
        return sc_status_invalid_argument("sc.onboard.invalid_argument");
    }
    status = prompt_line(ctx, "Your name", "Yehor", state->alloc, &state->user_name);
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Timezone", "Europe/Kyiv", state->alloc, &state->user_timezone);
    }
    if (sc_status_is_ok(status)) {
        status = prompt_line(ctx, "Languages", "English", state->alloc, &state->user_languages);
    }
    return status;
}

static sc_status build_managed_config(onboard_state *state, sc_string *out)
{
    sc_string_builder b = {0};
    sc_status status;

    sc_string_builder_init(&b, state->alloc);
    status = sc_string_builder_append_cstr(&b,
                                           "schema_version = 2\n\n"
                                           "[runtime]\n"
                                           "autonomy_level = \"supervised\"\n"
                                           "workspace_path = \"workspace\"\n\n"
                                           "[autonomy]\n"
                                           "level = \"supervised\"\n"
                                           "shell_enabled = false\n"
                                           "workspace_only = true\n"
                                           "auto_approve = []\n"
                                           "always_ask = []\n"
                                           "never_allow = []\n\n"
                                           "[agent.tool_receipts]\n"
                                           "enabled = true\n"
                                           "show_in_response = false\n"
                                           "inject_system_prompt = true\n\n"
                                           "[agent]\n"
                                           "max_history_messages = ");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&b, string_or_default(&state->agent_max_history_messages, "50"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b, "\nmax_tool_iterations = ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&b, string_or_default(&state->agent_max_tool_iterations, "10"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b, "\nmax_tool_result_chars = ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&b, string_or_default(&state->agent_max_tool_result_chars, "50000"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b, "\nmax_system_prompt_chars = ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&b, string_or_default(&state->agent_max_system_prompt_chars, "0"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b,
                                           "\n\n"
                                           "[gateway]\n"
                                           "enabled = false\n"
                                           "listener_enabled = false\n"
                                           "bind = \"127.0.0.1\"\n"
                                           "port = 8080\n"
                                           "public_bind_enabled = false\n\n"
                                           "[memory]\n"
                                           "backend = \"sqlite\"\n\n"
                                           "[reliability]\n"
                                           "max_retries = 2\n"
                                           "retry_backoff_ms = 250\n"
                                           "timeout_ms = 30000\n\n"
                                           "[heartbeat]\n"
                                           "enabled = ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&b, string_or_default(&state->heartbeat_enabled, "false"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b, "\nstate_path = ");
    }
    if (sc_status_is_ok(status)) {
        status = append_toml_string(&b, string_or_default(&state->heartbeat_state_path, "heartbeat.state"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b, "\n\n");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b, "[security.sandbox]\nbackend = ");
    }
    if (sc_status_is_ok(status)) {
        status = append_toml_string(&b,
                                    string_empty(&state->sandbox_backend)
                                        ? sc_str_from_cstr("auto")
                                        : sc_string_as_str(&state->sandbox_backend));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b, "\nnetwork = \"full\"\nimage_name = ");
    }
    if (sc_status_is_ok(status)) {
        status = append_toml_string(&b,
                                    string_empty(&state->sandbox_image_name)
                                        ? sc_str_from_cstr(ONBOARD_DEFAULT_SANDBOX_IMAGE)
                                        : sc_string_as_str(&state->sandbox_image_name));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b,
                                               "\nfallback_order = [\"landlock\", \"bubblewrap\", \"firejail\", \"docker\", \"podman\"]\n"
                                               "allow_noop_fallback = false");
    }
    if (sc_status_is_ok(status) && !string_empty(&state->sandbox_docker_path)) {
        status = sc_string_builder_append_cstr(&b, "\ndocker_path = ");
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->sandbox_docker_path));
        }
    }
    if (sc_status_is_ok(status) && !string_empty(&state->sandbox_podman_path)) {
        status = sc_string_builder_append_cstr(&b, "\npodman_path = ");
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->sandbox_podman_path));
        }
    }
    if (sc_status_is_ok(status) && !string_empty(&state->sandbox_container_runtime)) {
        status = sc_string_builder_append_cstr(&b, "\ncontainer_runtime = ");
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->sandbox_container_runtime));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b, "\n\n");
    }
    if (sc_status_is_ok(status) && !string_empty(&state->provider_alias)) {
        status = sc_string_builder_append_cstr(&b, "[providers]\nfallback = ");
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->provider_alias));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\n\n[providers.models.");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&b, sc_string_as_str(&state->provider_alias));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "]\nkind = ");
        }
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->provider_kind));
        }
        if (sc_status_is_ok(status) && !string_empty(&state->provider_model)) {
            status = sc_string_builder_append_cstr(&b, "\nmodel = ");
            if (sc_status_is_ok(status)) {
                status = append_toml_string(&b, sc_string_as_str(&state->provider_model));
            }
        }
        if (sc_status_is_ok(status) && !string_empty(&state->provider_base_url)) {
            status = sc_string_builder_append_cstr(&b, "\nbase_url = ");
            if (sc_status_is_ok(status)) {
                status = append_toml_string(&b, sc_string_as_str(&state->provider_base_url));
            }
        }
        if (sc_status_is_ok(status) && !string_empty(&state->provider_deployment)) {
            status = sc_string_builder_append_cstr(&b, "\ndeployment = ");
            if (sc_status_is_ok(status)) {
                status = append_toml_string(&b, sc_string_as_str(&state->provider_deployment));
            }
        }
        if (sc_status_is_ok(status) && !string_empty(&state->provider_api_version)) {
            status = sc_string_builder_append_cstr(&b, "\napi_version = ");
            if (sc_status_is_ok(status)) {
                status = append_toml_string(&b, sc_string_as_str(&state->provider_api_version));
            }
        }
        if (sc_status_is_ok(status) && !string_empty(&state->provider_region)) {
            status = sc_string_builder_append_cstr(&b, "\nregion = ");
            if (sc_status_is_ok(status)) {
                status = append_toml_string(&b, sc_string_as_str(&state->provider_region));
            }
        }
        if (sc_status_is_ok(status) && !string_empty(&state->provider_credential_env)) {
            status = sc_string_builder_append_cstr(&b, "\ncredential_env = ");
            if (sc_status_is_ok(status)) {
                status = append_toml_string(&b, sc_string_as_str(&state->provider_credential_env));
            }
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\n\n");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b,
                                               "[logging]\n"
                                               "level = \"info\"\n"
                                               "format = \"console\"\n"
                                               "\n"
                                               "[logging.daemon]\n"
                                               "file_enabled = true\n"
                                               "file_path = \"logs/smolclaw-daemon.log\"\n"
                                               "\n");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&b,
                                               "[channels]\n"
                                               "session_persistence = true\n"
                                               "session_backend = \"sqlite\"\n"
                                               "ack_reactions = ");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&b, string_or_default(&state->channels_ack_reactions, "false"));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\nshow_tool_calls = ");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&b, string_or_default(&state->channels_show_tool_calls, "false"));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\nmax_seen_message_ids = ");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&b, string_or_default(&state->channels_max_seen_message_ids, "1024"));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\n\n");
        }
    }
    if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&state->channel_kind), sc_str_from_cstr("telegram"))) {
        status = sc_string_builder_append_cstr(&b,
                                               "[channels.telegram]\n"
                                               "enabled = true\n"
                                               "bot_token_env = \"\"\n"
                                               "bot_token = \"\"\n"
                                               "allowed_users = ");
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&b, sc_string_as_str(&state->telegram_allowed_users));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b,
                                                   "\nmention_only = false\n"
                                                   "interrupt_on_new_message = false\n"
                                                   "post_reactions = ");
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&b, string_or_default(&state->telegram_post_reactions, "false"));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&b,
                                                   "\n"
                                                   "stream_mode = \"off\"\n"
                                                   "poll_timeout_seconds = ");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&b, string_or_default(&state->telegram_poll_timeout_seconds, "30"));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&b, "\nmessage_split_bytes = ");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append(&b, string_or_default(&state->telegram_message_split_bytes, "3900"));
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&b,
                                                   "\n"
                                                   "draft_update_interval_ms = 1000\n"
                                                   "approval_timeout_secs = 120\n\n");
            }
        }
    } else if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&state->channel_kind), sc_str_from_cstr("webhook"))) {
        status = sc_string_builder_append_cstr(&b, "[channels.webhook]\nenabled = true\nbind = ");
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->webhook_bind));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\nport = ");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&b, sc_string_as_str(&state->webhook_port));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\npath = ");
        }
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->webhook_path));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\nauth_token = \"\"\n\n");
        }
    } else if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&state->channel_kind), sc_str_from_cstr("rabbitmq"))) {
        status = sc_string_builder_append_cstr(&b, "[channels.rabbitmq]\nenabled = true\nurl = \"\"\nexchange = ");
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->rabbitmq_exchange));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\nrouting_key = ");
        }
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->rabbitmq_routing_key));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\nqueue = ");
        }
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->rabbitmq_queue));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\nconsumer_tag = \"smolclaw-c\"\nprefetch = 1\ndurable = true\n\n");
        }
    } else if (sc_status_is_ok(status) && sc_str_equal(sc_string_as_str(&state->channel_kind), sc_str_from_cstr("mail"))) {
        status = sc_string_builder_append_cstr(&b, "[channels.mail]\nenabled = true\ninbox_url = ");
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->mail_inbox_url));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\nsmtp_url = ");
        }
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->mail_smtp_url));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\nusername = ");
        }
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->mail_username));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\npassword = \"\"\nfrom = ");
        }
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->mail_from));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\nto = ");
        }
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&b, sc_string_as_str(&state->mail_to));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&b, "\nmax_message_bytes = 1048576\ndelete_after_read = false\n\n");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&b, out);
    } else {
        sc_string_builder_clear(&b);
    }
    return status;
}

static sc_status dirname_to_buffer(char *out, size_t capacity, const char *path)
{
    size_t len = 0;

    if (out == nullptr || capacity == 0 || path == nullptr || path[0] == '\0') {
        return sc_status_invalid_argument("sc.onboard.path_invalid");
    }
    len = strlen(path);
    while (len > 0 && path[len - 1U] != '/') {
        len -= 1U;
    }
    if (len == 0) {
        return sc_app_copy_cstr_to_buffer(out, capacity, ".", "sc.onboard.path_invalid");
    }
    if (len >= capacity) {
        return sc_status_invalid_argument("sc.onboard.path_invalid");
    }
    memcpy(out, path, len == 1U ? 1U : len - 1U);
    out[len == 1U ? 1U : len - 1U] = '\0';
    return sc_status_ok();
}

static sc_status write_onboard_workspace_files(onboard_state *state)
{
    char workspace_dir[4096] = {0};
    char path[4096] = {0};
    const char *workspace_env = getenv("SMOLCLAW_WORKSPACE");
    sc_status status;

    if (state == nullptr || state->config_path.ptr == nullptr) {
        return sc_status_invalid_argument("sc.onboard.workspace_invalid_argument");
    }

    if (sc_app_env_truthy(workspace_env)) {
        status = sc_app_copy_cstr_to_buffer(workspace_dir,
                                            sizeof(workspace_dir),
                                            workspace_env,
                                            "sc.onboard.workspace_path_invalid");
    } else {
        char config_dir[4096] = {0};

        status = dirname_to_buffer(config_dir, sizeof(config_dir), state->config_path.ptr);
        if (sc_status_is_ok(status)) {
            status = sc_app_join_cstr_path(workspace_dir,
                                           sizeof(workspace_dir),
                                           config_dir,
                                           "workspace",
                                           "sc.onboard.workspace_path_invalid");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_app_ensure_cli_dir(workspace_dir);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(ONBOARD_WORKSPACE_FILES); i += 1U) {
        sc_string rendered = {0};
        sc_str body = sc_str_from_cstr(ONBOARD_WORKSPACE_FILES[i].body);

        status = sc_app_join_cstr_path(path,
                                       sizeof(path),
                                       workspace_dir,
                                       ONBOARD_WORKSPACE_FILES[i].name,
                                       "sc.onboard.workspace_file_path_invalid");
        if (sc_status_is_ok(status) && strcmp(ONBOARD_WORKSPACE_FILES[i].name, "USER.md") == 0) {
            status = build_user_workspace_file(state, &rendered);
            if (sc_status_is_ok(status)) {
                body = sc_string_as_str(&rendered);
            }
        }
        if (sc_status_is_ok(status) && !sc_app_file_exists(path)) {
            status = sc_app_write_text_file(path,
                                            body,
                                            "sc.onboard.workspace_file_open_failed",
                                            "sc.onboard.workspace_file_write_failed",
                                            "sc.onboard.workspace_file_close_failed");
        }
        sc_string_clear(&rendered);
    }
    return status;
}

static sc_status build_user_workspace_file(onboard_state *state, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    if (state == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.onboard.user_template_invalid_argument");
    }
    sc_string_builder_init(&builder, state->alloc);
    status = sc_string_builder_append_cstr(&builder,
                                           "# USER.md - Who You're Helping\n"
                                           "\n"
                                           "SmolClaw reads this file every session to understand the user.\n"
                                           "\n"
                                           "## About You\n"
                                           "\n"
                                           "- Name: ");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, string_or_default(&state->user_name, "Yehor"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\n- Timezone: ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, string_or_default(&state->user_timezone, "Europe/Kyiv"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\n- Languages: ");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, string_or_default(&state->user_languages, "English"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder,
                                               "\n\n"
                                               "## Communication Style\n"
                                               "\n"
                                               "- Be direct and concise. Skip pleasantries. Get to the point.\n"
                                               "\n"
                                               "## Preferences\n"
                                               "\n"
                                               "- Add durable preferences here.\n"
                                               "\n"
                                               "## Work Context\n"
                                               "\n"
                                               "- Add durable work context here.\n");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status write_secret_values(onboard_state *state)
{
    sc_secret_store *store = nullptr;
    sc_status status = sc_secret_store_file_new(state->alloc, sc_string_as_str(&state->secret_path), &store);

    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (!string_empty(&state->provider_secret) && !string_empty(&state->provider_secret_path)) {
        status = sc_secret_store_put(store, sc_string_as_str(&state->provider_secret_path), sc_string_as_str(&state->provider_secret));
    }
    if (sc_status_is_ok(status) && !string_empty(&state->channel_secret) && !string_empty(&state->channel_secret_path)) {
        status = sc_secret_store_put(store, sc_string_as_str(&state->channel_secret_path), sc_string_as_str(&state->channel_secret));
    }
    sc_secret_store_destroy(store);
    return status;
}

static sc_status validate_written_config(onboard_state *state)
{
    sc_string body = {0};
    sc_config config = {0};
    sc_config_diag diag = {0};
    sc_secret_store *store = nullptr;
    sc_status status;

    status = sc_app_read_text_file(state->alloc, state->config_path.ptr, &body);
    if (status.code == SC_ERR_IO && status.error_key != nullptr && strcmp(status.error_key, "sc.cli.read_file.open_failed") == 0) {
        sc_status_clear(&status);
        status = sc_status_io("sc.onboard.validate_open_failed");
    } else if (status.code == SC_ERR_IO) {
        sc_status_clear(&status);
        status = sc_status_io("sc.onboard.validate_read_failed");
    }
    if (sc_status_is_ok(status)) {
        status = sc_config_load(state->alloc,
                                &(sc_config_load_options){
                                    .explicit_file = {
                                        .kind = SC_CONFIG_SOURCE_EXPLICIT_FILE,
                                        .source_path = sc_string_as_str(&state->config_path),
                                        .body = sc_string_as_str(&body),
                                        .present = true,
                                    },
                                },
                                &config,
                                &diag);
    }
    if (sc_status_is_ok(status)) {
        status = sc_secret_store_file_new(state->alloc, sc_string_as_str(&state->secret_path), &store);
    }
    if (sc_status_is_ok(status)) {
        status = sc_config_attach_secret_store(&config, store, true);
    }
    sc_config_diag_clear(&diag);
    sc_config_clear(&config);
    sc_secret_store_destroy(store);
    sc_string_clear(&body);
    return status;
}

static void onboard_state_clear(onboard_state *state)
{
    if (state == nullptr) {
        return;
    }
    sc_string_clear(&state->config_path);
    sc_string_clear(&state->secret_path);
    sc_string_clear(&state->provider_alias);
    sc_string_clear(&state->user_name);
    sc_string_clear(&state->user_timezone);
    sc_string_clear(&state->user_languages);
    sc_string_clear(&state->provider_kind);
    sc_string_clear(&state->provider_model);
    sc_string_clear(&state->provider_base_url);
    sc_string_clear(&state->provider_deployment);
    sc_string_clear(&state->provider_api_version);
    sc_string_clear(&state->provider_region);
    sc_string_clear(&state->provider_credential_env);
    sc_string_secure_clear(&state->provider_secret);
    sc_string_clear(&state->provider_secret_path);
    sc_string_clear(&state->channel_kind);
    sc_string_secure_clear(&state->channel_secret);
    sc_string_clear(&state->channel_secret_path);
    sc_string_clear(&state->telegram_allowed_users);
    sc_string_clear(&state->webhook_bind);
    sc_string_clear(&state->webhook_port);
    sc_string_clear(&state->webhook_path);
    sc_string_clear(&state->rabbitmq_exchange);
    sc_string_clear(&state->rabbitmq_routing_key);
    sc_string_clear(&state->rabbitmq_queue);
    sc_string_clear(&state->mail_inbox_url);
    sc_string_clear(&state->mail_smtp_url);
    sc_string_clear(&state->mail_username);
    sc_string_clear(&state->mail_from);
    sc_string_clear(&state->mail_to);
    sc_string_clear(&state->sandbox_backend);
    sc_string_clear(&state->sandbox_docker_path);
    sc_string_clear(&state->sandbox_podman_path);
    sc_string_clear(&state->sandbox_container_runtime);
    sc_string_clear(&state->sandbox_image_name);
    sc_string_clear(&state->agent_max_history_messages);
    sc_string_clear(&state->agent_max_tool_iterations);
    sc_string_clear(&state->agent_max_tool_result_chars);
    sc_string_clear(&state->agent_max_system_prompt_chars);
    sc_string_clear(&state->channels_ack_reactions);
    sc_string_clear(&state->channels_show_tool_calls);
    sc_string_clear(&state->channels_max_seen_message_ids);
    sc_string_clear(&state->telegram_post_reactions);
    sc_string_clear(&state->telegram_poll_timeout_seconds);
    sc_string_clear(&state->telegram_message_split_bytes);
    sc_string_clear(&state->heartbeat_enabled);
    sc_string_clear(&state->heartbeat_state_path);
}

static const char *provider_kind_name(onboard_provider_kind kind)
{
    switch (kind) {
    case ONBOARD_PROVIDER_OPENAI:
        return "openai";
    case ONBOARD_PROVIDER_ANTHROPIC:
        return "anthropic";
    case ONBOARD_PROVIDER_GEMINI:
        return "gemini";
    case ONBOARD_PROVIDER_OPENROUTER:
        return "openrouter";
    case ONBOARD_PROVIDER_AZURE_OPENAI:
        return "azure-openai";
    case ONBOARD_PROVIDER_BEDROCK:
        return "bedrock";
    case ONBOARD_PROVIDER_OLLAMA:
        return "ollama";
    case ONBOARD_PROVIDER_OPENAI_COMPATIBLE:
        return "openai-compatible";
    case ONBOARD_PROVIDER_GEMINI_CLI:
        return "gemini-cli";
    case ONBOARD_PROVIDER_CLAUDE_CODE:
        return "claude-code";
    case ONBOARD_PROVIDER_COPILOT:
        return "copilot";
    }
    return "openai";
}

static const char *provider_default_alias(onboard_provider_kind kind)
{
    switch (kind) {
    case ONBOARD_PROVIDER_OPENAI:
        return "gpt";
    case ONBOARD_PROVIDER_ANTHROPIC:
        return "claude";
    case ONBOARD_PROVIDER_GEMINI:
        return "gemini";
    case ONBOARD_PROVIDER_OPENROUTER:
        return "openrouter";
    case ONBOARD_PROVIDER_AZURE_OPENAI:
        return "azure";
    case ONBOARD_PROVIDER_BEDROCK:
        return "bedrock";
    case ONBOARD_PROVIDER_OLLAMA:
        return "local";
    case ONBOARD_PROVIDER_OPENAI_COMPATIBLE:
        return "compatible";
    case ONBOARD_PROVIDER_GEMINI_CLI:
        return "gemini-cli";
    case ONBOARD_PROVIDER_CLAUDE_CODE:
        return "cc";
    case ONBOARD_PROVIDER_COPILOT:
        return "copilot";
    }
    return "gpt";
}

static const char *provider_default_model(onboard_provider_kind kind)
{
    switch (kind) {
    case ONBOARD_PROVIDER_OPENAI:
        return "gpt-4o-mini";
    case ONBOARD_PROVIDER_ANTHROPIC:
        return "claude-haiku-4-5-20251001";
    case ONBOARD_PROVIDER_GEMINI:
    case ONBOARD_PROVIDER_GEMINI_CLI:
        return "gemini-2.5-flash";
    case ONBOARD_PROVIDER_OPENROUTER:
        return "openai/gpt-4o-mini";
    case ONBOARD_PROVIDER_AZURE_OPENAI:
        return "gpt-4o";
    case ONBOARD_PROVIDER_BEDROCK:
        return "anthropic.claude-3-5-sonnet-20241022-v2:0";
    case ONBOARD_PROVIDER_OLLAMA:
        return "llama3.2";
    case ONBOARD_PROVIDER_OPENAI_COMPATIBLE:
        return "model";
    case ONBOARD_PROVIDER_COPILOT:
        return "gpt-4o";
    case ONBOARD_PROVIDER_CLAUDE_CODE:
        return "";
    }
    return "gpt-4o-mini";
}

static const char *provider_default_env(onboard_provider_kind kind)
{
    switch (kind) {
    case ONBOARD_PROVIDER_OPENAI:
    case ONBOARD_PROVIDER_AZURE_OPENAI:
        return "OPENAI_API_KEY";
    case ONBOARD_PROVIDER_ANTHROPIC:
        return "ANTHROPIC_API_KEY";
    case ONBOARD_PROVIDER_GEMINI:
        return "GEMINI_API_KEY";
    case ONBOARD_PROVIDER_OPENROUTER:
        return "OPENROUTER_API_KEY";
    case ONBOARD_PROVIDER_COPILOT:
        return "GITHUB_COPILOT_TOKEN";
    default:
        return "";
    }
}

static bool provider_needs_secret(onboard_provider_kind kind)
{
    return kind == ONBOARD_PROVIDER_OPENAI || kind == ONBOARD_PROVIDER_ANTHROPIC || kind == ONBOARD_PROVIDER_GEMINI ||
           kind == ONBOARD_PROVIDER_OPENROUTER || kind == ONBOARD_PROVIDER_AZURE_OPENAI ||
           kind == ONBOARD_PROVIDER_OPENAI_COMPATIBLE || kind == ONBOARD_PROVIDER_COPILOT ||
           kind == ONBOARD_PROVIDER_BEDROCK;
}

static bool provider_uses_env_fallback(onboard_provider_kind kind)
{
    return provider_default_env(kind)[0] != '\0';
}

static const char *sandbox_backend_name(onboard_sandbox_kind kind)
{
    switch (kind) {
    case ONBOARD_SANDBOX_AUTO:
        return "auto";
    case ONBOARD_SANDBOX_DOCKER:
        return "docker";
    case ONBOARD_SANDBOX_PODMAN:
        return "podman";
    case ONBOARD_SANDBOX_CONTAINER:
        return "container";
    case ONBOARD_SANDBOX_NOOP:
        return "noop";
    }
    return "auto";
}

static bool string_empty(const sc_string *value)
{
    return value == nullptr || value->ptr == nullptr || value->len == 0;
}

static sc_str string_or_default(const sc_string *value, const char *fallback)
{
    return string_empty(value) ? sc_str_from_cstr(fallback == nullptr ? "" : fallback) : sc_string_as_str(value);
}

static sc_status copy_cstr(sc_allocator *alloc, const char *value, sc_string *out)
{
    return sc_string_from_cstr(alloc, value == nullptr ? "" : value, out);
}

static sc_status normalize_telegram_allowed_users(sc_allocator *alloc, sc_str input, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;
    bool first = true;
    size_t index = 0;
    size_t trimmed = 0;

    if (out == nullptr || (input.len > 0 && input.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.onboard.telegram_users_invalid_argument");
    }

    sc_string_clear(out);
    while (trimmed < input.len &&
           (input.ptr[trimmed] == ' ' || input.ptr[trimmed] == '\t' ||
            input.ptr[trimmed] == '\r' || input.ptr[trimmed] == '\n')) {
        trimmed += 1;
    }
    if (trimmed < input.len && input.ptr[trimmed] == '[') {
        return sc_string_from_str(alloc, input, out);
    }

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "[");
    while (sc_status_is_ok(status) && index < input.len) {
        size_t start = 0;
        size_t end = 0;
        sc_str user = {0};

        while (index < input.len &&
               (input.ptr[index] == ',' || input.ptr[index] == ';' ||
                input.ptr[index] == '\n' || input.ptr[index] == '\r' ||
                input.ptr[index] == '\t' || input.ptr[index] == ' ')) {
            index += 1;
        }
        start = index;
        while (index < input.len &&
               input.ptr[index] != ',' && input.ptr[index] != ';' &&
               input.ptr[index] != '\n' && input.ptr[index] != '\r' &&
               input.ptr[index] != '\t' && input.ptr[index] != ' ') {
            index += 1;
        }
        end = index;
        while (start < end && input.ptr[start] == '@') {
            start += 1;
        }
        if (end <= start) {
            continue;
        }
        user = sc_str_from_parts(input.ptr + start, end - start);
        if (!first) {
            status = sc_string_builder_append_cstr(&builder, ", ");
        }
        if (sc_status_is_ok(status)) {
            status = append_toml_string(&builder, user);
        }
        first = false;
    }
    if (sc_status_is_ok(status) && first) {
        status = append_toml_string(&builder, sc_str_from_cstr("*"));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "]");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_toml_string(sc_string_builder *builder, sc_str value)
{
    sc_status status = sc_string_builder_append_cstr(builder, "\"");

    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        char ch = value.ptr[i];
        if (ch == '\\' || ch == '"') {
            char escaped[3] = {'\\', ch, '\0'};
            status = sc_string_builder_append_cstr(builder, escaped);
        } else {
            status = sc_string_builder_append(builder, sc_str_from_parts(&ch, 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    return status;
}
