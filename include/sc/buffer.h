#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sc/allocator.h"
#include "sc/result.h"

typedef struct sc_buf {
    const uint8_t *ptr;
    size_t len;
} sc_buf;

typedef struct sc_bytes {
    uint8_t *ptr;
    size_t len;
    size_t cap;
    sc_allocator *alloc;
} sc_bytes;

sc_buf sc_buf_from_parts(const void *ptr, size_t len);
void sc_bytes_init(sc_bytes *bytes, sc_allocator *alloc);
sc_status sc_bytes_from_buf(sc_allocator *alloc, sc_buf input, sc_bytes *out);
sc_status sc_bytes_reserve(sc_bytes *bytes, size_t additional);
sc_status sc_bytes_append(sc_bytes *bytes, sc_buf input);
void sc_bytes_clear(sc_bytes *bytes);
void sc_bytes_secure_clear(sc_bytes *bytes);
