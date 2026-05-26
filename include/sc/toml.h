#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sc/map.h"
#include "sc/result.h"
#include "sc/string.h"

typedef enum sc_toml_type {
    SC_TOML_STRING = 0,
    SC_TOML_INTEGER,
    SC_TOML_BOOL,
    SC_TOML_FLOAT,
    SC_TOML_ARRAY,
    SC_TOML_RAW
} sc_toml_type;

typedef struct sc_toml_value {
    sc_allocator *alloc;
    sc_toml_type type;
    size_t line;
    size_t column;
    sc_string string_value;
    int64_t integer_value;
    double float_value;
    bool bool_value;
} sc_toml_value;

typedef struct sc_toml_source {
    sc_allocator *alloc;
    sc_string source_path;
    sc_map values;
} sc_toml_source;

typedef struct sc_toml_diag {
    sc_string source_path;
    size_t line;
    size_t column;
    const char *error_key;
} sc_toml_diag;

void sc_toml_source_init(sc_toml_source *source, sc_allocator *alloc);
sc_status sc_toml_parse_source(sc_allocator *alloc,
                               sc_str source_path,
                               sc_str input,
                               sc_toml_source *out,
                               sc_toml_diag *diag);
const sc_toml_value *sc_toml_get(const sc_toml_source *source, sc_str dotted_key);
void sc_toml_source_clear(sc_toml_source *source);
void sc_toml_diag_clear(sc_toml_diag *diag);
