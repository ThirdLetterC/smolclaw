#include "app/app_commands.h"
#include "app/app_line_editor.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc/allocator.h"
#include "sc/bootstrap.h"
#include "sc/runtime.h"

static bool chat_str_matches_cstr_table(sc_str line, const char *const *values, size_t value_count);

static bool chat_command_is_exit(sc_str line)
{
    static const char *const commands[] = {"/exit", "/quit", "exit", "quit"};

    return chat_str_matches_cstr_table(line, commands, sizeof(commands) / sizeof(commands[0]));
}

static bool chat_answer_is_yes(sc_str line)
{
    static const char *const answers[] = {"y", "yes", "approve"};

    return chat_str_matches_cstr_table(line, answers, sizeof(answers) / sizeof(answers[0]));
}

static bool chat_answer_is_no(sc_str line)
{
    static const char *const answers[] = {"n", "no", "decline", "deny"};

    return chat_str_matches_cstr_table(line, answers, sizeof(answers) / sizeof(answers[0]));
}

static char chat_ascii_lower(char value)
{
    if (value >= 'A' && value <= 'Z') {
        return (char)(value + ('a' - 'A'));
    }
    return value;
}

static bool chat_str_equal_ascii_case(sc_str left, const char *right)
{
    size_t right_len = 0;

    if (left.ptr == nullptr || right == nullptr) {
        return false;
    }
    right_len = strlen(right);
    if (left.len != right_len) {
        return false;
    }
    for (size_t i = 0; i < left.len; i += 1U) {
        if (chat_ascii_lower(left.ptr[i]) != chat_ascii_lower(right[i])) {
            return false;
        }
    }
    return true;
}

static bool chat_str_matches_cstr_table(sc_str line, const char *const *values, size_t value_count)
{
    if (values == nullptr) {
        return false;
    }
    for (size_t i = 0; i < value_count; i += 1U) {
        if (chat_str_equal_ascii_case(line, values[i])) {
            return true;
        }
    }
    return false;
}

static bool chat_status_is_recoverable_turn_failure(const sc_status *status)
{
    if (status == nullptr || status->error_key == nullptr) {
        return false;
    }
    return status->code == SC_ERR_CANCELLED &&
           (strcmp(status->error_key, "sc.agent.tool_loop.max_iterations") == 0 ||
            strcmp(status->error_key, "sc.agent.tool_loop.cancelled") == 0);
}

static void chat_print_str(FILE *stream, sc_str value, const char *fallback)
{
    if (stream == nullptr) {
        return;
    }
    if (value.ptr == nullptr) {
        (void)fputs(fallback == nullptr ? "" : fallback, stream);
        return;
    }
    if (value.len > 0) {
        (void)fwrite(value.ptr, 1, value.len, stream);
    }
}

static void chat_print_tool_event_line(const sc_turn_event *event, const char *suffix, const char *message_fallback)
{
    (void)fputs("tool> ", stdout);
    chat_print_str(stdout, sc_string_as_str(&event->name), "unknown");
    if (suffix != nullptr) {
        (void)fputs(suffix, stdout);
        chat_print_str(stdout, sc_string_as_str(&event->message), message_fallback);
    }
    (void)fputc('\n', stdout);
    (void)fflush(stdout);
}

static void chat_print_turn_event(void *user_data, const sc_turn_event *event)
{
    (void)user_data;

    if (event == nullptr) {
        return;
    }
    switch (event->type) {
        case SC_TURN_EVENT_TOOL_CALLED:
            chat_print_tool_event_line(event, nullptr, "");
            break;
        case SC_TURN_EVENT_TOOL_DENIED:
            chat_print_tool_event_line(event, " denied: ", "denied");
            break;
        case SC_TURN_EVENT_TOOL_RESULT:
            if (event->status_code != SC_OK) {
                chat_print_tool_event_line(event, " failed: ", "error");
            }
            break;
        default:
            break;
    }
}

static sc_status chat_request_tool_approval(void *user_data,
                                            sc_str tool_name,
                                            sc_str arguments_json,
                                            sc_allocator *alloc,
                                            bool *out_approved)
{
    (void)user_data;

    if (out_approved == nullptr) {
        return sc_status_invalid_argument("sc.cli.chat.approval_invalid_argument");
    }
    *out_approved = false;
    (void)fprintf(stdout, "\ntool approval required\n");
    (void)fputs("tool: ", stdout);
    chat_print_str(stdout, tool_name, "");
    (void)fputc('\n', stdout);
    if (arguments_json.len > 0) {
        (void)fputs("args: ", stdout);
        chat_print_str(stdout, arguments_json, "");
        (void)fputc('\n', stdout);
    }

    while (true) {
        sc_string answer = {0};
        bool eof = false;
        sc_status status = sc_app_line_editor_read_plain(alloc, "approve? [y/N] ", &answer, &eof);

        if (!sc_status_is_ok(status)) {
            return status;
        }
        if (eof || answer.len == 0 || chat_answer_is_no(sc_string_as_str(&answer))) {
            sc_string_clear(&answer);
            *out_approved = false;
            return sc_status_ok();
        }
        if (chat_answer_is_yes(sc_string_as_str(&answer))) {
            sc_string_clear(&answer);
            *out_approved = true;
            return sc_status_ok();
        }
        sc_string_clear(&answer);
        (void)fprintf(stdout, "enter y/yes/approve or n/no/decline\n");
    }
}

static sc_status chat_history_path(sc_allocator *alloc, sc_string *out)
{
    const char *home = getenv("HOME");
    sc_string_builder builder = {0};
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.cli.chat.history_invalid_argument");
    }
    if (home == nullptr || home[0] == '\0') {
        return sc_string_from_cstr(alloc, "", out);
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, home);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/.smolclaw/chat.history");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

int sc_app_run_chat()
{
    sc_allocator *alloc = sc_allocator_heap();
    const char *config_path = getenv("SMOLCLAW_CONFIG");
    const char *workspace_path = getenv("SMOLCLAW_WORKSPACE");
    sc_boot_session *session = nullptr;
    sc_string history_path = {0};
    sc_status status;
    sc_status history_status;
    int exit_code = 0;

    if (config_path == nullptr || config_path[0] == '\0') {
        config_path = "smolclaw.toml";
    }

    status = sc_boot_session_open(alloc,
                                  &(sc_boot_options){
                                      .struct_size = sizeof(sc_boot_options),
                                      .config_path = sc_str_from_cstr(config_path),
                                      .workspace_path = sc_str_from_cstr(workspace_path),
                                  },
                                  &session);
    if (!sc_status_is_ok(status)) {
        sc_app_log_bootstrap_failure(&status);
        sc_app_print_bootstrap_failure(stderr, "chat", &status);
        sc_status_clear(&status);
        return 1;
    }

    history_status = chat_history_path(alloc, &history_path);
    if (sc_status_is_ok(history_status)) {
        sc_app_line_editor_init(sc_string_as_str(&history_path));
    } else {
        sc_app_line_editor_init(sc_str_from_cstr(""));
        sc_status_clear(&history_status);
    }

    (void)fprintf(stdout, "SmolClaw interactive chat. Type /exit to quit.\n");
    while (true) {
        sc_string line = {0};
        sc_runtime_response response = {0};
        bool eof = false;

        status = sc_app_line_editor_read(alloc, "you> ", &line, &eof);
        if (!sc_status_is_ok(status)) {
            sc_app_print_bootstrap_failure(stderr, "chat", &status);
            sc_status_clear(&status);
            exit_code = 1;
            break;
        }
        if (eof || chat_command_is_exit(sc_string_as_str(&line))) {
            sc_string_clear(&line);
            break;
        }
        if (line.len == 0) {
            sc_string_clear(&line);
            continue;
        }

        status = sc_boot_session_process_ex(session,
                                            sc_string_as_str(&line),
                                            chat_request_tool_approval,
                                            nullptr,
                                            chat_print_turn_event,
                                            nullptr,
                                            alloc,
                                            &response);
        sc_string_clear(&line);
        if (!sc_status_is_ok(status)) {
            if (chat_status_is_recoverable_turn_failure(&status)) {
                (void)fprintf(stdout,
                              "assistant> I could not complete that turn: %s\n",
                              status.error_key == nullptr ? "sc.chat.turn_failed" : status.error_key);
                sc_status_clear(&status);
                sc_runtime_response_clear(&response);
                continue;
            }
            sc_app_print_bootstrap_failure(stderr, "chat", &status);
            sc_status_clear(&status);
            sc_runtime_response_clear(&response);
            exit_code = 1;
            break;
        }
        if (response.output.len > 0) {
            (void)fputs("assistant> ", stdout);
            chat_print_str(stdout, sc_string_as_str(&response.output), "");
            (void)fputc('\n', stdout);
        }
        sc_runtime_response_clear(&response);
    }

    sc_string_clear(&history_path);
    sc_boot_session_destroy(session);
    return exit_code;
}
