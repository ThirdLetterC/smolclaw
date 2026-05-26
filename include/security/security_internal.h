#pragma once

#include "sc/security.h"

uint64_t sc_security_hash_init(void);
uint64_t sc_security_hash_bytes(uint64_t hash, sc_str input);
uint64_t sc_security_hash_u64(uint64_t hash, uint64_t value);
uint64_t sc_security_hash_bool(uint64_t hash, bool value);
sc_status sc_security_list_add(sc_vec *list, sc_allocator *alloc, sc_str value);
bool sc_security_list_contains(const sc_vec *list, sc_str value);
void sc_security_string_list_clear(sc_vec *list);
bool sc_security_path_has_prefix(sc_str path, sc_str prefix);
