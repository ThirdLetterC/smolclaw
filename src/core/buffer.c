#include "sc/buffer.h"

#include <string.h>

#include "sc/api.h"

sc_buf sc_buf_from_parts(const void *ptr, size_t len)
{
    return (sc_buf){
        .ptr = (const uint8_t *)ptr,
        .len = len,
    };
}

void sc_bytes_init(sc_bytes *bytes, sc_allocator *alloc)
{
    if (bytes == nullptr) {
        return;
    }

    *bytes = (sc_bytes){
        .ptr = nullptr,
        .len = 0,
        .cap = 0,
        .alloc = alloc == nullptr ? sc_allocator_heap() : alloc,
    };
}

sc_status sc_bytes_from_buf(sc_allocator *alloc, sc_buf input, sc_bytes *out)
{
    sc_bytes tmp = {0};
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.bytes.invalid_argument");
    }
    if (input.len > 0 && input.ptr == nullptr) {
        return sc_status_invalid_argument("sc.bytes.invalid_argument");
    }

    sc_bytes_init(&tmp, alloc);
    status = sc_bytes_append(&tmp, input);
    if (!sc_status_is_ok(status)) {
        sc_bytes_clear(&tmp);
        return status;
    }

    *out = tmp;
    return sc_status_ok();
}

sc_status sc_bytes_reserve(sc_bytes *bytes, size_t additional)
{
    size_t needed = 0;
    size_t new_cap = 0;
    uint8_t *next = nullptr;

    if (bytes == nullptr) {
        return sc_status_invalid_argument("sc.bytes.invalid_argument");
    }
    if (bytes->alloc == nullptr) {
        bytes->alloc = sc_allocator_heap();
    }
    if (sc_size_add_overflow(bytes->len, additional, &needed)) {
        return sc_status_no_memory();
    }
    if (needed <= bytes->cap) {
        return sc_status_ok();
    }

    new_cap = bytes->cap == 0 ? 16 : bytes->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }

    next = sc_realloc(bytes->alloc, bytes->ptr, bytes->cap, new_cap, _Alignof(uint8_t));
    if (next == nullptr) {
        return sc_status_no_memory();
    }

    bytes->ptr = next;
    bytes->cap = new_cap;
    return sc_status_ok();
}

sc_status sc_bytes_append(sc_bytes *bytes, sc_buf input)
{
    sc_status status;
    size_t additional = 0;

    if (bytes == nullptr || (input.len > 0 && input.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.bytes.invalid_argument");
    }

    if (sc_size_add_overflow(input.len, 1, &additional)) {
        return sc_status_no_memory();
    }

    status = sc_bytes_reserve(bytes, additional);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    if (input.len > 0) {
        (void)memcpy(&bytes->ptr[bytes->len], input.ptr, input.len);
    }
    bytes->len += input.len;
    bytes->ptr[bytes->len] = 0;
    return sc_status_ok();
}

void sc_bytes_clear(sc_bytes *bytes)
{
    if (bytes == nullptr) {
        return;
    }
    if (bytes->ptr != nullptr) {
        sc_free(bytes->alloc, bytes->ptr, bytes->cap, _Alignof(uint8_t));
    }

    *bytes = (sc_bytes){0};
}

void sc_bytes_secure_clear(sc_bytes *bytes)
{
    if (bytes == nullptr) {
        return;
    }
    if (bytes->ptr != nullptr) {
        sc_secure_zero(bytes->ptr, bytes->len);
    }

    sc_bytes_clear(bytes);
}
