#pragma once

#include <stddef.h>

#include "sc/allocator.h"
#include "sc/result.h"
#include "sc/string.h"

typedef struct sc_json_value sc_json_value;

typedef enum sc_json_type {
    SC_JSON_NULL = 0,
    SC_JSON_BOOL,
    SC_JSON_NUMBER,
    SC_JSON_STRING,
    SC_JSON_ARRAY,
    SC_JSON_OBJECT
} sc_json_type;

typedef struct sc_json_parse_error {
    size_t offset;
    const char *error_key;
} sc_json_parse_error;

sc_status sc_json_parse(sc_allocator *alloc,
                        sc_str input,
                        sc_json_value **out,
                        sc_json_parse_error *error);
sc_status sc_json_serialize(const sc_json_value *value, sc_allocator *alloc, sc_string *out);
sc_status sc_json_clone(const sc_json_value *value, sc_allocator *alloc, sc_json_value **out);
void sc_json_destroy(sc_json_value *value);

sc_json_type sc_json_type_of(const sc_json_value *value);
sc_json_value *sc_json_object_get(const sc_json_value *object, sc_str key);
size_t sc_json_array_len(const sc_json_value *array);
sc_json_value *sc_json_array_get(const sc_json_value *array, size_t index);
bool sc_json_as_bool(const sc_json_value *value, bool *out);
bool sc_json_as_number(const sc_json_value *value, double *out);
bool sc_json_as_str(const sc_json_value *value, sc_str *out);
bool sc_json_is_null(const sc_json_value *value);

sc_status sc_json_object_new(sc_allocator *alloc, sc_json_value **out);
sc_status sc_json_array_new(sc_allocator *alloc, sc_json_value **out);
sc_status sc_json_string_new(sc_allocator *alloc, sc_str value, sc_json_value **out);
sc_status sc_json_number_new(sc_allocator *alloc, double value, sc_json_value **out);
sc_status sc_json_bool_new(sc_allocator *alloc, bool value, sc_json_value **out);
sc_status sc_json_null_new(sc_allocator *alloc, sc_json_value **out);
sc_status sc_json_object_set(sc_json_value *object, sc_str key, sc_json_value *value);
sc_status sc_json_array_append(sc_json_value *array, sc_json_value *value);

sc_status sc_json_schema_object(sc_allocator *alloc, sc_json_value **out);
sc_status sc_json_schema_add_string_property(sc_json_value *schema, sc_str name, bool required);
sc_status sc_json_provider_payload(sc_allocator *alloc,
                                   sc_str model,
                                   sc_str prompt,
                                   sc_json_value **out);
