#pragma once

#include <stddef.h>

#include "sc/allocator.h"
#include "sc/buffer.h"
#include "sc/result.h"

typedef struct sc_str {
    const char *ptr;
    size_t len;
} sc_str;

typedef struct sc_string {
    char *ptr;
    size_t len;
    sc_allocator *alloc;
} sc_string;

typedef struct sc_string_builder {
    sc_bytes bytes;
} sc_string_builder;

sc_str sc_str_from_parts(const char *ptr, size_t len);
sc_str sc_str_from_cstr(const char *ptr);
bool sc_str_equal(sc_str left, sc_str right);
bool sc_str_is_valid_utf8(sc_str input);

void sc_string_init(sc_string *string);
sc_status sc_string_from_str(sc_allocator *alloc, sc_str input, sc_string *out);
sc_status sc_string_from_cstr(sc_allocator *alloc, const char *input, sc_string *out);
sc_status sc_string_redacted(sc_allocator *alloc, sc_string *out);
void sc_string_clear(sc_string *string);
void sc_string_secure_clear(sc_string *string);
sc_str sc_string_as_str(const sc_string *string);
bool sc_string_is_empty(const sc_string *string);

void sc_string_builder_init(sc_string_builder *builder, sc_allocator *alloc);
sc_status sc_string_builder_append(sc_string_builder *builder, sc_str input);
sc_status sc_string_builder_append_cstr(sc_string_builder *builder, const char *input);
sc_status sc_string_builder_finish(sc_string_builder *builder, sc_string *out);
void sc_string_builder_clear(sc_string_builder *builder);
