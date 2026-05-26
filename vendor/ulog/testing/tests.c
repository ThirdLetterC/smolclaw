#include "ulog/ulog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static constexpr size_t capture_message_size  = 128;
static constexpr size_t capture_rendered_size = 256;

typedef struct {
    size_t call_count;
    ulog_level level;
    ulog_topic_id topic;
    int line;
    const char *file;
    bool has_time;
    char message[capture_message_size];
    char rendered[capture_rendered_size];
} capture_t;

static void capture_reset(capture_t *capture) {
    if (capture == nullptr) {
        return;
    }

    memset(capture, 0, sizeof(*capture));
    capture->topic = ULOG_TOPIC_ID_INVALID;
    capture->line  = -1;
}

static void test_fail(const char *test_name, int line, const char *check) {
    fprintf(stderr, "FAIL %s:%d: %s\n", test_name, line, check);
}

#define EXPECT_TRUE(EXPR)                                                      \
    do {                                                                       \
        if (!(EXPR)) {                                                         \
            test_fail(__func__, __LINE__, #EXPR);                              \
            return false;                                                      \
        }                                                                      \
    } while (false)

#define EXPECT_STATUS(ACTUAL, EXPECTED)                                        \
    do {                                                                       \
        auto actual_status   = (ACTUAL);                                       \
        auto expected_status = (EXPECTED);                                     \
        if (actual_status != expected_status) {                                \
            fprintf(stderr, "FAIL %s:%d: %s == %s (got %d, expected %d)\n",    \
                    __func__, __LINE__, #ACTUAL, #EXPECTED,                    \
                    (int)actual_status, (int)expected_status);                 \
            return false;                                                      \
        }                                                                      \
    } while (false)

#define EXPECT_STREQ(ACTUAL, EXPECTED)                                         \
    do {                                                                       \
        auto actual_str   = (ACTUAL);                                          \
        auto expected_str = (EXPECTED);                                        \
        if (actual_str == nullptr || strcmp(actual_str, expected_str) != 0) {  \
            fprintf(stderr, "FAIL %s:%d: %s == \"%s\" (got \"%s\")\n",         \
                    __func__, __LINE__, #ACTUAL, expected_str,                 \
                    actual_str != nullptr ? actual_str : "(null)");            \
            return false;                                                      \
        }                                                                      \
    } while (false)

#define EXPECT_CONTAINS(HAYSTACK, NEEDLE)                                      \
    do {                                                                       \
        auto haystack = (HAYSTACK);                                            \
        auto needle   = (NEEDLE);                                              \
        if (haystack == nullptr || strstr(haystack, needle) == nullptr) {      \
            fprintf(stderr, "FAIL %s:%d: %s contains \"%s\" (got \"%s\")\n",   \
                    __func__, __LINE__, #HAYSTACK, needle,                     \
                    haystack != nullptr ? haystack : "(null)");                \
            return false;                                                      \
        }                                                                      \
    } while (false)

static void capture_output(ulog_event *ev, void *arg) {
    auto capture = (capture_t *)arg;
    if (capture == nullptr) {
        return;
    }

    capture->call_count += 1;
    capture->level    = ulog_event_get_level(ev);
    capture->topic    = ulog_event_get_topic(ev);
    capture->line     = ulog_event_get_line(ev);
    capture->file     = ulog_event_get_file(ev);
    capture->has_time = ulog_event_get_time(ev) != nullptr;

    auto message_status =
        ulog_event_get_message(ev, capture->message, sizeof(capture->message));
    if (message_status != ULOG_STATUS_OK) {
        capture->message[0] = '\0';
    }

    auto rendered_status =
        ulog_event_to_cstr(ev, capture->rendered, sizeof(capture->rendered));
    if (rendered_status != ULOG_STATUS_OK) {
        capture->rendered[0] = '\0';
    }
}

static ulog_output_id capture_output_add(capture_t *capture, ulog_level level) {
    capture_reset(capture);
    return ulog_output_add(capture_output, capture, level);
}

static bool reset_ulog() {
    EXPECT_STATUS(ulog_lock_set_fn(nullptr, nullptr), ULOG_STATUS_OK);
    EXPECT_STATUS(ulog_cleanup(), ULOG_STATUS_OK);
    EXPECT_STATUS(ulog_output_level_set(ULOG_OUTPUT_STDOUT, ULOG_LEVEL_FATAL),
                  ULOG_STATUS_OK);
    return true;
}

static void fixed_prefix([[maybe_unused]] ulog_event *ev, char *prefix,
                         size_t prefix_size) {
    (void)snprintf(prefix, prefix_size, "[PFX]");
}

static bool test_level_configuration() {
    EXPECT_TRUE(reset_ulog());

    EXPECT_STREQ(ulog_level_to_string(ULOG_LEVEL_TRACE), "TRACE");
    EXPECT_STREQ(ulog_level_to_string(ULOG_LEVEL_FATAL), "FATAL");

    auto custom_levels = (ulog_level_descriptor){
        .max_level = ULOG_LEVEL_6,
        .names     = {"TRC", "DBG", "INF", "WRN", "ERR", "FTL", "CRT"},
    };

    EXPECT_STATUS(ulog_level_set_new_levels(&custom_levels), ULOG_STATUS_OK);
    EXPECT_STREQ(ulog_level_to_string(ULOG_LEVEL_TRACE), "TRC");
    EXPECT_STREQ(ulog_level_to_string(ULOG_LEVEL_6), "CRT");
    EXPECT_STREQ(ulog_level_to_string(ULOG_LEVEL_7), "?");

    EXPECT_STATUS(ulog_level_reset_levels(), ULOG_STATUS_OK);
    EXPECT_STREQ(ulog_level_to_string(ULOG_LEVEL_TRACE), "TRACE");
    EXPECT_STREQ(ulog_level_to_string(ULOG_LEVEL_6), "?");
    return true;
}

static bool test_output_filtering_and_event_accessors() {
    EXPECT_TRUE(reset_ulog());

    auto capture = (capture_t){0};
    auto output  = capture_output_add(&capture, ULOG_LEVEL_INFO);
    EXPECT_TRUE(output != ULOG_OUTPUT_INVALID);

    ulog_debug("hidden");
    EXPECT_TRUE(capture.call_count == 0U);

    ulog_info("value %d", 7);
    EXPECT_TRUE(capture.call_count == 1U);
    EXPECT_TRUE(capture.level == ULOG_LEVEL_INFO);
    EXPECT_TRUE(capture.topic == ULOG_TOPIC_ID_INVALID);
    EXPECT_TRUE(capture.line > 0);
    EXPECT_TRUE(capture.file != nullptr);
    EXPECT_TRUE(capture.has_time);
    EXPECT_STREQ(capture.message, "value 7");
    EXPECT_CONTAINS(capture.file, "tests.c");
    EXPECT_CONTAINS(capture.rendered, "INFO ");
    EXPECT_CONTAINS(capture.rendered, "value 7");

    EXPECT_STATUS(ulog_output_remove(output), ULOG_STATUS_OK);
    ulog_info("ignored");
    EXPECT_TRUE(capture.call_count == 1U);
    EXPECT_STATUS(ulog_output_remove(output), ULOG_STATUS_NOT_FOUND);
    return true;
}

static bool test_topics_route_to_configured_output() {
    EXPECT_TRUE(reset_ulog());

    auto primary_capture   = (capture_t){0};
    auto secondary_capture = (capture_t){0};
    auto primary_output =
        capture_output_add(&primary_capture, ULOG_LEVEL_TRACE);
    auto secondary_output =
        capture_output_add(&secondary_capture, ULOG_LEVEL_TRACE);

    EXPECT_TRUE(primary_output != ULOG_OUTPUT_INVALID);
    EXPECT_TRUE(secondary_output != ULOG_OUTPUT_INVALID);

    auto topic_id = ulog_topic_add("net", primary_output, ULOG_LEVEL_INFO);
    EXPECT_TRUE(topic_id != ULOG_TOPIC_ID_INVALID);
    EXPECT_TRUE(ulog_topic_get_id("net") == topic_id);

    ulog_t_debug("net", "skip");
    EXPECT_TRUE(primary_capture.call_count == 0U);
    EXPECT_TRUE(secondary_capture.call_count == 0U);

    ulog_t_info("net", "link %s", "up");
    EXPECT_TRUE(primary_capture.call_count == 1U);
    EXPECT_TRUE(secondary_capture.call_count == 0U);
    EXPECT_TRUE(primary_capture.topic == topic_id);
    EXPECT_STREQ(primary_capture.message, "link up");
    EXPECT_CONTAINS(primary_capture.rendered, "[net]");

    EXPECT_STATUS(ulog_topic_level_set("net", ULOG_LEVEL_ERROR),
                  ULOG_STATUS_OK);
    ulog_t_warn("net", "still filtered");
    EXPECT_TRUE(primary_capture.call_count == 1U);

    EXPECT_STATUS(ulog_topic_remove("net"), ULOG_STATUS_OK);
    EXPECT_TRUE(ulog_topic_get_id("net") == ULOG_TOPIC_ID_INVALID);
    ulog_t_error("net", "dropped");
    EXPECT_TRUE(primary_capture.call_count == 1U);
    return true;
}

static bool test_dynamic_configuration_and_cleanup() {
    EXPECT_TRUE(reset_ulog());

    auto capture = (capture_t){0};
    auto output  = capture_output_add(&capture, ULOG_LEVEL_TRACE);
    EXPECT_TRUE(output != ULOG_OUTPUT_INVALID);

    EXPECT_STATUS(ulog_prefix_set_fn(fixed_prefix), ULOG_STATUS_OK);
    ulog_info("alpha");
    EXPECT_CONTAINS(capture.rendered, "[PFX]");
    EXPECT_CONTAINS(capture.rendered, "tests.c:");
    EXPECT_CONTAINS(capture.rendered, "INFO ");

    capture_reset(&capture);
    EXPECT_STATUS(ulog_prefix_config(false), ULOG_STATUS_OK);
    EXPECT_STATUS(ulog_source_location_config(false), ULOG_STATUS_OK);
    EXPECT_STATUS(ulog_level_config(ULOG_LEVEL_CONFIG_STYLE_SHORT),
                  ULOG_STATUS_OK);
    ulog_info("beta");
    EXPECT_TRUE(capture.call_count == 1U);
    EXPECT_CONTAINS(capture.rendered, "I ");
    EXPECT_TRUE(strstr(capture.rendered, "[PFX]") == nullptr);
    EXPECT_TRUE(strstr(capture.rendered, "tests.c:") == nullptr);
    EXPECT_TRUE(strstr(capture.rendered, "INFO ") == nullptr);

    EXPECT_STATUS(ulog_cleanup(), ULOG_STATUS_OK);
    EXPECT_STATUS(ulog_output_level_set(ULOG_OUTPUT_STDOUT, ULOG_LEVEL_FATAL),
                  ULOG_STATUS_OK);

    capture_reset(&capture);
    output = capture_output_add(&capture, ULOG_LEVEL_TRACE);
    EXPECT_TRUE(output != ULOG_OUTPUT_INVALID);

    ulog_info("gamma");
    EXPECT_TRUE(capture.call_count == 1U);
    EXPECT_CONTAINS(capture.rendered, "INFO ");
    EXPECT_CONTAINS(capture.rendered, "tests.c:");
    EXPECT_TRUE(strstr(capture.rendered, "[PFX]") == nullptr);
    return true;
}

typedef bool (*test_fn)();

int main() {
    static const struct {
        const char *name;
        test_fn fn;
    } tests[] = {
        {"level configuration", test_level_configuration},
        {"output filtering and accessors",
         test_output_filtering_and_event_accessors},
        {"topic routing", test_topics_route_to_configured_output},
        {"dynamic configuration and cleanup",
         test_dynamic_configuration_and_cleanup},
    };

    auto passed = (size_t)0;

    for (auto i = 0U; i < (sizeof(tests) / sizeof(tests[0])); i++) {
        if (!tests[i].fn()) {
            fprintf(stderr, "FAILED: %s\n", tests[i].name);
            return EXIT_FAILURE;
        }

        passed += 1;
    }

    printf("ok: %zu tests passed\n", passed);
    return EXIT_SUCCESS;
}
