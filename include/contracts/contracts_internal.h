#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sc/allocator.h"
#include "sc/result.h"
#include "sc/string.h"

typedef struct sc_contract_handle {
    sc_allocator *alloc;
    const void *vtab;
    void *impl;
} sc_contract_handle;

sc_status sc_contract_handle_new(sc_allocator *alloc, const void *vtab, void *impl, size_t size, void **out);
void sc_contract_handle_destroy(sc_contract_handle *handle, size_t size, void (*destroy)(void *impl));
bool sc_contract_common_vtab_valid(size_t struct_size,
                                   uint32_t abi_major,
                                   const char *name,
                                   bool required_present,
                                   bool destroy_present);
