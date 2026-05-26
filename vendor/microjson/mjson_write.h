/* No-allocation JSON writer extension for microjson.
 *
 * This file is a SmolClaw-local extension to the vendored microjson snapshot.
 * It writes JSON tokens into caller-provided storage, or counts bytes when the
 * writer is initialized with a null buffer and zero capacity.
 */

#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MJSON_WRITE_OK = 0,
    MJSON_WRITE_INVALID = 1,
    MJSON_WRITE_OVERFLOW = 2,
    MJSON_WRITE_FORMAT = 3,
};

typedef struct mjson_writer {
    char *buffer;
    size_t capacity;
    size_t length;
    int truncated;
} mjson_writer;

void mjson_writer_init(mjson_writer *writer, char *buffer, size_t capacity);
size_t mjson_writer_length(const mjson_writer *writer);
int mjson_writer_truncated(const mjson_writer *writer);

int mjson_write_raw(mjson_writer *writer, const char *ptr, size_t len);
int mjson_write_cstr(mjson_writer *writer, const char *ptr);
int mjson_write_null(mjson_writer *writer);
int mjson_write_bool(mjson_writer *writer, int value);
int mjson_write_number_double(mjson_writer *writer, double value);
int mjson_write_string(mjson_writer *writer, const char *ptr, size_t len);

#ifdef __cplusplus
}
#endif
