#pragma once

#include <stddef.h>

#include "sc/api.h"

typedef struct sc_allocator sc_allocator;

typedef struct sc_allocator_vtab {
    void *(*alloc)(sc_allocator *self, size_t size, size_t align);
    void *(*resize)(sc_allocator *self,
                    void *ptr,
                    size_t old_size,
                    size_t new_size,
                    size_t align);
    void (*dealloc)(sc_allocator *self, void *ptr, size_t size, size_t align);
} sc_allocator_vtab;

struct sc_allocator {
    const sc_allocator_vtab *vtab;
};

typedef struct sc_test_allocator {
    sc_allocator base;
    size_t allocations;
    size_t deallocations;
    size_t resize_calls;
    size_t fail_after;
    bool fail_enabled;
} sc_test_allocator;

typedef struct sc_arena_block sc_arena_block;

typedef struct sc_arena {
    sc_allocator base;
    sc_allocator *alloc;
    sc_arena_block *blocks;
    size_t default_block_size;
} sc_arena;

sc_allocator *sc_allocator_heap(void);
void *sc_alloc(sc_allocator *alloc, size_t size, size_t align);
void *sc_realloc(sc_allocator *alloc,
                 void *ptr,
                 size_t old_size,
                 size_t new_size,
                 size_t align);
void sc_free(sc_allocator *alloc, void *ptr, size_t size, size_t align);
void sc_secure_zero(void *ptr, size_t len);

void sc_test_allocator_init(sc_test_allocator *test_alloc);
void sc_test_allocator_fail_after(sc_test_allocator *test_alloc, size_t successful_allocations);
void sc_test_allocator_disable_failures(sc_test_allocator *test_alloc);

void sc_arena_init(sc_arena *arena, sc_allocator *alloc, size_t default_block_size);
sc_allocator *sc_arena_allocator(sc_arena *arena);
void *sc_arena_alloc(sc_arena *arena, size_t size, size_t align);
void sc_arena_reset(sc_arena *arena);
void sc_arena_clear(sc_arena *arena);
