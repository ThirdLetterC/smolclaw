#include "sc/allocator.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#ifdef SC_HAVE_MIMALLOC
#include <mimalloc.h>
#endif
#ifdef SC_HAVE_JEMALLOC
#include <jemalloc/jemalloc.h>
#endif

constexpr size_t SC_ARENA_DEFAULT_BLOCK_SIZE = 4'096;

struct sc_arena_block {
    struct sc_arena_block *next;
    size_t used;
    size_t cap;
    max_align_t align;
    unsigned char data[];
};

static void *heap_alloc(sc_allocator *self, size_t size, size_t align);
static void *heap_resize(sc_allocator *self,
                         void *ptr,
                         size_t old_size,
                         size_t new_size,
                         size_t align);
static void heap_dealloc(sc_allocator *self, void *ptr, size_t size, size_t align);
static void *test_alloc(sc_allocator *self, size_t size, size_t align);
static void *test_resize(sc_allocator *self,
                         void *ptr,
                         size_t old_size,
                         size_t new_size,
                         size_t align);
static void test_dealloc(sc_allocator *self, void *ptr, size_t size, size_t align);
static void *arena_allocator_alloc(sc_allocator *self, size_t size, size_t align);
static void *arena_allocator_resize(sc_allocator *self,
                                    void *ptr,
                                    size_t old_size,
                                    size_t new_size,
                                    size_t align);
static void arena_allocator_dealloc(sc_allocator *self, void *ptr, size_t size, size_t align);
static size_t align_offset(const unsigned char *base, size_t value, size_t align);
static bool should_fail(const sc_test_allocator *state);
#if defined(SC_HAVE_MIMALLOC) || defined(SC_HAVE_JEMALLOC)
static bool heap_needs_explicit_alignment(size_t align);
#endif
#ifdef SC_HAVE_JEMALLOC
static int heap_jemalloc_flags(size_t align, bool zeroed);
#endif

static const sc_allocator_vtab heap_vtab = {
    .alloc = heap_alloc,
    .resize = heap_resize,
    .dealloc = heap_dealloc,
};

static sc_allocator heap_allocator = {
    .vtab = &heap_vtab,
};

static const sc_allocator_vtab test_vtab = {
    .alloc = test_alloc,
    .resize = test_resize,
    .dealloc = test_dealloc,
};

static const sc_allocator_vtab arena_vtab = {
    .alloc = arena_allocator_alloc,
    .resize = arena_allocator_resize,
    .dealloc = arena_allocator_dealloc,
};

sc_allocator *sc_allocator_heap(void)
{
    return &heap_allocator;
}

void *sc_alloc(sc_allocator *alloc, size_t size, size_t align)
{
    if (alloc == nullptr) {
        alloc = sc_allocator_heap();
    }
    if (alloc->vtab == nullptr || alloc->vtab->alloc == nullptr) {
        return nullptr;
    }

    return alloc->vtab->alloc(alloc, size, align);
}

void *sc_realloc(sc_allocator *alloc,
                 void *ptr,
                 size_t old_size,
                 size_t new_size,
                 size_t align)
{
    if (alloc == nullptr) {
        alloc = sc_allocator_heap();
    }
    if (alloc->vtab == nullptr || alloc->vtab->resize == nullptr) {
        return nullptr;
    }

    return alloc->vtab->resize(alloc, ptr, old_size, new_size, align);
}

void sc_free(sc_allocator *alloc, void *ptr, size_t size, size_t align)
{
    if (ptr == nullptr) {
        return;
    }
    if (alloc == nullptr) {
        alloc = sc_allocator_heap();
    }
    if (alloc->vtab == nullptr || alloc->vtab->dealloc == nullptr) {
        return;
    }

    alloc->vtab->dealloc(alloc, ptr, size, align);
}

void sc_secure_zero(void *ptr, size_t len)
{
    volatile unsigned char *cursor = (volatile unsigned char *)ptr;

    if (ptr == nullptr) {
        return;
    }

    while (len > 0) {
        *cursor = 0;
        cursor += 1;
        len -= 1;
    }
}

void sc_test_allocator_init(sc_test_allocator *test_alloc)
{
    if (test_alloc == nullptr) {
        return;
    }

    *test_alloc = (sc_test_allocator){
        .base = {.vtab = &test_vtab},
        .allocations = 0,
        .deallocations = 0,
        .resize_calls = 0,
        .fail_after = 0,
        .fail_enabled = false,
    };
}

void sc_test_allocator_fail_after(sc_test_allocator *test_alloc, size_t successful_allocations)
{
    if (test_alloc == nullptr) {
        return;
    }

    test_alloc->fail_after = successful_allocations;
    test_alloc->fail_enabled = true;
}

void sc_test_allocator_disable_failures(sc_test_allocator *test_alloc)
{
    if (test_alloc == nullptr) {
        return;
    }

    test_alloc->fail_enabled = false;
}

void sc_arena_init(sc_arena *arena, sc_allocator *alloc, size_t default_block_size)
{
    if (arena == nullptr) {
        return;
    }

    if (default_block_size == 0) {
        default_block_size = SC_ARENA_DEFAULT_BLOCK_SIZE;
    }

    *arena = (sc_arena){
        .base = {.vtab = &arena_vtab},
        .alloc = alloc == nullptr ? sc_allocator_heap() : alloc,
        .blocks = nullptr,
        .default_block_size = default_block_size,
    };
}

sc_allocator *sc_arena_allocator(sc_arena *arena)
{
    if (arena == nullptr) {
        return nullptr;
    }

    arena->base.vtab = &arena_vtab;
    return &arena->base;
}

void *sc_arena_alloc(sc_arena *arena, size_t size, size_t align)
{
    sc_arena_block *block = nullptr;
    size_t total = 0;
    size_t block_cap = 0;
    size_t minimum_cap = 0;
    size_t align_slop = 0;

    if (arena == nullptr || size == 0) {
        return nullptr;
    }
    if (align == 0) {
        align = sizeof(void *);
    }

    for (block = arena->blocks; block != nullptr; block = block->next) {
        size_t start = align_offset(block->data, block->used, align);
        if (start <= block->cap && size <= block->cap - start) {
            block->used = start + size;
            return &block->data[start];
        }
    }

    align_slop = align > 0 ? align - 1 : 0;
    if (sc_size_add_overflow(size, align_slop, &minimum_cap)) {
        return nullptr;
    }
    block_cap = arena->default_block_size;
    if (block_cap < minimum_cap) {
        block_cap = minimum_cap;
    }
    if (sc_size_add_overflow(sizeof(sc_arena_block), block_cap, &total)) {
        return nullptr;
    }

    block = sc_alloc(arena->alloc, total, _Alignof(sc_arena_block));
    if (block == nullptr) {
        return nullptr;
    }

    block->next = arena->blocks;
    block->cap = block_cap;
    size_t start = align_offset(block->data, 0, align);
    if (start > block->cap || size > block->cap - start) {
        sc_free(arena->alloc, block, total, _Alignof(sc_arena_block));
        return nullptr;
    }
    block->used = start + size;
    arena->blocks = block;
    return &block->data[start];
}

// cppcheck-suppress constParameterPointer
void sc_arena_reset(sc_arena *arena)
{
    sc_arena_block *block = nullptr;

    if (arena == nullptr) {
        return;
    }

    for (block = arena->blocks; block != nullptr; block = block->next) {
        block->used = 0;
    }
}

void sc_arena_clear(sc_arena *arena)
{
    sc_arena_block *block = nullptr;
    sc_arena_block *next;

    if (arena == nullptr) {
        return;
    }

    block = arena->blocks;
    while (block != nullptr) {
        size_t total = sizeof(sc_arena_block) + block->cap;
        next = block->next;
        sc_free(arena->alloc, block, total, _Alignof(sc_arena_block));
        block = next;
    }

    arena->blocks = nullptr;
}

static void *heap_alloc(sc_allocator *self, size_t size, size_t align)
{
    (void)self;

    if (size == 0) {
        size = 1;
    }

#ifdef SC_HAVE_MIMALLOC
    if (heap_needs_explicit_alignment(align)) {
        return mi_zalloc_aligned(size, align);
    }

    return mi_zalloc(size);
#elif defined(SC_HAVE_JEMALLOC)
    return mallocx(size, heap_jemalloc_flags(align, true));
#else
    (void)align;
    return calloc(1, size);
#endif
}

static void *heap_resize(sc_allocator *self,
                         void *ptr,
                         size_t old_size,
                         size_t new_size,
                         size_t align)
{
    (void)self;
    (void)old_size;

    if (new_size == 0) {
        new_size = 1;
    }

#ifdef SC_HAVE_MIMALLOC
    if (heap_needs_explicit_alignment(align)) {
        return mi_realloc_aligned(ptr, new_size, align);
    }

    return mi_realloc(ptr, new_size);
#elif defined(SC_HAVE_JEMALLOC)
    if (ptr == nullptr) {
        return mallocx(new_size, heap_jemalloc_flags(align, false));
    }

    return rallocx(ptr, new_size, heap_jemalloc_flags(align, false));
#else
    (void)align;
    return realloc(ptr, new_size);
#endif
}

static void heap_dealloc(sc_allocator *self, void *ptr, size_t size, size_t align)
{
    (void)self;
    (void)size;
    (void)align;
#ifdef SC_HAVE_MIMALLOC
    mi_free(ptr);
#elif defined(SC_HAVE_JEMALLOC)
    dallocx(ptr, 0);
#else
    free(ptr);
#endif
}

static void *test_alloc(sc_allocator *self, size_t size, size_t align)
{
    sc_test_allocator *state = (sc_test_allocator *)self;

    if (should_fail(state)) {
        return nullptr;
    }

    state->allocations += 1;
    return heap_alloc(sc_allocator_heap(), size, align);
}

static void *test_resize(sc_allocator *self,
                         void *ptr,
                         size_t old_size,
                         size_t new_size,
                         size_t align)
{
    sc_test_allocator *state = (sc_test_allocator *)self;
    void *next = nullptr;

    if (should_fail(state)) {
        return nullptr;
    }

    state->resize_calls += 1;
    next = heap_resize(sc_allocator_heap(), ptr, old_size, new_size, align);
    if (ptr == nullptr && next != nullptr) {
        state->allocations += 1;
    }

    return next;
}

static void test_dealloc(sc_allocator *self, void *ptr, size_t size, size_t align)
{
    sc_test_allocator *state = (sc_test_allocator *)self;

    state->deallocations += 1;
    heap_dealloc(sc_allocator_heap(), ptr, size, align);
}

static void *arena_allocator_alloc(sc_allocator *self, size_t size, size_t align)
{
    return sc_arena_alloc((sc_arena *)self, size, align);
}

static void *arena_allocator_resize(sc_allocator *self,
                                    void *ptr,
                                    size_t old_size,
                                    size_t new_size,
                                    size_t align)
{
    void *next = nullptr;

    if (new_size == 0) {
        new_size = 1;
    }
    if (ptr != nullptr && new_size <= old_size) {
        return ptr;
    }

    next = sc_arena_alloc((sc_arena *)self, new_size, align);
    if (next != nullptr && ptr != nullptr && old_size > 0) {
        (void)memcpy(next, ptr, old_size < new_size ? old_size : new_size);
    }
    return next;
}

static void arena_allocator_dealloc(sc_allocator *self, void *ptr, size_t size, size_t align)
{
    (void)self;
    (void)ptr;
    (void)size;
    (void)align;
}

static size_t align_offset(const unsigned char *base, size_t value, size_t align)
{
    size_t remainder = 0;
    uintptr_t address = 0;

    if (align == 0) {
        return value;
    }

    address = (uintptr_t)base + value;
    remainder = (size_t)(address % align);
    if (remainder == 0) {
        return value;
    }

    return value + (align - remainder);
}

static bool should_fail(const sc_test_allocator *state)
{
    if (!state->fail_enabled) {
        return false;
    }

    return state->allocations + state->resize_calls >= state->fail_after;
}

#if defined(SC_HAVE_MIMALLOC) || defined(SC_HAVE_JEMALLOC)
static bool heap_needs_explicit_alignment(size_t align)
{
    return align > _Alignof(max_align_t);
}
#endif

#ifdef SC_HAVE_JEMALLOC
static int heap_jemalloc_flags(size_t align, bool zeroed)
{
    int flags = zeroed ? MALLOCX_ZERO : 0;

    if (heap_needs_explicit_alignment(align)) {
        flags |= MALLOCX_ALIGN(align);
    }

    return flags;
}
#endif
