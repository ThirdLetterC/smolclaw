#include "sc/log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SC_HAVE_ULOG
#include "ulog/ulog.h"
#endif

static sc_log_level min_level = SC_LOG_INFO;
static sc_log_format log_format = SC_LOG_FORMAT_CONSOLE;

static int append_text(char *buffer, size_t capacity, size_t *offset, const char *text);
static int append_str(char *buffer, size_t capacity, size_t *offset, sc_str text);
static bool equals_ignore_case(const char *left, const char *right);
static bool contains_ignore_case(const char *text, const char *needle);
static bool key_is_public_diagnostic(const char *key);
static char lower_ascii(char ch);
static void write_console_line(FILE *stream,
                               sc_log_level level,
                               const char *target,
                               const char *event_key,
                               const sc_log_field *fields,
                               size_t field_count);
static void write_console_fallback(FILE *stream,
                                   sc_log_level level,
                                   const char *target,
                                   const char *event_key,
                                   const sc_log_field *fields,
                                   size_t field_count);
static void write_json_line(FILE *stream,
                            sc_log_level level,
                            const char *target,
                            const char *event_key,
                            const sc_log_field *fields,
                            size_t field_count);
static void write_json_string(FILE *stream, sc_str value);

#ifdef SC_HAVE_ULOG
static ulog_level to_ulog_level(sc_log_level level);
static bool write_with_ulog(FILE *stream, sc_log_level level, const char *target, const char *message);
#endif

const char *sc_log_level_name(sc_log_level level)
{
    switch (level) {
    case SC_LOG_TRACE:
        return "trace";
    case SC_LOG_DEBUG:
        return "debug";
    case SC_LOG_INFO:
        return "info";
    case SC_LOG_WARN:
        return "warn";
    case SC_LOG_ERROR:
        return "error";
    case SC_LOG_FATAL:
        return "fatal";
    case SC_LOG_OFF:
        return "off";
    }
    return "unknown";
}

bool sc_log_level_from_cstr(const char *text, sc_log_level *out)
{
    sc_log_level level = SC_LOG_INFO;

    if (text == nullptr || out == nullptr) {
        return false;
    }
    if (equals_ignore_case(text, "trace")) {
        level = SC_LOG_TRACE;
    } else if (equals_ignore_case(text, "debug")) {
        level = SC_LOG_DEBUG;
    } else if (equals_ignore_case(text, "info")) {
        level = SC_LOG_INFO;
    } else if (equals_ignore_case(text, "warn") || equals_ignore_case(text, "warning")) {
        level = SC_LOG_WARN;
    } else if (equals_ignore_case(text, "error")) {
        level = SC_LOG_ERROR;
    } else if (equals_ignore_case(text, "fatal")) {
        level = SC_LOG_FATAL;
    } else if (equals_ignore_case(text, "off") || equals_ignore_case(text, "none") || equals_ignore_case(text, "quiet")) {
        level = SC_LOG_OFF;
    } else {
        return false;
    }
    *out = level;
    return true;
}

const char *sc_log_format_name(sc_log_format format)
{
    switch (format) {
    case SC_LOG_FORMAT_CONSOLE:
        return "console";
    case SC_LOG_FORMAT_JSON:
        return "json";
    }
    return "unknown";
}

bool sc_log_format_from_cstr(const char *text, sc_log_format *out)
{
    sc_log_format format = SC_LOG_FORMAT_CONSOLE;

    if (text == nullptr || out == nullptr) {
        return false;
    }
    if (equals_ignore_case(text, "console")) {
        format = SC_LOG_FORMAT_CONSOLE;
    } else if (equals_ignore_case(text, "json")) {
        format = SC_LOG_FORMAT_JSON;
    } else {
        return false;
    }
    *out = format;
    return true;
}

static bool key_looks_secret(const char *key)
{
    return key != nullptr &&
           !key_is_public_diagnostic(key) &&
           (contains_ignore_case(key, "key") ||
            contains_ignore_case(key, "token") ||
            contains_ignore_case(key, "secret") ||
            contains_ignore_case(key, "password") ||
            contains_ignore_case(key, "authorization") ||
            contains_ignore_case(key, "auth") ||
            contains_ignore_case(key, "cookie") ||
            contains_ignore_case(key, "pairing") ||
            contains_ignore_case(key, "otp") ||
            contains_ignore_case(key, "prompt") ||
            contains_ignore_case(key, "raw_message") ||
            contains_ignore_case(key, "channel_message") ||
            contains_ignore_case(key, "payload") ||
            contains_ignore_case(key, "body") ||
            contains_ignore_case(key, "content") ||
            contains_ignore_case(key, "private_file") ||
            contains_ignore_case(key, "file_content") ||
            contains_ignore_case(key, "header"));
}

void sc_log_set_level(sc_log_level level)
{
    if (level < SC_LOG_TRACE) {
        level = SC_LOG_TRACE;
    }
    if (level > SC_LOG_OFF) {
        level = SC_LOG_OFF;
    }
    min_level = level;
}

void sc_log_set_format(sc_log_format format)
{
    if (format < SC_LOG_FORMAT_CONSOLE || format > SC_LOG_FORMAT_JSON) {
        format = SC_LOG_FORMAT_CONSOLE;
    }
    log_format = format;
}

void sc_log_configure_from_env(void)
{
    const char *value = getenv("SMOLCLAW_LOG_LEVEL");
    const char *format_value = getenv("SMOLCLAW_LOG_FORMAT");
    sc_log_level level = SC_LOG_INFO;
    sc_log_format format = SC_LOG_FORMAT_CONSOLE;

    if (value == nullptr || value[0] == '\0') {
        value = getenv("SC_LOG");
    }
    if (sc_log_level_from_cstr(value, &level)) {
        sc_log_set_level(level);
    }
    if (format_value == nullptr || format_value[0] == '\0') {
        format_value = getenv("SC_LOG_FORMAT");
    }
    if (sc_log_format_from_cstr(format_value, &format)) {
        sc_log_set_format(format);
    }
}

void sc_log_write(sc_log_level level,
                  const char *target,
                  const char *event_key,
                  const sc_log_field *fields,
                  size_t field_count)
{
    if (level < min_level) {
        return;
    }
    sc_log_write_to(stderr, level, target, event_key, fields, field_count);
}

sc_str sc_log_redact_field(const sc_log_field *field)
{
    static const char redacted[] = "[REDACTED]";

    if (field == nullptr) {
        return sc_str_from_parts(nullptr, 0);
    }
    if (field->secret || key_looks_secret(field->key)) {
        return sc_str_from_parts(redacted, sizeof(redacted) - 1);
    }
    return field->value;
}

void sc_log_write_to(FILE *stream,
                     sc_log_level level,
                     const char *target,
                     const char *event_key,
                     const sc_log_field *fields,
                     size_t field_count)
{
    if (stream == nullptr) {
        return;
    }
    if (target == nullptr) {
        target = "sc";
    }
    if (event_key == nullptr) {
        event_key = "sc.log.event";
    }

    if (log_format == SC_LOG_FORMAT_JSON) {
        write_json_line(stream, level, target, event_key, fields, field_count);
        return;
    }

    write_console_line(stream, level, target, event_key, fields, field_count);
}

static void write_console_line(FILE *stream,
                               sc_log_level level,
                               const char *target,
                               const char *event_key,
                               const sc_log_field *fields,
                               size_t field_count)
{
    char message[4096] = {0};
    size_t offset = 0;

    if (append_text(message, sizeof(message), &offset, "level=") < 0 ||
        append_text(message, sizeof(message), &offset, sc_log_level_name(level)) < 0 ||
        append_text(message, sizeof(message), &offset, " target=") < 0 ||
        append_text(message, sizeof(message), &offset, target) < 0 ||
        append_text(message, sizeof(message), &offset, " event=") < 0 ||
        append_text(message, sizeof(message), &offset, event_key) < 0) {
        write_console_fallback(stream, level, target, event_key, fields, field_count);
        return;
    }
    for (size_t i = 0; i < field_count; i += 1) {
        sc_str value = sc_log_redact_field(&fields[i]);
        if (append_text(message, sizeof(message), &offset, " ") < 0 ||
            append_text(message, sizeof(message), &offset, fields[i].key == nullptr ? "field" : fields[i].key) < 0 ||
            append_text(message, sizeof(message), &offset, "=") < 0 ||
            append_str(message, sizeof(message), &offset, value) < 0) {
            write_console_fallback(stream, level, target, event_key, fields, field_count);
            return;
        }
    }

#ifdef SC_HAVE_ULOG
    if (write_with_ulog(stream, level, target, message)) {
        return;
    }
#endif

    write_console_fallback(stream, level, target, event_key, fields, field_count);
}

static int append_text(char *buffer, size_t capacity, size_t *offset, const char *text)
{
    size_t len = text == nullptr ? 0 : strlen(text);
    if (buffer == nullptr || offset == nullptr || *offset >= capacity || capacity - *offset <= len) {
        return -1;
    }
    if (len > 0) {
        (void)memcpy(buffer + *offset, text, len);
    }
    *offset += len;
    buffer[*offset] = '\0';
    return 0;
}

static int append_str(char *buffer, size_t capacity, size_t *offset, sc_str text)
{
    if (text.ptr == nullptr || text.len == 0) {
        return 0;
    }
    if (buffer == nullptr || offset == nullptr || *offset >= capacity || capacity - *offset <= text.len) {
        return -1;
    }
    (void)memcpy(buffer + *offset, text.ptr, text.len);
    *offset += text.len;
    buffer[*offset] = '\0';
    return 0;
}

static bool equals_ignore_case(const char *left, const char *right)
{
    size_t i = 0;
    if (left == nullptr || right == nullptr) {
        return false;
    }
    while (left[i] != '\0' && right[i] != '\0') {
        if (lower_ascii(left[i]) != lower_ascii(right[i])) {
            return false;
        }
        i += 1;
    }
    return left[i] == '\0' && right[i] == '\0';
}

static bool contains_ignore_case(const char *text, const char *needle)
{
    size_t text_len = text == nullptr ? 0 : strlen(text);
    size_t needle_len = needle == nullptr ? 0 : strlen(needle);

    if (text_len == 0 || needle_len == 0 || text_len < needle_len) {
        return false;
    }
    for (size_t i = 0; i <= text_len - needle_len; i += 1) {
        bool match = true;
        for (size_t j = 0; j < needle_len; j += 1) {
            if (lower_ascii(text[i + j]) != lower_ascii(needle[j])) {
                match = false;
                break;
            }
        }
        if (match) {
            return true;
        }
    }
    return false;
}

static char lower_ascii(char ch)
{
    return ch >= 'A' && ch <= 'Z' ? (char)(ch - 'A' + 'a') : ch;
}

static bool key_is_public_diagnostic(const char *key)
{
    return equals_ignore_case(key, "error_key");
}

static void write_console_fallback(FILE *stream,
                                   sc_log_level level,
                                   const char *target,
                                   const char *event_key,
                                   const sc_log_field *fields,
                                   size_t field_count)
{
    (void)fprintf(stream, "level=%s target=%s event=%s", sc_log_level_name(level), target, event_key);
    for (size_t i = 0; i < field_count; i += 1) {
        sc_str value = sc_log_redact_field(&fields[i]);
        (void)fprintf(stream, " %s=", fields[i].key == nullptr ? "field" : fields[i].key);
        if (value.ptr != nullptr && value.len > 0) {
            (void)fwrite(value.ptr, 1, value.len, stream);
        }
    }
    (void)fprintf(stream, "\n");
}

static void write_json_line(FILE *stream,
                            sc_log_level level,
                            const char *target,
                            const char *event_key,
                            const sc_log_field *fields,
                            size_t field_count)
{
    (void)fprintf(stream, "{\"level\":");
    write_json_string(stream, sc_str_from_cstr(sc_log_level_name(level)));
    (void)fprintf(stream, ",\"target\":");
    write_json_string(stream, sc_str_from_cstr(target));
    (void)fprintf(stream, ",\"event\":");
    write_json_string(stream, sc_str_from_cstr(event_key));
    (void)fprintf(stream, ",\"fields\":{");
    for (size_t i = 0; i < field_count; i += 1) {
        sc_str value = sc_log_redact_field(&fields[i]);
        if (i > 0) {
            (void)fprintf(stream, ",");
        }
        write_json_string(stream, sc_str_from_cstr(fields[i].key == nullptr ? "field" : fields[i].key));
        (void)fprintf(stream, ":");
        write_json_string(stream, value);
    }
    (void)fprintf(stream, "}}\n");
}

static void write_json_string(FILE *stream, sc_str value)
{
    (void)fprintf(stream, "\"");
    if (value.ptr == nullptr) {
        (void)fprintf(stream, "\"");
        return;
    }
    for (size_t i = 0; i < value.len; i += 1) {
        unsigned char ch = (unsigned char)value.ptr[i];
        switch (ch) {
        case '"':
            (void)fprintf(stream, "\\\"");
            break;
        case '\\':
            (void)fprintf(stream, "\\\\");
            break;
        case '\b':
            (void)fprintf(stream, "\\b");
            break;
        case '\f':
            (void)fprintf(stream, "\\f");
            break;
        case '\n':
            (void)fprintf(stream, "\\n");
            break;
        case '\r':
            (void)fprintf(stream, "\\r");
            break;
        case '\t':
            (void)fprintf(stream, "\\t");
            break;
        default:
            if (ch < 0x20) {
                (void)fprintf(stream, "\\u%04x", (unsigned int)ch);
            } else {
                (void)fputc((int)ch, stream);
            }
            break;
        }
    }
    (void)fprintf(stream, "\"");
}

#ifdef SC_HAVE_ULOG
static ulog_level to_ulog_level(sc_log_level level)
{
    switch (level) {
    case SC_LOG_TRACE:
        return ULOG_LEVEL_TRACE;
    case SC_LOG_DEBUG:
        return ULOG_LEVEL_DEBUG;
    case SC_LOG_INFO:
        return ULOG_LEVEL_INFO;
    case SC_LOG_WARN:
        return ULOG_LEVEL_WARN;
    case SC_LOG_ERROR:
        return ULOG_LEVEL_ERROR;
    case SC_LOG_FATAL:
    case SC_LOG_OFF:
        return ULOG_LEVEL_FATAL;
    }
    return ULOG_LEVEL_INFO;
}

static bool write_with_ulog(FILE *stream, sc_log_level level, const char *target, const char *message)
{
    static unsigned long topic_counter = 0;
    char topic[64] = {0};
    ulog_level ulog_level = to_ulog_level(level);
    ulog_output_id output;
    ulog_topic_id topic_id;

    output = ulog_output_add_file(stream, ulog_level);
    if (output == ULOG_OUTPUT_INVALID) {
        return false;
    }

    (void)snprintf(topic, sizeof(topic), "sc.%lu", ++topic_counter);
    topic_id = ulog_topic_add(topic, output, ulog_level);
    if (topic_id == ULOG_TOPIC_ID_INVALID) {
        (void)ulog_output_remove(output);
        return false;
    }

    ulog_log(ulog_level, target == nullptr ? "sc" : target, 0, topic, "%s", message == nullptr ? "" : message);
    (void)ulog_topic_remove(topic);
    (void)ulog_output_remove(output);
    return true;
}
#endif
