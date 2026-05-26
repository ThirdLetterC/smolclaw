#include "sc/map.h"

#include <stdint.h>
#include <string.h>

#include "sc/api.h"

enum {
    SC_MAP_INITIAL_CAP = 8,
    SC_MAP_MAX_LOAD_PERCENT = 70
};

static uint64_t hash_str(sc_str key);
static bool map_needs_growth(const sc_map *map);
static sc_status map_grow(sc_map *map);
static sc_status map_insert_owned(sc_map *map, sc_string key, void *value);
static size_t map_slot(const sc_map *map, sc_str key, bool *found);

void sc_map_init(sc_map *map, sc_allocator *alloc)
{
    if (map == nullptr) {
        return;
    }

    *map = (sc_map){
        .entries = nullptr,
        .len = 0,
        .cap = 0,
        .alloc = alloc == nullptr ? sc_allocator_heap() : alloc,
    };
}

sc_status sc_map_put(sc_map *map, sc_str key, void *value)
{
    sc_status status;
    sc_string owned = {0};

    if (map == nullptr || (key.len > 0 && key.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.map.invalid_argument");
    }
    if (map->alloc == nullptr) {
        map->alloc = sc_allocator_heap();
    }
    if (map->len == SIZE_MAX) {
        return sc_status_no_memory();
    }
    if (map_needs_growth(map)) {
        status = map_grow(map);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }

    status = sc_string_from_str(map->alloc, key, &owned);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    status = map_insert_owned(map, owned, value);
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&owned);
    }

    return status;
}

void *sc_map_get(const sc_map *map, sc_str key)
{
    bool found = false;
    size_t slot = 0;

    if (map == nullptr || map->cap == 0 || (key.len > 0 && key.ptr == nullptr)) {
        return nullptr;
    }

    slot = map_slot(map, key, &found);
    if (!found) {
        return nullptr;
    }

    return map->entries[slot].value;
}

bool sc_map_contains(const sc_map *map, sc_str key)
{
    bool found = false;

    if (map == nullptr || map->cap == 0 || (key.len > 0 && key.ptr == nullptr)) {
        return false;
    }

    (void)map_slot(map, key, &found);
    return found;
}

void sc_map_clear(sc_map *map)
{
    size_t i = 0;

    if (map == nullptr) {
        return;
    }

    for (i = 0; i < map->cap; i += 1) {
        if (map->entries[i].occupied) {
            sc_string_clear(&map->entries[i].key);
        }
    }

    sc_free(map->alloc, map->entries, map->cap * sizeof(sc_map_entry), _Alignof(sc_map_entry));
    *map = (sc_map){0};
}

static uint64_t hash_str(sc_str key)
{
    uint64_t hash = UINT64_C(1469598103934665603);
    size_t i = 0;

    for (i = 0; i < key.len; i += 1) {
        hash ^= (unsigned char)key.ptr[i];
        hash *= UINT64_C(1099511628211);
    }

    return hash;
}

static bool map_needs_growth(const sc_map *map)
{
    size_t threshold = 0;

    if (map->cap == 0) {
        return true;
    }

    if (map->cap > SIZE_MAX / SC_MAP_MAX_LOAD_PERCENT) {
        threshold = map->cap - ((map->cap / 100) * (100 - SC_MAP_MAX_LOAD_PERCENT));
    } else {
        threshold = (map->cap * SC_MAP_MAX_LOAD_PERCENT) / 100;
    }
    if (threshold == 0) {
        threshold = 1;
    }

    return map->len + 1 > threshold;
}

static sc_status map_grow(sc_map *map)
{
    sc_map next = {0};
    size_t next_cap = map->cap == 0 ? SC_MAP_INITIAL_CAP : map->cap * 2;
    size_t bytes = 0;
    size_t i = 0;

    if (sc_size_mul_overflow(next_cap, sizeof(sc_map_entry), &bytes)) {
        return sc_status_no_memory();
    }

    sc_map_init(&next, map->alloc);
    next.entries = sc_alloc(map->alloc, bytes, _Alignof(sc_map_entry));
    if (next.entries == nullptr) {
        return sc_status_no_memory();
    }
    (void)memset(next.entries, 0, bytes);
    next.cap = next_cap;

    for (i = 0; i < map->cap; i += 1) {
        if (map->entries[i].occupied) {
            sc_status status = map_insert_owned(&next, map->entries[i].key, map->entries[i].value);
            if (!sc_status_is_ok(status)) {
                sc_map_clear(&next);
                return status;
            }
            map->entries[i].key = (sc_string){0};
            map->entries[i].occupied = false;
        }
    }

    sc_free(map->alloc, map->entries, map->cap * sizeof(sc_map_entry), _Alignof(sc_map_entry));
    map->entries = next.entries;
    map->len = next.len;
    map->cap = next.cap;
    next.entries = nullptr;
    return sc_status_ok();
}

static sc_status map_insert_owned(sc_map *map, sc_string key, void *value)
{
    bool found = false;
    size_t slot = map_slot(map, sc_string_as_str(&key), &found);

    if (found) {
        sc_string_clear(&map->entries[slot].key);
        map->entries[slot].key = key;
        map->entries[slot].value = value;
        return sc_status_ok();
    }

    map->entries[slot] = (sc_map_entry){
        .key = key,
        .value = value,
        .occupied = true,
    };
    map->len += 1;
    return sc_status_ok();
}

static size_t map_slot(const sc_map *map, sc_str key, bool *found)
{
    size_t slot = (size_t)(hash_str(key) % map->cap);

    *found = false;
    while (map->entries[slot].occupied) {
        if (sc_str_equal(sc_string_as_str(&map->entries[slot].key), key)) {
            *found = true;
            return slot;
        }
        slot = (slot + 1) % map->cap;
    }

    return slot;
}
