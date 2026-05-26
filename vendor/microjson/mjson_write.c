/* No-allocation JSON writer extension for microjson.
 *
 * This file is a SmolClaw-local extension to the vendored microjson snapshot.
 */

#include "mjson_write.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

static char mjson_hex_digit(unsigned int value);

void mjson_writer_init(mjson_writer *writer, char *buffer, size_t capacity)
{
    if (writer == 0) {
        return;
    }
    writer->buffer = buffer;
    writer->capacity = capacity;
    writer->length = 0;
    writer->truncated = 0;
}

size_t mjson_writer_length(const mjson_writer *writer)
{
    return writer == 0 ? 0 : writer->length;
}

int mjson_writer_truncated(const mjson_writer *writer)
{
    return writer == 0 ? 1 : writer->truncated;
}

int mjson_write_raw(mjson_writer *writer, const char *ptr, size_t len)
{
    size_t available = 0;
    size_t remaining = 0;

    if (writer == 0 || (len > 0 && ptr == 0)) {
        return MJSON_WRITE_INVALID;
    }
    if (SIZE_MAX - writer->length < len) {
        return MJSON_WRITE_OVERFLOW;
    }
    if (writer->buffer != 0 && writer->length < writer->capacity) {
        available = writer->capacity - writer->length;
        if (available > len) {
            available = len;
        }
        if (available > 0) {
            (void)memcpy(writer->buffer + writer->length, ptr, available);
        }
    }
    remaining = writer->length < writer->capacity ? writer->capacity - writer->length : 0;
    if (writer->buffer != 0 && remaining < len) {
        writer->truncated = 1;
    }
    writer->length += len;
    return MJSON_WRITE_OK;
}

int mjson_write_cstr(mjson_writer *writer, const char *ptr)
{
    if (ptr == 0) {
        return MJSON_WRITE_INVALID;
    }
    return mjson_write_raw(writer, ptr, strlen(ptr));
}

int mjson_write_null(mjson_writer *writer)
{
    return mjson_write_cstr(writer, "null");
}

int mjson_write_bool(mjson_writer *writer, int value)
{
    return mjson_write_cstr(writer, value ? "true" : "false");
}

int mjson_write_number_double(mjson_writer *writer, double value)
{
    char number[64] = {0};
    int written = snprintf(number, sizeof(number), "%.17g", value);

    if (written < 0 || (size_t)written >= sizeof(number)) {
        return MJSON_WRITE_FORMAT;
    }
    return mjson_write_raw(writer, number, (size_t)written);
}

int mjson_write_string(mjson_writer *writer, const char *ptr, size_t len)
{
    int status = MJSON_WRITE_OK;

    if (writer == 0 || (len > 0 && ptr == 0)) {
        return MJSON_WRITE_INVALID;
    }
    status = mjson_write_cstr(writer, "\"");
    for (size_t i = 0; status == MJSON_WRITE_OK && i < len; i += 1) {
        unsigned char ch = (unsigned char)ptr[i];
        char escaped[6] = {'\\', 'u', '0', '0', '0', '0'};
        if (ch == '"') {
            status = mjson_write_cstr(writer, "\\\"");
        } else if (ch == '\\') {
            status = mjson_write_cstr(writer, "\\\\");
        } else if (ch == '\n') {
            status = mjson_write_cstr(writer, "\\n");
        } else if (ch == '\r') {
            status = mjson_write_cstr(writer, "\\r");
        } else if (ch == '\t') {
            status = mjson_write_cstr(writer, "\\t");
        } else if (ch < 0x20u) {
            escaped[4] = mjson_hex_digit((unsigned int)(ch >> 4u));
            escaped[5] = mjson_hex_digit((unsigned int)(ch & 0x0Fu));
            status = mjson_write_raw(writer, escaped, sizeof(escaped));
        } else {
            status = mjson_write_raw(writer, (const char *)&ptr[i], 1);
        }
    }
    if (status == MJSON_WRITE_OK) {
        status = mjson_write_cstr(writer, "\"");
    }
    return status;
}

static char mjson_hex_digit(unsigned int value)
{
    static const char digits[] = "0123456789abcdef";
    return digits[value & 0x0Fu];
}
