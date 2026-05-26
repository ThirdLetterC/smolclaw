#pragma once

#include <stddef.h>
#include <stdio.h>

#include "sc/string.h"

typedef enum sc_log_level {
    SC_LOG_TRACE = 0,
    SC_LOG_DEBUG,
    SC_LOG_INFO,
    SC_LOG_WARN,
    SC_LOG_ERROR,
    SC_LOG_FATAL,
    SC_LOG_OFF
} sc_log_level;

typedef enum sc_log_format {
    SC_LOG_FORMAT_CONSOLE = 0,
    SC_LOG_FORMAT_JSON
} sc_log_format;

typedef struct sc_log_field {
    const char *key;
    sc_str value;
    bool secret;
} sc_log_field;

const char *sc_log_level_name(sc_log_level level);
bool sc_log_level_from_cstr(const char *text, sc_log_level *out);
const char *sc_log_format_name(sc_log_format format);
bool sc_log_format_from_cstr(const char *text, sc_log_format *out);
void sc_log_set_level(sc_log_level level);
void sc_log_set_format(sc_log_format format);
void sc_log_configure_from_env(void);
void sc_log_write(sc_log_level level,
                  const char *target,
                  const char *event_key,
                  const sc_log_field *fields,
                  size_t field_count);
sc_str sc_log_redact_field(const sc_log_field *field);
void sc_log_write_to(FILE *stream,
                     sc_log_level level,
                     const char *target,
                     const char *event_key,
                     const sc_log_field *fields,
                     size_t field_count);
