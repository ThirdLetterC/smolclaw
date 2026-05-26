// *************************************************************************
//
// ulog v@ULOG_VERSION@ - A simple customizable logging library.
// https://github.com/an-dr/microlog
//
// *************************************************************************
//
// Original implementation by rxi: https://github.com/rxi
// Modified by Andrei Gramakov: https://agramakov.me, mail@agramakov.me
// Also modified by many beautiful contributors from GitHub
//
// Copyright (c) 2025 Andrei Gramakov. All rights reserved.
//
// This file is licensed under the terms of the MIT license.
// For a copy, see: https://opensource.org/licenses/MIT
//
// *************************************************************************

#include "ulog/ulog.h"
#include <stdlib.h>
#include <string.h>

// clang-format off
/* ====================================================================================================================
   Core Feature: Static Configuration
=======================================================================================================================

| Build Option                     | Default                    | Dependent Macro(s)        | Purpose                  |
| -------------------------------- | -------------------------- | ------------------------- | ------------------------ |
| ULOG_BUILD_COLOR                 | 0                          | ULOG_HAS_COLOR            | Compile color code paths |
| ULOG_BUILD_PREFIX_SIZE           | 0                          | ULOG_HAS_PREFIX           | Prefix buffer logic      |
| ULOG_BUILD_EXTRA_OUTPUTS         | 0                          | ULOG_HAS_EXTRA_OUTPUTS    | Extra output backends    |
| ULOG_BUILD_SOURCE_LOCATION       | 1                          | ULOG_HAS_SOURCE_LOCATION  | File\:line output        |
| ULOG_BUILD_LEVEL_SHORT           | 0                          | ULOG_LEVEL_HAS_SHORT/_LONG| Short level style        |
| ULOG_BUILD_TIME                  | 0                          | ULOG_HAS_TIME             | Timestamp support        |
| ULOG_BUILD_TOPICS_MODE           | ULOG_BUILD_TOPICS_MODE_OFF | ULOG_HAS_TOPICS           | Topics mode              |
| ULOG_BUILD_TOPICS_STATIC_NUM     | 0                          | -                         | Topic number             |
| ULOG_BUILD_DYNAMIC_CONFIG        | 0                          | ULOG_HAS_DYNAMIC_CONFIG   | Runtime toggles          |
| ULOG_BUILD_WARN_NOT_ENABLED      | 1                          | ULOG_HAS_WARN_NOT_ENABLED | Warning stubs            |
| ULOG_BUILD_CONFIG_HEADER_ENABLED | 0                          | -                         | Configuration header mode|
| ULOG_BUILD_CONFIG_HEADER_NAME    | "ulog_config.h"            | -                         | Configuration header name|
| ULOG_BUILD_DISABLED              | 0                          | -                         | Disable ulog completely  |

===================================================================================================================== */

/* ============================================================================
   Optional Feature: Disable
============================================================================ */

#if ULOG_BUILD_DISABLED == 0

/* ============================================================================
   Optional Feature: Configuration Header
============================================================================ */

#ifdef ULOG_BUILD_CONFIG_HEADER_ENABLED

    // If ULOG_BUILD_CONFIG_HEADER_ENABLED is defined, no other ULOG_BUILD_* macros should be defined to avoid conflicts
    #ifdef ULOG_BUILD_COLOR
        #error "ULOG_BUILD_CONFIG_HEADER_ENABLED cannot be used with ULOG_BUILD_COLOR"
    #endif
    #ifdef ULOG_BUILD_PREFIX_SIZE
        #error "ULOG_BUILD_CONFIG_HEADER_ENABLED cannot be used with ULOG_BUILD_PREFIX_SIZE"
    #endif
    #ifdef ULOG_BUILD_EXTRA_OUTPUTS
        #error "ULOG_BUILD_CONFIG_HEADER_ENABLED cannot be used with ULOG_BUILD_EXTRA_OUTPUTS"
    #endif
    #ifdef ULOG_BUILD_SOURCE_LOCATION
        #error "ULOG_BUILD_CONFIG_HEADER_ENABLED cannot be used with ULOG_BUILD_SOURCE_LOCATION"
    #endif
    #ifdef ULOG_BUILD_LEVEL_SHORT
        #error "ULOG_BUILD_CONFIG_HEADER_ENABLED cannot be used with ULOG_BUILD_LEVEL_SHORT"
    #endif
    #ifdef ULOG_BUILD_TIME
        #error "ULOG_BUILD_CONFIG_HEADER_ENABLED cannot be used with ULOG_BUILD_TIME"
    #endif
    #ifdef ULOG_BUILD_TOPICS_MODE
        #error "ULOG_BUILD_CONFIG_HEADER_ENABLED cannot be used with ULOG_BUILD_TOPICS_MODE"
    #endif
    #ifdef ULOG_BUILD_DYNAMIC_CONFIG
        #error "ULOG_BUILD_CONFIG_HEADER_ENABLED cannot be used with ULOG_BUILD_DYNAMIC_CONFIG"
    #endif
    #ifdef ULOG_BUILD_WARN_NOT_ENABLED
        #error "ULOG_BUILD_CONFIG_HEADER_ENABLED cannot be used with ULOG_BUILD_WARN_NOT_ENABLED"
    #endif

    // The user provided configuration header
    #ifndef ULOG_BUILD_CONFIG_HEADER_NAME
        #define ULOG_BUILD_CONFIG_HEADER_NAME "ulog_config.h"
    #endif

    #include ULOG_BUILD_CONFIG_HEADER_NAME
#endif // ULOG_BUILD_CONFIG_HEADER_ENABLED

/* ============================================================================
    Core Feature: Static Configuration
============================================================================ */

#ifndef ULOG_BUILD_COLOR
    #define ULOG_HAS_COLOR 0
#else
    #define ULOG_HAS_COLOR (ULOG_BUILD_COLOR==1)
#endif


#ifndef ULOG_BUILD_PREFIX_SIZE
    #define ULOG_HAS_PREFIX 0
#else
    #define ULOG_HAS_PREFIX (ULOG_BUILD_PREFIX_SIZE > 0)
#endif


#ifndef ULOG_BUILD_TIME
    #define ULOG_HAS_TIME 0
#else
    #define ULOG_HAS_TIME (ULOG_BUILD_TIME==1)
#endif


#ifndef ULOG_BUILD_LEVEL_SHORT
    #define ULOG_HAS_LEVEL_LONG  1
    #define ULOG_HAS_LEVEL_SHORT 0
#else
    #define ULOG_HAS_LEVEL_LONG  !ULOG_BUILD_LEVEL_SHORT
    #define ULOG_HAS_LEVEL_SHORT ULOG_BUILD_LEVEL_SHORT
#endif


#ifndef ULOG_BUILD_EXTRA_OUTPUTS
    #define ULOG_HAS_EXTRA_OUTPUTS 0
#else
    #define ULOG_HAS_EXTRA_OUTPUTS (ULOG_BUILD_EXTRA_OUTPUTS > 0)
#endif


#ifndef ULOG_BUILD_SOURCE_LOCATION
    #define ULOG_HAS_SOURCE_LOCATION 1
#else
    #define ULOG_HAS_SOURCE_LOCATION (ULOG_BUILD_SOURCE_LOCATION == 1)
#endif


#ifndef ULOG_BUILD_WARN_NOT_ENABLED
    #define ULOG_HAS_WARN_NOT_ENABLED 1
#else
    #define ULOG_HAS_WARN_NOT_ENABLED (ULOG_BUILD_WARN_NOT_ENABLED==1)
#endif

#ifndef ULOG_BUILD_TOPICS_MODE
    #define ULOG_HAS_TOPICS 0
#else
    /* Topics enabled when mode is not OFF (STATIC or DYNAMIC) */
    #define ULOG_HAS_TOPICS (ULOG_BUILD_TOPICS_MODE != ULOG_BUILD_TOPICS_MODE_OFF)
#endif

/* ============================================================================
   Optional Feature: Dynamic Configuration
============================================================================ */

#ifndef ULOG_BUILD_DYNAMIC_CONFIG
    #define ULOG_HAS_DYNAMIC_CONFIG 0
#else
    #define ULOG_HAS_DYNAMIC_CONFIG 1

    // Undef macros to avoid conflicts
    #undef ULOG_BUILD_EXTRA_OUTPUTS
    #undef ULOG_BUILD_PREFIX_SIZE
    #undef ULOG_BUILD_TOPICS_MODE
    #undef ULOG_HAS_COLOR
    #undef ULOG_HAS_EXTRA_OUTPUTS
    #undef ULOG_HAS_LEVEL_LONG
    #undef ULOG_HAS_LEVEL_SHORT
    #undef ULOG_HAS_PREFIX
    #undef ULOG_HAS_SOURCE_LOCATION
    #undef ULOG_HAS_TIME
    #undef ULOG_HAS_TOPICS
    #undef ULOG_HAS_WARN_NOT_ENABLED

    // Configure features based on runtime config
    #define ULOG_BUILD_EXTRA_OUTPUTS 8
    #define ULOG_BUILD_PREFIX_SIZE 64
    /* In dynamic configuration mode we enable dynamic topics */
    #define ULOG_BUILD_TOPICS_MODE ULOG_BUILD_TOPICS_MODE_DYNAMIC
    #define ULOG_HAS_COLOR 1
    #define ULOG_HAS_EXTRA_OUTPUTS 1
    #define ULOG_HAS_LEVEL_LONG 1
    #define ULOG_HAS_LEVEL_SHORT 1
    #define ULOG_HAS_PREFIX 1
    #define ULOG_HAS_SOURCE_LOCATION 1
    #define ULOG_HAS_TIME 1
    #define ULOG_HAS_TOPICS 1
    #define ULOG_HAS_WARN_NOT_ENABLED 0
#endif

// clang-format on

#if ULOG_HAS_TIME
#include <time.h>
#endif

/* ============================================================================
   Tools
============================================================================ */

// If testing is enabled, we define NOT_SO_STATIC as empty to allow
// the functions to be visible outside of this file for testing purposes.
#ifdef ULOG_TESTING
#define NOT_VERY_STATIC
#else
#define NOT_VERY_STATIC static
#endif

// Check if the string is empty or not provided
static inline bool is_str_empty(const char *str) {
    return (str == nullptr) || (str[0] == '\0');
}

/* ============================================================================
   Core Feature: Warn Not Enabled
   (`warn_not_enabled`, depends on: - )
============================================================================ */
#if ULOG_HAS_WARN_NOT_ENABLED

// Macro to log a warning when a feature is not enabled
// Usage: warn_not_enabled("ULOG_BUILD_TIME")
// Output:
//   WARN src/main.c:42: 'ulog_configure_time' ignored: ULOG_BUILD_TIME disabled
#define warn_not_enabled(feature)                                              \
    warn_non_enabled_full(__func__, feature, __FILE__, __LINE__)

#define warn_non_enabled_full(func, feature, file, line)                       \
    ulog_log(ULOG_LEVEL_WARN, file, line, nullptr,                             \
             "'%s' called with %s disabled", func, feature)

#endif  // ULOG_HAS_WARN_NOT_ENABLED
/* ============================================================================
   Core Feature: Print
   (`print_*`, depends on: - )
============================================================================ */

//  Private
// ================

typedef struct {
    char *data;
    size_t curr_pos;
    size_t size;
} print_buffer;

typedef union {
    print_buffer buffer;
    FILE *stream;
} print_target_descriptor;

typedef enum { PRINT_TARGET_BUFFER, PRINT_TARGET_STREAM } print_target_type;

typedef struct {
    print_target_type type;
    print_target_descriptor dsc;
} print_target;

static void print_to_target_valist(print_target *tgt, const char *format,
                                   va_list args) {
    if (tgt->type == PRINT_TARGET_BUFFER) {
        auto buf = &tgt->dsc.buffer;

        if (buf->curr_pos >= buf->size) {
            return;  // No space available
        }

        auto remaining = buf->size - buf->curr_pos;
        auto write_pos = buf->data + buf->curr_pos;

        auto written = vsnprintf(write_pos, remaining, format, args);
        if (written < 0) {
            return;  // Encoding error
        }

        // Update position, capping at buffer end
        if ((size_t)written >= remaining) {
            buf->curr_pos = buf->size;
        } else {
            buf->curr_pos += written;
        }

    } else if (tgt->type == PRINT_TARGET_STREAM) {
        vfprintf(tgt->dsc.stream, format, args);
    }
}

static void print_to_target(print_target *tgt, const char *format, ...) {
    va_list args;
    va_start(args, format);
    print_to_target_valist(tgt, format, args);
    va_end(args);
}

/* ============================================================================
   Core Feature: Events
   (`event_*`, depends on: Print)
============================================================================ */

// Private
// ================

static void log_print_message(print_target *tgt, ulog_event *ev);

/// @brief Event structure
struct ulog_event {
    const char *message;          // Message format string
    va_list message_format_args;  // Format arguments

#if ULOG_HAS_TOPICS
    ulog_topic_id topic;
#endif

#if ULOG_HAS_TIME
    struct tm time_snapshot;
    bool has_time_snapshot;
#endif

#if ULOG_HAS_SOURCE_LOCATION
    const char *file;  // Event file name
    int line;          // Event line number
#endif                 // ULOG_HAS_SOURCE_LOCATION

    ulog_level level;  // Event debug level
};

ulog_status ulog_event_get_message(ulog_event *ev, char *buffer,
                                   size_t buffer_size) {
    if (ev == nullptr || buffer == nullptr || buffer_size == 0) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }

    auto tgt = (print_target){.type       = PRINT_TARGET_BUFFER,
                              .dsc.buffer = {buffer, 0, buffer_size}};

    // Create a copy of the event to avoid va_list issues
    auto ev_copy = *ev;
    va_copy(ev_copy.message_format_args, ev->message_format_args);

    if (!is_str_empty(ev_copy.message)) {
        print_to_target_valist(&tgt, ev_copy.message,
                               ev_copy.message_format_args);
    } else {
        print_to_target(&tgt, "nullptr");
    }

    va_end(ev_copy.message_format_args);
    return ULOG_STATUS_OK;
}

#if ULOG_HAS_TOPICS
ulog_topic_id ulog_event_get_topic(ulog_event *ev) {
    if (ev == nullptr) {
        return ULOG_TOPIC_ID_INVALID;  // Invalid topic
    }
    return ev->topic;
}
#endif  // ULOG_HAS_TOPICS

#if ULOG_HAS_TIME
struct tm *ulog_event_get_time(ulog_event *ev) {
    if (ev == nullptr || !ev->has_time_snapshot) {
        return nullptr;
    }
    return &ev->time_snapshot;
}
#endif  // ULOG_HAS_TIME

#if ULOG_HAS_SOURCE_LOCATION
const char *ulog_event_get_file(ulog_event *ev) {
    if (ev == nullptr) {
        return nullptr;
    }
    return ev->file;
}

int ulog_event_get_line(ulog_event *ev) {
    if (ev == nullptr) {
        return -1;
    }
    return ev->line;
}
#endif  // ULOG_HAS_SOURCE_LOCATION

ulog_level ulog_event_get_level(ulog_event *ev) {
    if (ev == nullptr) {
        return ULOG_LEVEL_TRACE;  // Invalid level
    }
    return ev->level;
}

/* ============================================================================
   Core Functionality: Lock
   (`lock_*`, depends on: - )
============================================================================ */

// Private
// ================
typedef struct {
    ulog_lock_fn function;  // Lock function
    void *args;             // Argument for the lock function
} lock_data_t;

static lock_data_t lock_data = {
    .function = nullptr,  // No lock function by default
    .args     = nullptr,  // No lock argument by default
};

static ulog_status lock_lock() {
    if (lock_data.function != nullptr) {
        return lock_data.function(true, lock_data.args);
    }
    return ULOG_STATUS_OK;
}

static ulog_status lock_unlock() {
    if (lock_data.function != nullptr) {
        return lock_data.function(false, lock_data.args);
    }
    return ULOG_STATUS_OK;
}

static ulog_status unlock_with_status(ulog_status status) {
    auto unlock_status = lock_unlock();
    if (unlock_status != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;
    }
    return status;
}

// Public
// ================

/// @brief  Sets the lock function and user data
ulog_status ulog_lock_set_fn(ulog_lock_fn function, void *lock_arg) {
    if (function == nullptr) {
        lock_data.function = nullptr;
        lock_data.args     = nullptr;
        return ULOG_STATUS_OK;
    }
    lock_data.function = function;
    lock_data.args     = lock_arg;
    return ULOG_STATUS_OK;
}
/* ============================================================================
   Optional Feature: Dynamic Configuration - Color
   (`color_config_*`, depends on: - )
============================================================================ */
#if ULOG_HAS_DYNAMIC_CONFIG

typedef struct {
    bool enabled;
} color_config;

static color_config color_cfg = {
    .enabled = (bool)ULOG_HAS_COLOR,
};

// Private
// ================

bool color_config_is_enabled() {
    return color_cfg.enabled;
}

// Public
// ================

ulog_status ulog_color_config(bool enabled) {
    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;  // Failed to acquire lock
    }
    color_cfg.enabled = enabled;
    return lock_unlock();
}

#else  // ULOG_HAS_DYNAMIC_CONFIG

// Disabled Public
// ================

#if ULOG_HAS_WARN_NOT_ENABLED
ulog_status ulog_color_config(bool enabled) {
    (void)(enabled);
    warn_not_enabled("ULOG_BUILD_COLOR");
    return ULOG_STATUS_DISABLED;
}
#endif  // ULOG_HAS_WARN_NOT_ENABLED

// Disabled Private
// ================

#define color_config_is_enabled() (ULOG_HAS_COLOR)

#endif  // ULOG_HAS_DYNAMIC_CONFIG

/* ============================================================================
   Optional Feature: Color
   (`color_*`, depends on: Print, Color Config)
============================================================================ */
#if ULOG_HAS_COLOR

//  Private
// ================

// From https://en.wikipedia.org/wiki/ANSI_escape_code#8-bit
// ANSI color codes from least to most attention-grabbing
static constexpr const char color_levels[][11] = {
    "\x1b[m",            // LEVEL_0: Color reset (default)
    "\x1b[36m",          // LEVEL_1: Color (Cyan)
    "\x1b[32m",          // LEVEL_2: Color (Green)
    "\x1b[33m",          // LEVEL_3: Color (Yellow)
    "\x1b[31m",          // LEVEL_4: Color (Red)
    "\x1b[31m\x1b[47m",  // LEVEL_5: Color (Red on White)
    "\x1b[43m\x1b[31m",  // LEVEL_6: Color (Yellow on Red)
    "\x1b[41m\x1b[97m",  // LEVEL_7: Color (White on Red)
};

static constexpr const char color_terminator[] = "\x1b[0m";

static void color_print_start(print_target *tgt, ulog_event *ev) {
    if (!color_config_is_enabled()) {
        return;  // Color is disabled, do not print color codes
    }
    print_to_target(tgt, "%s", color_levels[ev->level]);  // color start
}

static void color_print_end(print_target *tgt) {
    if (!color_config_is_enabled()) {
        return;  // Color is disabled, do not print color codes
    }
    print_to_target(tgt, "%s", color_terminator);  // color end
}

#else  // ULOG_HAS_COLOR

// Disabled Private
// ================

#define color_print_start(tgt, ev) (void)(tgt), (void)(ev)
#define color_print_end(tgt) (void)(tgt)

#endif  // ULOG_HAS_COLOR

/* ============================================================================
   Optional Feature: Dynamic Configuration - Prefix
   (`prefix_config_*`, depends on: - )
============================================================================ */
#if ULOG_HAS_DYNAMIC_CONFIG

typedef struct {
    bool enabled;
} prefix_config;

static prefix_config prefix_cfg = {
    .enabled = (bool)ULOG_HAS_PREFIX,
};

// Private
// ================

bool prefix_config_is_enabled() {
    return prefix_cfg.enabled;
}

// Public
// ================

ulog_status ulog_prefix_config(bool enabled) {
    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;
    }
    prefix_cfg.enabled = enabled;
    return lock_unlock();
}

#else  // ULOG_HAS_DYNAMIC_CONFIG

// Disabled Public
// ================

#if ULOG_HAS_WARN_NOT_ENABLED
ulog_status ulog_prefix_config(bool enabled) {
    (void)(enabled);
    warn_not_enabled("ULOG_BUILD_DYNAMIC_CONFIG");
    return ULOG_STATUS_DISABLED;
}
#endif  // ULOG_HAS_WARN_NOT_ENABLED

// Disabled Private
// ================

#define prefix_config_is_enabled() (ULOG_HAS_PREFIX)
#endif  // ULOG_HAS_DYNAMIC_CONFIG

/* ============================================================================
   Optional Feature: Prefix
   (`prefix_*`, depends on: Print, Prefix Config)
============================================================================ */
#if ULOG_HAS_PREFIX

// Private
// ================
typedef struct {
    ulog_prefix_fn function;
    char prefix[ULOG_BUILD_PREFIX_SIZE];
} prefix_data_t;

static prefix_data_t prefix_data = {
    .function = nullptr,
    .prefix   = {0},
};

static void prefix_update(ulog_event *ev) {
    if (prefix_data.function == nullptr || !prefix_config_is_enabled()) {
        return;
    }
    prefix_data.function(ev, prefix_data.prefix, ULOG_BUILD_PREFIX_SIZE);
}

static void prefix_print(print_target *tgt) {
    if (prefix_data.function == nullptr || !prefix_config_is_enabled()) {
        return;
    }
    print_to_target(tgt, "%s", prefix_data.prefix);
}

// Public
// ================

ulog_status ulog_prefix_set_fn(ulog_prefix_fn function) {
    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;
    }
    prefix_data.function = function;
    if (function == nullptr) {
        memset(prefix_data.prefix, 0, sizeof(prefix_data.prefix));
    }
    return unlock_with_status(ULOG_STATUS_OK);
}

#else  // ULOG_HAS_PREFIX

// Disabled Public
// ================

#if ULOG_HAS_WARN_NOT_ENABLED
ulog_status ulog_prefix_set_fn(ulog_prefix_fn function) {
    (void)(function);
    warn_not_enabled("ULOG_BUILD_PREFIX_SIZE");
    return ULOG_STATUS_DISABLED;
}
#endif  // ULOG_HAS_WARN_NOT_ENABLED

// Disabled Private
// ================

#define prefix_print(tgt) (void)(tgt)
#define prefix_update(ev) (void)(ev)
#endif  // ULOG_HAS_PREFIX

/* ============================================================================
   Optional Feature: Dynamic Configuration - Time
   (`time_config_*`, depends on: - )
============================================================================ */
#if ULOG_HAS_DYNAMIC_CONFIG

typedef struct {
    bool enabled;
} time_config;

static time_config time_cfg = {
    .enabled = ULOG_HAS_TIME,
};

// Private
// ================

bool time_config_is_enabled() {
    return time_cfg.enabled;
}

// Public
// ================

ulog_status ulog_time_config(bool enabled) {
    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;
    }
    time_cfg.enabled = enabled;
    return lock_unlock();
}

#else  // ULOG_HAS_DYNAMIC_CONFIG

// Disabled Public
// ================

#if ULOG_HAS_WARN_NOT_ENABLED
ulog_status ulog_time_config(bool enabled) {
    (void)(enabled);
    warn_not_enabled("ULOG_BUILD_TIME");
    return ULOG_STATUS_DISABLED;
}
#endif  // ULOG_HAS_WARN_NOT_ENABLED

// Disabled Private
// ================

#define time_config_is_enabled() (ULOG_HAS_TIME)
#endif  // ULOG_HAS_DYNAMIC_CONFIG

/* ============================================================================
   Optional Feature: Time
   (`time_*`, depends on: Time, Print)
============================================================================ */
#if ULOG_HAS_TIME

static constexpr size_t time_short_buf_size =
    10U;  // HH:MM:SS(8) + 1 space + null
static constexpr size_t time_full_buf_size =
    21U;  // YYYY-MM-DD HH:MM:SS(19) + 1 space + null

// Private
// ================
static bool time_print_if_invalid(print_target *tgt, ulog_event *ev) {
    if (!ev->has_time_snapshot) {
        print_to_target(tgt, "INVALID_TIME");
        return true;  // Time is invalid, print error message
    }
    return false;  // Time is valid
}

static bool time_fill_local_snapshot(time_t current_time, struct tm *out) {
#if defined(_WIN32)
    return localtime_s(out, &current_time) == 0;
#elif defined(__unix__) || defined(__APPLE__) || defined(__MVS__)
    return localtime_r(&current_time, out) != nullptr;
#else
    auto tmp = localtime(&current_time);
    if (tmp == nullptr) {
        return false;
    }

    *out = *tmp;
    return true;
#endif
}

/// @brief Fills the event time with the current local time
/// @param ev - Event to fill. Assumed not nullptr
static void time_fill_current_time(ulog_event *ev) {
    auto current_time = time(nullptr);  // Get current time
    if (current_time == (time_t)-1) {
        ev->has_time_snapshot = false;
        return;
    }

    ev->has_time_snapshot =
        time_fill_local_snapshot(current_time, &ev->time_snapshot);
}

static void time_print_short(print_target *tgt, ulog_event *ev,
                             bool append_space) {
    if (!time_config_is_enabled() || time_print_if_invalid(tgt, ev)) {
        return;  // If time is not valid or disabled, stop printing
    }
    char buf[time_short_buf_size] = {0};
    auto format                   = append_space ? "%H:%M:%S " : "%H:%M:%S";
    strftime(buf, time_short_buf_size, format, &ev->time_snapshot);
    print_to_target(tgt, "%s", buf);
}

#if ULOG_HAS_EXTRA_OUTPUTS
static void time_print_full(print_target *tgt, ulog_event *ev,
                            bool append_space) {
    if (!time_config_is_enabled() || time_print_if_invalid(tgt, ev)) {
        return;  // If time is not valid or disabled, stop printing
    }
    char buf[time_full_buf_size] = {0};
    auto format = append_space ? "%Y-%m-%d %H:%M:%S " : "%Y-%m-%d %H:%M:%S";
    strftime(buf, time_full_buf_size, format, &ev->time_snapshot);
    print_to_target(tgt, "%s", buf);
}
#else
#define time_print_full(tgt, ev, append_space) (void)(0)
#endif  // ULOG_HAS_EXTRA_OUTPUTS

#else  // ULOG_HAS_TIME

// Disabled Private
// ================

#define time_print_short(tgt, ev, append_space) (void)(0)
#define time_print_full(tgt, ev, append_space) (void)(0)
#define time_fill_current_time(ev) (void)(ev)
#endif  // ULOG_HAS_TIME

/* ============================================================================
   Core Feature: Levels
   (`level_*`, depends on: Levels Config, Print)
============================================================================ */

// Private
// ================
static constexpr ulog_level level_min_value = ULOG_LEVEL_0;

static const char *const level_names_default_long[ULOG_LEVEL_TOTAL] = {
    "TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL", nullptr, nullptr,
};

[[maybe_unused]] static const char
    *const level_names_default_short[ULOG_LEVEL_TOTAL] = {
        "T", "D", "I", "W", "E", "F", nullptr, nullptr,
};

static constexpr size_t level_name_storage_size = 64U;

typedef struct {
    ulog_level max_level;
    const char *names[ULOG_LEVEL_TOTAL];
    char custom_names[ULOG_LEVEL_TOTAL][level_name_storage_size];
} level_data_t;

static level_data_t level_data = {
#if ULOG_HAS_LEVEL_SHORT && !ULOG_HAS_LEVEL_LONG
    .max_level = ULOG_LEVEL_FATAL,
    .names     = {"T", "D", "I", "W", "E", "F", nullptr, nullptr},
#else
    .max_level = ULOG_LEVEL_FATAL,
    .names     = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR", "FATAL", nullptr,
                  nullptr},
#endif
    .custom_names = {{0}},
};

static void level_apply_defaults(const char *const default_names[],
                                 ulog_level max_level) {
    level_data.max_level = max_level;

    for (auto i = 0; i < ULOG_LEVEL_TOTAL; i++) {
        level_data.names[i] =
            (i <= (int)max_level) ? default_names[i] : nullptr;
        memset(level_data.custom_names[i], 0, level_name_storage_size);
    }
}

static bool level_is_allowed(ulog_level msg_level, ulog_level log_verbosity) {
    if (msg_level < log_verbosity || msg_level < level_min_value) {
        return false;  // Level is higher than the configured level, not allowed
    }

    return true;  // Level is allowed
}

static bool level_is_valid(ulog_level level) {
    return (level >= level_min_value && level <= level_data.max_level);
}

static bool level_descriptor_is_valid(const ulog_level_descriptor *dsc) {
    if (dsc == nullptr || dsc->max_level < level_min_value ||
        dsc->max_level >= ULOG_LEVEL_TOTAL) {
        return false;
    }

    for (auto i = (int)level_min_value; i <= (int)dsc->max_level; i++) {
        if (is_str_empty(dsc->names[i]) ||
            strlen(dsc->names[i]) >= level_name_storage_size) {
            return false;
        }
    }

    return true;
}

static void level_print(print_target *tgt, ulog_event *ev) {
    if (!level_is_valid(ev->level)) {
        print_to_target(tgt, "? ");
        return;
    }
    print_to_target(tgt, "%s ", level_data.names[ev->level]);
}

// Public
// ================

/// @brief Returns the string representation of the level
const char *ulog_level_to_string(ulog_level level) {
    if (level < level_min_value || level > level_data.max_level) {
        return "?";  // Return a default string for invalid levels
    }

    return level_data.names[level];
}

ulog_status ulog_level_set_new_levels(const ulog_level_descriptor *new_levels) {
    if (!level_descriptor_is_valid(new_levels)) {
        return ULOG_STATUS_INVALID_ARGUMENT;  // Invalid argument
    }
    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;  // Failed to acquire lock
    }

    level_data.max_level = new_levels->max_level;

    for (auto i = 0; i < ULOG_LEVEL_TOTAL; i++) {
        memset(level_data.custom_names[i], 0, level_name_storage_size);
        level_data.names[i] = nullptr;
    }

    for (auto i = (int)level_min_value; i <= (int)new_levels->max_level; i++) {
        memcpy(level_data.custom_names[i], new_levels->names[i],
               strlen(new_levels->names[i]) + 1);
        level_data.names[i] = level_data.custom_names[i];
    }

    return unlock_with_status(ULOG_STATUS_OK);
}

ulog_status ulog_level_reset_levels() {
    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;  // Failed to acquire lock
    }

    level_apply_defaults(
#if ULOG_HAS_LEVEL_SHORT && !ULOG_HAS_LEVEL_LONG
        level_names_default_short,
#else
        level_names_default_long,
#endif
        ULOG_LEVEL_FATAL);
    return unlock_with_status(ULOG_STATUS_OK);
}

/* ============================================================================
   Optional Feature: Dynamic Configuration - Level
   (`level_config_*`, depends on: - )
============================================================================ */
#if ULOG_HAS_DYNAMIC_CONFIG

// Private
// ================

typedef struct {
    bool short_style;  // Use short level strings
} level_config;

static level_config level_cfg = {
    .short_style = false,
};

bool level_config_is_short() {
    return level_cfg.short_style;
}

// Public
// ================

ulog_status ulog_level_config(ulog_level_config_style style) {
    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;
    }
    level_cfg.short_style = (style == ULOG_LEVEL_CONFIG_STYLE_SHORT);
    level_apply_defaults(level_cfg.short_style ? level_names_default_short
                                               : level_names_default_long,
                         ULOG_LEVEL_FATAL);
    return unlock_with_status(ULOG_STATUS_OK);
}

#else  // ULOG_HAS_DYNAMIC_CONFIG

// Disabled Public
// ================

#if ULOG_HAS_WARN_NOT_ENABLED

ulog_status ulog_level_config(ulog_level_config_style style) {
    (void)(style);
    warn_not_enabled("ULOG_BUILD_DYNAMIC_CONFIG");
    return ULOG_STATUS_DISABLED;
}

#endif  // ULOG_HAS_WARN_NOT_ENABLED

// Disabled Private
// ================

#define level_config_is_short() (ULOG_HAS_LEVEL_SHORT)
#endif  // ULOG_HAS_DYNAMIC_CONFIG

/* ============================================================================
   Core Feature: Outputs
   (`output_*`, depends on: Print, Log, Level)
============================================================================ */

//  Private
// ================
#if ULOG_HAS_EXTRA_OUTPUTS
static constexpr int output_total_num =
    (1 + ULOG_BUILD_EXTRA_OUTPUTS);  // stdout + extra
#else
static constexpr int output_total_num = 1;  // Only stdout
#endif  // ULOG_HAS_EXTRA_OUTPUTS

static constexpr ulog_level output_stdout_default_level = ULOG_LEVEL_TRACE;

// Prototypes
static void output_stdout_handler(ulog_event *ev, void *arg);
static void log_print_event(print_target *tgt, ulog_event *ev, bool full_time,
                            bool color, bool new_line);

typedef struct {
    ulog_output_handler_fn handler;
    void *arg;
    ulog_level level;
} output_slot;

typedef struct {
    output_slot outputs[output_total_num];  // order num = id. 0 is for stdout
} output_data_t;

static output_data_t output_data = {
    .outputs = {{output_stdout_handler, nullptr, output_stdout_default_level}}};

static bool output_id_is_configurable(ulog_output_id output) {
    return output >= ULOG_OUTPUT_STDOUT && output < output_total_num;
}

[[maybe_unused]] static bool output_id_is_routable(ulog_output_id output) {
    if (output == ULOG_OUTPUT_ALL) {
        return true;
    }

    if (!output_id_is_configurable(output)) {
        return false;
    }

    return output_data.outputs[output].handler != nullptr;
}

static void output_handle_single(ulog_event *ev, output_slot *output) {
    if (output->handler == nullptr) {
        return;  // Output has been removed, skip it
    }

    if (level_is_allowed(ev->level, output->level)) {

        // Create event copy to avoid va_list issues
        auto ev_copy = (ulog_event){0};
        memcpy(&ev_copy, ev, sizeof(ulog_event));

        // Initialize the va_list for the copied event
        // Note: We use a copy of the va_list to avoid issues with passing it
        // directly as on some platforms using the same va_list multiple times
        // can lead to undefined behavior.
        va_copy(ev_copy.message_format_args, ev->message_format_args);
        output->handler(&ev_copy, output->arg);
        va_end(ev_copy.message_format_args);
    }
}

static void output_handle_by_id(ulog_event *ev, ulog_output_id output_id) {
    // Validate output ID bounds
    if (output_id < 0 || output_id >= output_total_num) {
        return;  // Invalid output ID
    }
    output_handle_single(ev, &output_data.outputs[output_id]);
}

static void output_handle_all(ulog_event *ev) {
    // Processing the message for outputs
    for (auto i = 0; (i < output_total_num); i++) {
        output_handle_single(ev, &output_data.outputs[i]);
    }
}

static void output_stdout_handler(ulog_event *ev, [[maybe_unused]] void *arg) {
    auto tgt =
        (print_target){.type = PRINT_TARGET_STREAM, .dsc.stream = stdout};
    log_print_event(&tgt, ev, false, true, true);
}

// Public
// ================

ulog_status ulog_output_level_set(ulog_output_id output, ulog_level level) {
    if (!level_is_valid(level)) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }
    if (!output_id_is_configurable(output)) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }

    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;
    }

    if (output_data.outputs[output].handler == nullptr) {
        return unlock_with_status(
            ULOG_STATUS_NOT_FOUND);  // Output exists but no handler assigned
    }
    output_data.outputs[output].level = level;
    return unlock_with_status(ULOG_STATUS_OK);
}

ulog_status ulog_output_level_set_all(ulog_level level) {
    if (!level_is_valid(level)) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }

    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;
    }

    for (auto i = 0; i < output_total_num; i++) {
        output_data.outputs[i].level = level;
    }
    return unlock_with_status(ULOG_STATUS_OK);
}

/* ============================================================================
   Optional Feature: Extra Outputs
   (`output_*` depends on: Outputs)
============================================================================ */
#if ULOG_HAS_EXTRA_OUTPUTS

//  Private
// ================
static void output_file_handler(ulog_event *ev, void *arg) {
    auto tgt =
        (print_target){.type = PRINT_TARGET_STREAM, .dsc.stream = (FILE *)arg};
    log_print_event(&tgt, ev, true, false, true);
}

// Public
// ================

ulog_output_id ulog_output_add(ulog_output_handler_fn handler, void *arg,
                               ulog_level level) {
    if (handler == nullptr || !level_is_valid(level)) {
        return ULOG_OUTPUT_INVALID;
    }

    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_OUTPUT_INVALID;
    }
    for (auto i = 0; i < output_total_num; i++) {
        if (output_data.outputs[i].handler == nullptr) {
            output_data.outputs[i] = (output_slot){handler, arg, level};
            (void)lock_unlock();
            return i;
        }
    }
    (void)lock_unlock();
    return ULOG_OUTPUT_INVALID;
}

/// @brief Add file handler
ulog_output_id ulog_output_add_file(FILE *file, ulog_level level) {
    if (file == nullptr) {
        return ULOG_OUTPUT_INVALID;
    }
    return ulog_output_add(output_file_handler, file, level);
}

/// @brief Remove an output from the logging system
ulog_status ulog_output_remove(ulog_output_id output) {
    if (output < 0 || output >= output_total_num) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }

    if (output == ULOG_OUTPUT_STDOUT) {
        return ULOG_STATUS_ERROR;  // Cannot remove stdout output
    }

    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;
    }
    if (output_data.outputs[output].handler == nullptr) {
        if (lock_unlock() != ULOG_STATUS_OK) {
            return ULOG_STATUS_BUSY;
        }
        return ULOG_STATUS_NOT_FOUND;  // Output not found or already removed
    }

    // Mark output as removed by setting handler to nullptr
    output_data.outputs[output].handler = nullptr;
    output_data.outputs[output].arg     = nullptr;
    output_data.outputs[output].level   = output_stdout_default_level;

    return lock_unlock();
}

#else  // ULOG_HAS_EXTRA_OUTPUTS

// Disabled Public
// ================

#if ULOG_HAS_WARN_NOT_ENABLED

ulog_output_id ulog_output_add(ulog_output_handler_fn handler, void *arg,
                               ulog_level level) {
    (void)(handler);
    (void)(arg);
    (void)(level);
    warn_not_enabled("ULOG_BUILD_EXTRA_OUTPUTS");
    return ULOG_OUTPUT_INVALID;
}

ulog_output_id ulog_output_add_file(FILE *file, ulog_level level) {
    (void)(file);
    (void)(level);
    warn_not_enabled("ULOG_BUILD_EXTRA_OUTPUTS");
    return ULOG_OUTPUT_INVALID;
}

ulog_status ulog_output_remove(ulog_output_id output) {
    (void)(output);
    warn_not_enabled("ULOG_BUILD_EXTRA_OUTPUTS");
    return ULOG_STATUS_DISABLED;
}

#endif  // ULOG_HAS_WARN_NOT_ENABLED

#endif  // ULOG_HAS_EXTRA_OUTPUTS

/* ============================================================================
   Optional Feature: Dynamic Configuration - Topics
   (`topic_config_*`, depends on: - )
============================================================================ */
#if ULOG_HAS_DYNAMIC_CONFIG

typedef struct {
    bool enabled;
} topic_config;

static topic_config topic_cfg = {
    .enabled = (bool)ULOG_HAS_TOPICS,
};

// Private
// ================

bool topic_config_is_enabled() {
    return topic_cfg.enabled;
}

// Public
// ================

ulog_status ulog_topic_config(bool enabled) {
    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;
    }
    topic_cfg.enabled = enabled;
    return lock_unlock();
}

#else  // ULOG_HAS_DYNAMIC_CONFIG

// Disabled Public
// ================

#if ULOG_HAS_WARN_NOT_ENABLED

ulog_status ulog_topic_config(bool enabled) {
    (void)(enabled);
    warn_not_enabled("ULOG_BUILD_DYNAMIC_CONFIG");
    return ULOG_STATUS_DISABLED;
}

#endif  // ULOG_HAS_WARN_NOT_ENABLED

// Disabled Private
// ================

#define topic_config_is_enabled() (ULOG_HAS_TOPICS)
#endif  // ULOG_HAS_DYNAMIC_CONFIG

/* ============================================================================
   Optional Feature: Topics
   (`topic_*`, depends on: Topics Config, Print, Levels)
============================================================================ */

#if ULOG_HAS_TOPICS

// Private
// ================
#ifndef ULOG_BUILD_TOPICS_STATIC_NUM
/* If static count not provided, default to 0 */
#define ULOG_BUILD_TOPICS_STATIC_NUM 0
#endif

/* Dynamic if mode equals DYNAMIC */
#define TOPIC_IS_DYNAMIC                                                       \
    (ULOG_BUILD_TOPICS_MODE == ULOG_BUILD_TOPICS_MODE_DYNAMIC)
[[maybe_unused]] static constexpr int topic_static_num =
    ULOG_BUILD_TOPICS_STATIC_NUM;
static constexpr ulog_level topic_level_default = ULOG_LEVEL_TRACE;

typedef struct topic_t {
    ulog_topic_id id;
    const char *name;
    ulog_level level;
    ulog_output_id output;

#if TOPIC_IS_DYNAMIC
    struct topic_t *next;  // Pointer to the next topic
#endif

} topic_t;

typedef struct {
    bool new_topic_enabled;  // Whether new topics are enabled by default

#if TOPIC_IS_DYNAMIC
    topic_t *topics;
#else
    topic_t topics[topic_static_num];
#endif

} topic_data_t;

static topic_data_t topic_data = {
    .new_topic_enabled = false,  // New topics are disabled by default

#if TOPIC_IS_DYNAMIC
    .topics = nullptr,  // No topics allocated by default
#else
    .topics = {{0}},  // Initialize static topics array to zero
#endif
};

// === Implementation specific functions for topics ===========================

/// @brief Converts a topic string to its ID
/// @param str - Topic string
/// @return Topic ID or ULOG_TOPIC_ID_INVALID if not found
static ulog_topic_id topic_str_to_id(const char *str);

/// @brief Gets the topic by ID
/// @param topic - Topic ID
/// @return Pointer to the topic if found, nullptr otherwise
static topic_t *topic_get(ulog_topic_id topic);

/// @brief Add a new topic
/// @param topic_name - Topic name
/// @param output - Output id
static ulog_topic_id topic_add(const char *topic_name, ulog_output_id output);

/// @brief Remove a topic by name
/// @param topic_name - Topic name
/// @return ulog_status
static ulog_status topic_remove(const char *topic_name);

/// @brief Remove all topics without taking the configuration lock
static void topic_remove_all();

// === Common Topic Functions =================================================

static void topic_print(print_target *tgt, ulog_event *ev) {
    if (!topic_config_is_enabled()) {
        return;  // Topics are disabled, do nothing
    }

    auto t = topic_get(ev->topic);
    if (t != nullptr) {
        print_to_target(tgt, "[%s] ", t->name);
    }
}

/// @brief Sets the topic level
/// @param topic - Topic ID
/// @param level - Log level to set
/// @return ULOG_STATUS_OK if success, ULOG_STATUS_NOT_FOUND if topic not found
static ulog_status topic_set_level(int topic, ulog_level level) {
    if (!level_is_valid(level)) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }
    auto t = topic_get(topic);
    if (t != nullptr) {
        t->level = level;
        return ULOG_STATUS_OK;
    }
    return ULOG_STATUS_NOT_FOUND;
}

/// @brief Checks if the topic is loggable
/// @param t - Pointer to the topic, nullptr is allowed
/// @param level - Log level to check against
/// @return true if loggable, false otherwise
static bool topic_is_loggable(topic_t *t, ulog_level level) {
    if (t == nullptr) {
        return false;  // Topic not found, cannot log
    }
    if (!level_is_allowed(level, t->level)) {
        return false;  // Topic is disabled, cannot log
    }
    return true;
}

/// @brief Processes the topic
/// @param topic - Topic name
/// @param level - Log level
/// @param is_log_allowed - (Output) log allowed
/// @param topic_id - (Output) topic ID
/// @param output - (Output) topic output ID
static void topic_process(const char *topic, ulog_level level,
                          bool *is_log_allowed, int *topic_id,
                          ulog_output_id *output) {
    if (is_log_allowed == nullptr || topic_id == nullptr || output == nullptr) {
        return;  // Invalid arguments, do nothing
    }

    auto t = topic_get(topic_str_to_id(topic));

    *is_log_allowed = topic_is_loggable(t, level);
    if (!*is_log_allowed) {
        return;  // Topic is not loggable, stop processing
    }
    *topic_id = t->id;      // Set topic ID
    *output   = t->output;  // Set topic output
}

// Public
// ================

ulog_status ulog_topic_level_set(const char *topic_name, ulog_level level) {
    if (is_str_empty(topic_name)) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }

    if (!level_is_valid(level)) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }

    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;
    }

    auto topic_id = topic_str_to_id(topic_name);
    if (topic_id == ULOG_TOPIC_ID_INVALID) {
        return unlock_with_status(
            ULOG_STATUS_NOT_FOUND);  // Topic not found, do nothing
    }
    return unlock_with_status(topic_set_level(topic_id, level));
}

ulog_topic_id ulog_topic_get_id(const char *topic_name) {
    if (is_str_empty(topic_name)) {
        return ULOG_TOPIC_ID_INVALID;
    }

    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_TOPIC_ID_INVALID;
    }

    auto topic_id = topic_str_to_id(topic_name);
    if (lock_unlock() != ULOG_STATUS_OK) {
        return ULOG_TOPIC_ID_INVALID;
    }
    return topic_id;
}

ulog_topic_id ulog_topic_add(const char *topic_name, ulog_output_id output,
                             ulog_level level) {
    if (is_str_empty(topic_name) || !level_is_valid(level)) {
        return ULOG_TOPIC_ID_INVALID;  // Invalid topic name, do nothing
    }

    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_TOPIC_ID_INVALID;
    }

    if (!output_id_is_routable(output)) {
        (void)lock_unlock();
        return ULOG_TOPIC_ID_INVALID;
    }

    auto id = topic_add(topic_name, output);
    if (id != ULOG_TOPIC_ID_INVALID) {
        (void)topic_set_level(id, level);
    }

    if (lock_unlock() != ULOG_STATUS_OK) {
        return ULOG_TOPIC_ID_INVALID;
    }
    return id;
}

ulog_status ulog_topic_remove(const char *topic_name) {
    if (is_str_empty(topic_name)) {
        return ULOG_STATUS_INVALID_ARGUMENT;  // Invalid topic name, do nothing
    }
    return topic_remove(topic_name);
}

#else  // ULOG_HAS_TOPICS

// Disabled Public
// ================

#if ULOG_HAS_WARN_NOT_ENABLED

ulog_status ulog_topic_level_set(const char *topic_name, ulog_level level) {
    (void)(topic_name);
    (void)(level);
    warn_not_enabled("ULOG_BUILD_TOPICS_MODE");
    return ULOG_STATUS_DISABLED;
}

ulog_topic_id ulog_topic_get_id(const char *topic_name) {
    (void)(topic_name);
    warn_not_enabled("ULOG_BUILD_TOPICS_MODE");
    return ULOG_TOPIC_ID_INVALID;
}

ulog_topic_id ulog_topic_add(const char *topic_name, ulog_output_id output,
                             ulog_level level) {
    (void)(topic_name);
    (void)(output);
    (void)(level);
    warn_not_enabled("ULOG_BUILD_TOPICS_MODE");
    return ULOG_TOPIC_ID_INVALID;
}

ulog_status ulog_topic_remove(const char *topic_name) {
    (void)(topic_name);
    warn_not_enabled("ULOG_BUILD_TOPICS_MODE");
    return ULOG_STATUS_DISABLED;
}

#endif  // ULOG_HAS_WARN_NOT_ENABLED

// Disabled Private
// ================

#define topic_print(tgt, ev) (void)(tgt), (void)(ev)
#define topic_process(topic, level, is_log_allowed, topic_id, output)          \
    (void)(topic), (void)(level), (void)(is_log_allowed), (void)(topic_id),    \
        (void)(output)

#endif  // ULOG_HAS_TOPICS

/* ============================================================================
   Optional Feature: Topics - Static Allocation
   (`topic_*`, depends on: Topics)
============================================================================ */
#if ULOG_HAS_TOPICS && TOPIC_IS_DYNAMIC == false
// Private
// ================

ulog_topic_id topic_str_to_id(const char *str) {
    if (is_str_empty(str)) {
        return ULOG_TOPIC_ID_INVALID;
    }
    for (auto i = 0; i < topic_static_num; i++) {
        if (is_str_empty(topic_data.topics[i].name)) {
            continue;  // Skip empty slot; continue searching
        }
        if (strcmp(topic_data.topics[i].name, str) == 0) {
            return topic_data.topics[i].id;
        }
    }
    return ULOG_TOPIC_ID_INVALID;  // Not found
}

static topic_t *topic_get(ulog_topic_id topic) {
    if (topic < topic_static_num && topic >= 0) {
        return &topic_data.topics[topic];
    }
    return nullptr;
}

static ulog_topic_id topic_add(const char *topic_name, ulog_output_id output) {
    if (is_str_empty(topic_name)) {
        return ULOG_TOPIC_ID_INVALID;
    }

    for (auto i = 0; i < topic_static_num; i++) {
        // If there is an empty slot
        if (is_str_empty(topic_data.topics[i].name)) {
            topic_data.topics[i].id     = i;
            topic_data.topics[i].name   = topic_name;
            topic_data.topics[i].level  = topic_level_default;
            topic_data.topics[i].output = output;
            return i;
        }
        // If the topic already exists
        else if (strcmp(topic_data.topics[i].name, topic_name) == 0) {
            return i;
        }
    }
    return ULOG_TOPIC_ID_INVALID;  // No space for new topics
}

static ulog_status topic_remove(const char *topic_name) {
    if (is_str_empty(topic_name)) {
        return ULOG_STATUS_INVALID_ARGUMENT;  // Invalid topic name, do nothing
    }
    if (lock_lock() != ULOG_STATUS_OK) {  // Lock the configuration
        return ULOG_STATUS_BUSY;
    }
    for (auto i = 0; i < topic_static_num; i++) {
        if (is_str_empty(topic_data.topics[i].name)) {
            continue;  // Skip empty slot; continue search
        }
        if (strcmp(topic_data.topics[i].name, topic_name) == 0) {
            // Clear the topic entry
            topic_data.topics[i] = (topic_t){0};
            return lock_unlock();
        }
    }
    if (lock_unlock() != ULOG_STATUS_OK) {  // Unlock the configuration
        return ULOG_STATUS_BUSY;
    }
    return ULOG_STATUS_NOT_FOUND;  // Topic not found
}

static void topic_remove_all() {
    topic_data.new_topic_enabled = false;
    memset(topic_data.topics, 0, sizeof(topic_data.topics));
}
#endif  // ULOG_HAS_TOPICS && TOPIC_IS_DYNAMIC == false

/* ============================================================================
   Optional Feature: Topics - Dynamic Allocation
   (`topic_*`, depends on: Topics)
============================================================================ */

#if ULOG_HAS_TOPICS && TOPIC_IS_DYNAMIC == true
// Private
// ================

static topic_t *topic_get_first() {
    return topic_data.topics;
}

static topic_t *topic_get_next(topic_t *t) {
    return t->next;
}

static topic_t *topic_get_last() {
    auto last = topic_data.topics;
    if (last == nullptr) {
        return nullptr;  // No topics
    }
    while (last->next != nullptr) {
        last = last->next;
    }
    return last;
}

ulog_topic_id topic_str_to_id(const char *str) {
    if (is_str_empty(str)) {
        return ULOG_TOPIC_ID_INVALID;
    }
    for (auto t = topic_get_first(); t != nullptr; t = topic_get_next(t)) {
        if (!is_str_empty(t->name) && strcmp(t->name, str) == 0) {
            return t->id;
        }
    }
    return ULOG_TOPIC_ID_INVALID;
}

static topic_t *topic_get(int topic) {
    if (topic < 0) {
        return nullptr;  // Invalid topic ID
    }
    for (auto t = topic_get_first(); t != nullptr; t = topic_get_next(t)) {
        if (t->id == topic) {
            return t;
        }
    }
    return nullptr;
}

[[nodiscard]] static topic_t *topic_allocate(int id, const char *topic_name,
                                             ulog_output_id output) {
    if (is_str_empty(topic_name)) {
        return nullptr;  // Invalid topic name, do not allocate
    }
    if (id < 0) {
        return nullptr;  // Invalid ID, do not allocate
    }

    auto t = (topic_t *)calloc(1, sizeof(topic_t));
    if (t == nullptr) {
        return nullptr;
    }

    auto name_len  = strlen(topic_name) + 1U;
    auto name_copy = (char *)calloc(name_len, sizeof(char));
    if (name_copy == nullptr) {
        goto cleanup_topic;
    }

    memcpy(name_copy, topic_name, name_len);

    t->id     = id;
    t->name   = name_copy;
    t->level  = topic_level_default;
    t->output = output;
    t->next   = nullptr;
    return t;

cleanup_topic:
    free(t);
    return nullptr;
}

static ulog_topic_id topic_add(const char *topic_name, ulog_output_id output) {
    if (is_str_empty(topic_name)) {
        return ULOG_TOPIC_ID_INVALID;
    }

    for (auto t = topic_get_first(); t != nullptr; t = topic_get_next(t)) {
        if (!is_str_empty(t->name) && strcmp(t->name, topic_name) == 0) {
            return t->id;
        }
    }

    auto t = topic_get_last();
    if (t == nullptr) {
        topic_data.topics = topic_allocate(0, topic_name, output);
        if (topic_data.topics != nullptr) {
            return 0;
        }
        return ULOG_TOPIC_ID_INVALID;
    }

    t->next = topic_allocate(t->id + 1, topic_name, output);
    if (t->next) {
        return t->id + 1;
    }
    return ULOG_TOPIC_ID_INVALID;
}

static ulog_status topic_remove(const char *topic_name) {
    if (is_str_empty(topic_name)) {
        return ULOG_STATUS_INVALID_ARGUMENT;  // Invalid topic name, do nothing
    }
    if (lock_lock() != ULOG_STATUS_OK) {  // Lock the configuration
        return ULOG_STATUS_BUSY;
    }

    auto t          = topic_get_first();
    topic_t *t_prev = nullptr;

    while (t != nullptr) {
        if (!is_str_empty(t->name) && strcmp(t->name, topic_name) == 0) {
            // Found the topic to remove
            if (t_prev == nullptr) {
                // Removing the first topic
                topic_data.topics = t->next;
            } else {
                t_prev->next = t->next;
            }
            free((void *)t->name);  // Free the allocated topic name
            free(t);                // Free the topic memory
            return lock_unlock();
        }
        t_prev = t;
        t      = t->next;
    }

    if (lock_unlock() != ULOG_STATUS_OK) {  // Unlock the configuration
        return ULOG_STATUS_BUSY;
    }
    return ULOG_STATUS_NOT_FOUND;  // Topic not found
}

static void topic_remove_all() {
    topic_data.new_topic_enabled = false;

    auto t            = topic_data.topics;
    topic_data.topics = nullptr;

    while (t != nullptr) {
        auto next = t->next;
        free((void *)t->name);
        free(t);
        t = next;
    }
}

#endif  // ULOG_HAS_TOPICS && TOPIC_IS_DYNAMIC == true

/* ============================================================================
   Optional Feature: Dynamic Configuration - Source Location
   (`src_loc_config_*`, depends on: - )
============================================================================ */
#if ULOG_HAS_DYNAMIC_CONFIG

typedef struct {
    bool enabled;
} src_loc_config;

static src_loc_config src_loc_cfg = {
    .enabled = (bool)ULOG_HAS_SOURCE_LOCATION,
};

// Private
// ================

bool src_loc_config_is_enabled() {
    return src_loc_cfg.enabled;
}

// Public
// ================

ulog_status ulog_source_location_config(bool enabled) {
    if (lock_lock() != ULOG_STATUS_OK) {
        return ULOG_STATUS_BUSY;
    }
    src_loc_cfg.enabled = enabled;
    return lock_unlock();
}

#else  // ULOG_HAS_DYNAMIC_CONFIG

// Disabled Public
// ================

#if ULOG_HAS_WARN_NOT_ENABLED

ulog_status ulog_source_location_config(bool enabled) {
    (void)(enabled);
    warn_not_enabled("ULOG_BUILD_SOURCE_LOCATION");
    return ULOG_STATUS_DISABLED;
}

#endif  // ULOG_HAS_WARN_NOT_ENABLED

// Disabled Private
// ================

#define src_loc_config_is_enabled() (ULOG_HAS_SOURCE_LOCATION)
#endif  // ULOG_HAS_DYNAMIC_CONFIG

/* ============================================================================
   Core Feature: Log
   (`log_*`, depends on: Print, Level, Outputs, Extra Outputs, Prefix, Topics,
                         Time, Color, Locking, Source Location)
============================================================================ */

// Private
// ================
/// @brief Prints the message
/// @param tgt - Target
/// @param ev - Event
static void log_print_message(print_target *tgt, ulog_event *ev) {

#if ULOG_HAS_SOURCE_LOCATION
    if (src_loc_config_is_enabled() && ev->file != nullptr) {
        print_to_target(tgt, "%s:%d: ", ev->file, ev->line);  // file and line
    }
#endif  // ULOG_HAS_SOURCE_LOCATION

    if (!is_str_empty(ev->message)) {
        print_to_target_valist(tgt, ev->message,
                               ev->message_format_args);  // message
    } else {
        print_to_target(tgt, "nullptr");  // message
    }
}

/// @brief Writes a formatted message
/// @details The message is formatted as follows:
///
/// [Time][Prefix][Topic]Level [File: ]Message
/// or
/// [Time ][Topic ]Level [File: ]Message
///
/// where [Entry] is an optional part
///
/// @param tgt - Target
/// @param ev - Event
/// @param full_time - Full time or short time
/// @param color - Color or no color
/// @param new_line - New line in the end or no new line
static void log_print_event(print_target *tgt, ulog_event *ev, bool full_time,
                            bool color, bool new_line) {

    color ? color_print_start(tgt, ev) : (void)0;

    auto append_space = true;
    (void)append_space;  // May be unused if no prefix and time
#if ULOG_HAS_PREFIX
    if (prefix_data.function != nullptr && prefix_config_is_enabled()) {
        append_space = false;  // Prefix does not need leading space
    }
#endif

    full_time ? time_print_full(tgt, ev, append_space)
              : time_print_short(tgt, ev, append_space);

    prefix_print(tgt);
    level_print(tgt, ev);
    topic_print(tgt, ev);
    log_print_message(tgt, ev);

    color ? color_print_end(tgt) : (void)0;
    new_line ? print_to_target(tgt, "\n") : (void)0;
}

void log_fill_event(ulog_event *ev, const char *message, ulog_level level,
                    const char *file, int line, int topic_id) {
    if (ev == nullptr) {
        return;  // Invalid event, do nothing
    }

    ev->message = message;
    ev->level   = level;

#if ULOG_HAS_SOURCE_LOCATION
    ev->file = file;
    ev->line = line;
#else
    (void)(file), (void)(line);  // Unused if file string is disabled
#endif  // ULOG_HAS_SOURCE_LOCATION

#if ULOG_HAS_TOPICS
    ev->topic = topic_id;
#else
    (void)(topic_id);  // Unused if topics are disabled
#endif

#if ULOG_HAS_TIME
    ev->has_time_snapshot = false;  // Time will be filled later
#endif

    time_fill_current_time(ev);  // Fill time with current value
}

// Public
// ================

ulog_status ulog_event_to_cstr(ulog_event *ev, char *out, size_t out_size) {
    if (ev == nullptr || out == nullptr || out_size == 0) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }
    auto tgt = (print_target){.type       = PRINT_TARGET_BUFFER,
                              .dsc.buffer = {out, 0, out_size}};

    // Create a copy of the event to avoid va_list issues
    auto ev_copy = *ev;
    va_copy(ev_copy.message_format_args, ev->message_format_args);

    log_print_event(&tgt, &ev_copy, false, false, false);

    va_end(ev_copy.message_format_args);
    return ULOG_STATUS_OK;
}

void ulog_log(ulog_level level, const char *file, int line, const char *topic,
              const char *message, ...) {
    if (lock_lock() != ULOG_STATUS_OK) {
        return;  // Failed to acquire lock, drop log
    }

    if (!level_is_valid(level)) {
        (void)lock_unlock();
        return;  // Invalid level for current configuration
    }

    // Try to get topic ID, outputs and check if logging is allowed for this
    // topic
    auto output   = ULOG_OUTPUT_ALL;
    auto topic_id = -1;
    if (!is_str_empty(topic)) {
        auto is_log_allowed = false;
        topic_process(topic, level, &is_log_allowed, &topic_id, &output);
        if (!is_log_allowed) {
            (void)lock_unlock();
            return;  // Topic is not enabled or level is lower than topic level
        }
    }

    auto ev = (ulog_event){0};
    log_fill_event(&ev, message, level, file, line, topic_id);
    va_start(ev.message_format_args, message);

    prefix_update(&ev);

    // Handle output routing
    if (output == ULOG_OUTPUT_ALL) {
        output_handle_all(&ev);
    } else {
        output_handle_by_id(&ev, output);
    }

    va_end(ev.message_format_args);

    (void)lock_unlock();
}

/* ============================================================================
   Core Feature: Clean up
   (`init_*`, depends on: Locking, Outputs, Prefix, Time, Color)
============================================================================ */

// Public
// ================

ulog_status ulog_cleanup() {
    if (lock_lock() != ULOG_STATUS_OK) {  // Lock the configuration
        return ULOG_STATUS_BUSY;
    }
#if ULOG_HAS_TOPICS
    topic_remove_all();
#endif  // ULOG_HAS_TOPICS

    level_apply_defaults(
#if ULOG_HAS_LEVEL_SHORT && !ULOG_HAS_LEVEL_LONG
        level_names_default_short,
#else
        level_names_default_long,
#endif
        ULOG_LEVEL_FATAL);

    // Cleanup Outputs (keep stdout (index 0) registered but reset its level)
    output_data.outputs[ULOG_OUTPUT_STDOUT].level = output_stdout_default_level;
#if ULOG_HAS_EXTRA_OUTPUTS
    for (auto i = 1; i < output_total_num; i++) {
        output_data.outputs[i].handler = nullptr;
        output_data.outputs[i].arg     = nullptr;
        output_data.outputs[i].level   = output_stdout_default_level;
    }
#endif  // ULOG_HAS_EXTRA_OUTPUTS

#if ULOG_HAS_PREFIX
    // Reset prefix state
    prefix_data.function = nullptr;
    memset(prefix_data.prefix, 0, sizeof(prefix_data.prefix));
#endif

#if ULOG_HAS_DYNAMIC_CONFIG
    color_cfg.enabled     = (bool)ULOG_HAS_COLOR;
    prefix_cfg.enabled    = (bool)ULOG_HAS_PREFIX;
    time_cfg.enabled      = (bool)ULOG_HAS_TIME;
    level_cfg.short_style = false;
    topic_cfg.enabled     = (bool)ULOG_HAS_TOPICS;
    src_loc_cfg.enabled   = (bool)ULOG_HAS_SOURCE_LOCATION;
#endif

    return unlock_with_status(ULOG_STATUS_OK);
}

#endif  // ULOG_BUILD_DISABLED
