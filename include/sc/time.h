#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sc/result.h"
#include "sc/string.h"

typedef struct sc_instant {
    int64_t ns;
} sc_instant;

typedef struct sc_wall_time {
    int64_t unix_ns;
} sc_wall_time;

sc_status sc_clock_monotonic(sc_instant *out);
sc_status sc_clock_wall(sc_wall_time *out);
sc_status sc_time_format_rfc3339(sc_allocator *alloc, sc_wall_time time, sc_string *out);
sc_status sc_random_bytes(void *ptr, size_t len);
sc_status sc_uuid_v4(sc_allocator *alloc, sc_string *out);
