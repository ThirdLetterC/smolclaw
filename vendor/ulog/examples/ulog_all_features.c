#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "ulog/ulog.h"
#include "ulog_generic_interface.h"
#include "ulog_syslog.h"

typedef struct {
    bool locked;
    unsigned depth;
} example_lock_state;

typedef struct {
    FILE *stream;
    unsigned lines;
} example_output_state;

static constexpr size_t EXAMPLE_LINE_SIZE = 256U;
static constexpr size_t EXAMPLE_MSG_SIZE  = 128U;
static constexpr size_t EXAMPLE_TIME_SIZE = 16U;

static void print_status(const char *label, ulog_status status) {
    if (status == ULOG_STATUS_OK) {
        return;
    }

    const char *name = ulog_level_to_string(ULOG_LEVEL_ERROR);
    if (name == nullptr) {
        name = "ERROR";
    }

    fprintf(stderr, "%s: %s (%d)\n", name, label, (int)status);
}

static ulog_status example_lock(bool lock, void *arg) {
    if (arg == nullptr) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }

    auto state = (example_lock_state *)arg;
    if (lock) {
        state->locked = true;
        state->depth++;
    } else {
        if (state->depth > 0U) {
            state->depth--;
        }
        if (state->depth == 0U) {
            state->locked = false;
        }
    }

    return ULOG_STATUS_OK;
}

static void example_prefix(ulog_event *ev, char *prefix, size_t prefix_size) {
    if ((prefix == nullptr) || (prefix_size == 0U)) {
        return;
    }
    if (ev == nullptr) {
        prefix[0] = '\0';
        return;
    }

    const char *file = ulog_event_get_file(ev);
    if (file == nullptr) {
        file = "?";
    }

    auto level             = ulog_event_get_level(ev);
    const char *level_name = ulog_level_to_string(level);
    if (level_name == nullptr) {
        level_name = "?";
    }

    char time_buf[EXAMPLE_TIME_SIZE] = {0};
    auto ts                          = ulog_event_get_time(ev);
    if (ts != nullptr) {
        (void)strftime(time_buf, sizeof(time_buf), "%H:%M:%S", ts);
    }

    const int line = ulog_event_get_line(ev);
    (void)snprintf(prefix, prefix_size, "[%s %s %s:%d] ",
                   (time_buf[0] != '\0') ? time_buf : "--:--:--", level_name,
                   file, line);
}

static void example_output(ulog_event *ev, void *arg) {
    auto state = (example_output_state *)arg;
    if ((state == nullptr) || (state->stream == nullptr)) {
        return;
    }

    char line[EXAMPLE_LINE_SIZE];
    auto status = ulog_event_to_cstr(ev, line, sizeof(line));
    if (status == ULOG_STATUS_OK) {
        fprintf(state->stream, "mirror: %s\n", line);
    } else {
        fprintf(state->stream, "mirror: <format error>\n");
    }

    char msg[EXAMPLE_MSG_SIZE];
    status = ulog_event_get_message(ev, msg, sizeof(msg));
    if (status == ULOG_STATUS_OK) {
        fprintf(state->stream, "message-only: %s\n", msg);
    }

    auto level             = ulog_event_get_level(ev);
    const char *level_name = ulog_level_to_string(level);
    if (level_name == nullptr) {
        level_name = "?";
    }

    const char *file = ulog_event_get_file(ev);
    if (file == nullptr) {
        file = "?";
    }

    auto topic        = ulog_event_get_topic(ev);
    const int line_no = ulog_event_get_line(ev);

    char time_buf[EXAMPLE_TIME_SIZE] = {0};
    auto ts                          = ulog_event_get_time(ev);
    if (ts != nullptr) {
        (void)strftime(time_buf, sizeof(time_buf), "%H:%M:%S", ts);
    }

    fprintf(state->stream, "meta: level=%s file=%s line=%d topic=%d time=%s\n",
            level_name, file, line_no, (int)topic,
            (time_buf[0] != '\0') ? time_buf : "--:--:--");

    state->lines++;
}

int main() {
    example_lock_state lock_state = {.locked = false, .depth = 0U};
    auto status                   = ulog_lock_set_fn(example_lock, &lock_state);
    print_status("ulog_lock_set_fn", status);

    status = ulog_color_config(true);
    print_status("ulog_color_config(true)", status);
    status = ulog_time_config(true);
    print_status("ulog_time_config(true)", status);
    status = ulog_source_location_config(true);
    print_status("ulog_source_location_config(true)", status);
    status = ulog_prefix_set_fn(example_prefix);
    print_status("ulog_prefix_set_fn", status);
    status = ulog_prefix_config(true);
    print_status("ulog_prefix_config(true)", status);
    status = ulog_level_config(ULOG_LEVEL_CONFIG_STYLE_DEFAULT);
    print_status("ulog_level_config(default)", status);
    status = ulog_topic_config(true);
    print_status("ulog_topic_config(true)", status);

    status = ulog_output_level_set_all(ULOG_LEVEL_TRACE);
    print_status("ulog_output_level_set_all", status);

    status = ulog_output_level_set(ULOG_OUTPUT_STDOUT, ULOG_LEVEL_TRACE);
    print_status("ulog_output_level_set(stdout)", status);

    FILE *log_file             = fopen("ulog_all_features.log", "w");
    ulog_output_id file_output = ULOG_OUTPUT_INVALID;
    if (log_file != nullptr) {
        file_output = ulog_output_add_file(log_file, ULOG_LEVEL_DEBUG);
        if (file_output == ULOG_OUTPUT_INVALID) {
            fclose(log_file);
            log_file = nullptr;
        } else {
            status = ulog_output_level_set(file_output, ULOG_LEVEL_TRACE);
            print_status("ulog_output_level_set(file)", status);
        }
    }

    example_output_state mirror_state = {.stream = stderr, .lines = 0U};
    auto mirror_output =
        ulog_output_add(example_output, &mirror_state, ULOG_LEVEL_INFO);
    if (mirror_output == ULOG_OUTPUT_INVALID) {
        fprintf(stderr, "mirror output: failed to add\n");
    }

    ulog_trace("trace message");
    ulog_debug("debug message");
    ulog_info("info message");
    ulog_warn("warn message");
    ulog_error("error message");
    ulog_fatal("fatal message");
    ulog(ULOG_LEVEL_6, "custom level %d", 6);

    const char *warn_name = ulog_level_to_string(ULOG_LEVEL_WARN);
    if (warn_name == nullptr) {
        warn_name = "?";
    }
    ulog_info("level string: %s", warn_name);

    static const ulog_level_descriptor custom_levels = {
        .max_level = ULOG_LEVEL_7,
        .names = {"ZERO ", "ONE  ", "TWO  ", "THREE", "FOUR ", "FIVE ", "SIX  ",
                  "SEVEN"},
    };
    status = ulog_level_set_new_levels(&custom_levels);
    print_status("ulog_level_set_new_levels", status);
    ulog_info("custom level names active");
    status = ulog_level_reset_levels();
    print_status("ulog_level_reset_levels", status);

    status = ulog_syslog_enable();
    print_status("ulog_syslog_enable", status);
    ulog(ULOG_SYSLOG_NOTICE, "syslog notice");
    ulog(ULOG_SYSLOG_ERR, "syslog error");
    status = ulog_syslog_disable();
    print_status("ulog_syslog_disable", status);

    auto net_topic = ulog_topic_add("net", ULOG_OUTPUT_ALL, ULOG_LEVEL_TRACE);
    if (net_topic != ULOG_TOPIC_ID_INVALID) {
        ulog_t_info("net", "link up");
        status = ulog_topic_level_set("net", ULOG_LEVEL_WARN);
        print_status("ulog_topic_level_set", status);
        ulog_t_debug("net", "debug should be filtered");
        ulog_t_warn("net", "link unstable");
        auto net_id = ulog_topic_get_id("net");
        ulog_info("net topic id: %d", (int)net_id);
        status = ulog_topic_remove("net");
        print_status("ulog_topic_remove", status);
    }

    status = ulog_topic_config(false);
    print_status("ulog_topic_config(false)", status);
    ulog_t_info("net", "topics disabled");
    status = ulog_topic_config(true);
    print_status("ulog_topic_config(true)", status);

    status = ulog_level_config(ULOG_LEVEL_CONFIG_STYLE_SHORT);
    print_status("ulog_level_config(short)", status);
    ulog_info("short levels enabled");
    status = ulog_level_config(ULOG_LEVEL_CONFIG_STYLE_DEFAULT);
    print_status("ulog_level_config(default)", status);

    status = ulog_source_location_config(false);
    print_status("ulog_source_location_config(false)", status);
    ulog_info("source location hidden");
    status = ulog_source_location_config(true);
    print_status("ulog_source_location_config(true)", status);

    status = ulog_time_config(false);
    print_status("ulog_time_config(false)", status);
    ulog_info("time hidden");
    status = ulog_time_config(true);
    print_status("ulog_time_config(true)", status);

    status = ulog_color_config(false);
    print_status("ulog_color_config(false)", status);
    ulog_info("color disabled");
    status = ulog_color_config(true);
    print_status("ulog_color_config(true)", status);

    status = ulog_prefix_config(false);
    print_status("ulog_prefix_config(false)", status);
    ulog_info("prefix disabled");
    status = ulog_prefix_config(true);
    print_status("ulog_prefix_config(true)", status);

    log_message((ulog_level)LOG_INFO, "generic interface info");
    log_topic((ulog_level)LOG_WARN, "GEN", "generic interface topic");

    ulog_info("mirror output lines: %u", mirror_state.lines);

    if (mirror_output != ULOG_OUTPUT_INVALID) {
        status = ulog_output_remove(mirror_output);
        print_status("ulog_output_remove(mirror)", status);
    }

    if (file_output != ULOG_OUTPUT_INVALID) {
        status = ulog_output_remove(file_output);
        print_status("ulog_output_remove(file)", status);
    }

    if (log_file != nullptr) {
        fclose(log_file);
    }

    status = ulog_cleanup();
    print_status("ulog_cleanup", status);

    return 0;
}
