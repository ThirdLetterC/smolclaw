#pragma once

#include <stddef.h>

#include "sc/result.h"
#include "sc/string.h"

typedef struct sc_map_entry {
    sc_string key;
    void *value;
    bool occupied;
} sc_map_entry;

typedef struct sc_map {
    sc_map_entry *entries;
    size_t len;
    size_t cap;
    sc_allocator *alloc;
} sc_map;

void sc_map_init(sc_map *map, sc_allocator *alloc);
sc_status sc_map_put(sc_map *map, sc_str key, void *value);
void *sc_map_get(const sc_map *map, sc_str key);
bool sc_map_contains(const sc_map *map, sc_str key);
void sc_map_clear(sc_map *map);
