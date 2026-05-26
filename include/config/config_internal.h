#pragma once

#include "sc/config.h"

struct sc_secret_store {
    sc_allocator *alloc;
    const sc_secret_store_vtab *vtab;
    void *impl;
};
