#pragma once

#include <stddef.h>

#include "sc/allocator.h"
#include "sc/result.h"

typedef struct sc_vec {
    void *ptr;
    size_t len;
    size_t cap;
    size_t item_size;
    sc_allocator *alloc;
} sc_vec;

typedef struct sc_ptr_vec {
    sc_vec inner;
} sc_ptr_vec;

void sc_vec_init(sc_vec *vec, sc_allocator *alloc, size_t item_size);
sc_status sc_vec_reserve(sc_vec *vec, size_t additional);
sc_status sc_vec_push(sc_vec *vec, const void *item);
void *sc_vec_at(sc_vec *vec, size_t index);
const void *sc_vec_at_const(const sc_vec *vec, size_t index);
void sc_vec_clear(sc_vec *vec);

void sc_ptr_vec_init(sc_ptr_vec *vec, sc_allocator *alloc);
sc_status sc_ptr_vec_push(sc_ptr_vec *vec, void *ptr);
void *sc_ptr_vec_at(sc_ptr_vec *vec, size_t index);
void sc_ptr_vec_clear(sc_ptr_vec *vec);
