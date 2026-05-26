#pragma once

#include "sc/json.h"
#include "sc/vector.h"

typedef struct json_member {
    sc_string key;
    sc_json_value *value;
} json_member;

struct sc_json_value {
    sc_allocator *alloc;
    sc_json_type type;
    union {
        bool bool_value;
        double number_value;
        sc_string string_value;
        sc_ptr_vec array;
        sc_vec object;
    } as;
};

size_t sc_json_object_len_internal(const sc_json_value *object);
sc_status sc_json_object_entry_internal(const sc_json_value *object,
                                        size_t index,
                                        sc_str *key,
                                        sc_json_value **value);
