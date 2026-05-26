#include "sc/time.h"

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

sc_status sc_clock_monotonic(sc_instant *out)
{
    struct timespec ts = {0};

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.time.invalid_argument");
    }
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return sc_status_io("sc.time.monotonic_failed");
    }

    out->ns = ((int64_t)ts.tv_sec * 1000000000) + (int64_t)ts.tv_nsec;
    return sc_status_ok();
}

sc_status sc_clock_wall(sc_wall_time *out)
{
    struct timespec ts = {0};

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.time.invalid_argument");
    }
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) {
        return sc_status_io("sc.time.wall_failed");
    }

    out->unix_ns = ((int64_t)ts.tv_sec * 1000000000) + (int64_t)ts.tv_nsec;
    return sc_status_ok();
}

sc_status sc_time_format_rfc3339(sc_allocator *alloc, sc_wall_time time, sc_string *out)
{
    time_t seconds = (time_t)(time.unix_ns / 1000000000);
    struct tm tm = {0};
    char buffer[sizeof("1970-01-01T00:00:00Z")] = {0};
    int written = 0;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.time.invalid_argument");
    }
    if (gmtime_r(&seconds, &tm) == nullptr) {
        return sc_status_io("sc.time.rfc3339_failed");
    }

    written = snprintf(buffer,
                       sizeof(buffer),
                       "%04d-%02d-%02dT%02d:%02d:%02dZ",
                       tm.tm_year + 1900,
                       tm.tm_mon + 1,
                       tm.tm_mday,
                       tm.tm_hour,
                       tm.tm_min,
                       tm.tm_sec);
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return sc_status_io("sc.time.rfc3339_failed");
    }

    return sc_string_from_cstr(alloc, buffer, out);
}

sc_status sc_random_bytes(void *ptr, size_t len)
{
    FILE *file = nullptr;
    size_t read_count = 0;
    sc_status status = sc_status_ok();

    if (ptr == nullptr && len > 0) {
        return sc_status_invalid_argument("sc.random.invalid_argument");
    }
    if (len == 0) {
        return sc_status_ok();
    }

    file = fopen("/dev/urandom", "rb");
    if (file == nullptr) {
        status = sc_status_io("sc.random.open_failed");
        goto cleanup;
    }

    read_count = fread(ptr, 1, len, file);
    if (fclose(file) != 0) {
        file = nullptr;
        status = sc_status_io("sc.random.close_failed");
        goto cleanup;
    }
    file = nullptr;
    if (read_count != len) {
        status = sc_status_io(errno == 0 ? "sc.random.short_read" : "sc.random.read_failed");
        goto cleanup;
    }

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    return status;
}

sc_status sc_uuid_v4(sc_allocator *alloc, sc_string *out)
{
    unsigned char bytes[16] = {0};
    char text[37] = {0};
    sc_status status;
    int written = 0;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.uuid.invalid_argument");
    }

    status = sc_random_bytes(bytes, sizeof(bytes));
    if (!sc_status_is_ok(status)) {
        return status;
    }

    bytes[6] = (unsigned char)((bytes[6] & 0x0Fu) | 0x40u);
    bytes[8] = (unsigned char)((bytes[8] & 0x3Fu) | 0x80u);

    written = snprintf(text,
                       sizeof(text),
                       "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                       bytes[0],
                       bytes[1],
                       bytes[2],
                       bytes[3],
                       bytes[4],
                       bytes[5],
                       bytes[6],
                       bytes[7],
                       bytes[8],
                       bytes[9],
                       bytes[10],
                       bytes[11],
                       bytes[12],
                       bytes[13],
                       bytes[14],
                       bytes[15]);
    if (written != 36) {
        return sc_status_io("sc.uuid.format_failed");
    }

    return sc_string_from_cstr(alloc, text, out);
}
