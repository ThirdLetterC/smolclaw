#include "sc/vector.h"

#include <string.h>

#include "sc/api.h"

void sc_vec_init(sc_vec *vec, sc_allocator *alloc, size_t item_size)
{
    if (vec == nullptr) {
        return;
    }

    *vec = (sc_vec){
        .ptr = nullptr,
        .len = 0,
        .cap = 0,
        .item_size = item_size,
        .alloc = alloc == nullptr ? sc_allocator_heap() : alloc,
    };
}

sc_status sc_vec_reserve(sc_vec *vec, size_t additional)
{
    size_t needed = 0;
    size_t new_cap = 0;
    size_t old_bytes = 0;
    size_t new_bytes = 0;
    void *next = nullptr;

    if (vec == nullptr || vec->item_size == 0) {
        return sc_status_invalid_argument("sc.vec.invalid_argument");
    }
    if (sc_size_add_overflow(vec->len, additional, &needed)) {
        return sc_status_no_memory();
    }
    if (needed <= vec->cap) {
        return sc_status_ok();
    }

    new_cap = vec->cap == 0 ? 4 : vec->cap;
    while (new_cap < needed) {
        if (new_cap > SIZE_MAX / 2) {
            new_cap = needed;
            break;
        }
        new_cap *= 2;
    }
    if (sc_size_mul_overflow(vec->cap, vec->item_size, &old_bytes) ||
        sc_size_mul_overflow(new_cap, vec->item_size, &new_bytes)) {
        return sc_status_no_memory();
    }

    next = sc_realloc(vec->alloc, vec->ptr, old_bytes, new_bytes, _Alignof(max_align_t));
    if (next == nullptr) {
        return sc_status_no_memory();
    }

    vec->ptr = next;
    vec->cap = new_cap;
    return sc_status_ok();
}

sc_status sc_vec_push(sc_vec *vec, const void *item)
{
    sc_status status;

    if (vec == nullptr || item == nullptr) {
        return sc_status_invalid_argument("sc.vec.invalid_argument");
    }

    status = sc_vec_reserve(vec, 1);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    (void)memcpy((unsigned char *)vec->ptr + (vec->len * vec->item_size), item, vec->item_size);
    vec->len += 1;
    return sc_status_ok();
}

void *sc_vec_at(sc_vec *vec, size_t index)
{
    if (vec == nullptr || index >= vec->len) {
        return nullptr;
    }

    return (unsigned char *)vec->ptr + (index * vec->item_size);
}

const void *sc_vec_at_const(const sc_vec *vec, size_t index)
{
    if (vec == nullptr || index >= vec->len) {
        return nullptr;
    }

    return (const unsigned char *)vec->ptr + (index * vec->item_size);
}

void sc_vec_clear(sc_vec *vec)
{
    size_t bytes = 0;

    if (vec == nullptr) {
        return;
    }

    if (vec->ptr != nullptr && !sc_size_mul_overflow(vec->cap, vec->item_size, &bytes)) {
        sc_free(vec->alloc, vec->ptr, bytes, _Alignof(max_align_t));
    }

    *vec = (sc_vec){0};
}

void sc_ptr_vec_init(sc_ptr_vec *vec, sc_allocator *alloc)
{
    if (vec == nullptr) {
        return;
    }

    sc_vec_init(&vec->inner, alloc, sizeof(void *));
}

sc_status sc_ptr_vec_push(sc_ptr_vec *vec, void *ptr)
{
    if (vec == nullptr) {
        return sc_status_invalid_argument("sc.ptr_vec.invalid_argument");
    }

    return sc_vec_push(&vec->inner, &ptr);
}

void *sc_ptr_vec_at(sc_ptr_vec *vec, size_t index)
{
    void **slot = nullptr;

    if (vec == nullptr) {
        return nullptr;
    }

    slot = sc_vec_at(&vec->inner, index);
    return slot == nullptr ? nullptr : *slot;
}

void sc_ptr_vec_clear(sc_ptr_vec *vec)
{
    if (vec == nullptr) {
        return;
    }

    sc_vec_clear(&vec->inner);
}
