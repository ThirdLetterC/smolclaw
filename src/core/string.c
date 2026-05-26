#include "sc/string.h"

#include <string.h>

#include "sc/api.h"
sc_str sc_str_from_parts(const char *ptr, size_t len)
{
    return (sc_str){
        .ptr = ptr,
        .len = len,
    };
}

sc_str sc_str_from_cstr(const char *ptr)
{
    if (ptr == nullptr) {
        return sc_str_from_parts(nullptr, 0);
    }

    return sc_str_from_parts(ptr, strlen(ptr));
}

bool sc_str_equal(sc_str left, sc_str right)
{
    if (left.len != right.len) {
        return false;
    }
    if (left.len == 0) {
        return true;
    }
    if (left.ptr == nullptr || right.ptr == nullptr) {
        return false;
    }

    return memcmp(left.ptr, right.ptr, left.len) == 0;
}

bool sc_str_is_valid_utf8(sc_str input)
{
    size_t i = 0;

    while (i < input.len) {
        unsigned char byte = 0;
        size_t remaining = input.len - i;
        size_t needed = 0;
        uint32_t codepoint = 0;

        if (input.ptr == nullptr) {
            return false;
        }

        byte = (unsigned char)input.ptr[i];
        if (byte <= 0x7Fu) {
            i += 1;
            continue;
        }
        if ((byte & 0xE0u) == 0xC0u) {
            needed = 2;
            codepoint = (uint32_t)(byte & 0x1Fu);
        } else if ((byte & 0xF0u) == 0xE0u) {
            needed = 3;
            codepoint = (uint32_t)(byte & 0x0Fu);
        } else if ((byte & 0xF8u) == 0xF0u) {
            needed = 4;
            codepoint = (uint32_t)(byte & 0x07u);
        } else {
            return false;
        }

        if (remaining < needed) {
            return false;
        }

        for (size_t j = 1; j < needed; j += 1) {
            unsigned char continuation = (unsigned char)input.ptr[i + j];
            if ((continuation & 0xC0u) != 0x80u) {
                return false;
            }
            codepoint = (codepoint << 6) | (uint32_t)(continuation & 0x3Fu);
        }

        if ((needed == 2 && codepoint < 0x80u) ||
            (needed == 3 && codepoint < 0x800u) ||
            (needed == 4 && codepoint < 0x10000u) ||
            codepoint > 0x10FFFFu ||
            (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
            return false;
        }

        i += needed;
    }

    return true;
}

void sc_string_init(sc_string *string)
{
    if (string == nullptr) {
        return;
    }

    *string = (sc_string){0};
}

sc_status sc_string_from_str(sc_allocator *alloc, sc_str input, sc_string *out)
{
    sc_string tmp = {0};

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.string.invalid_argument");
    }
    if (input.len > 0 && input.ptr == nullptr) {
        return sc_status_invalid_argument("sc.string.invalid_argument");
    }
    if (alloc == nullptr) {
        alloc = sc_allocator_heap();
    }
    if (input.len == SIZE_MAX) {
        return sc_status_no_memory();
    }

    tmp.ptr = sc_alloc(alloc, input.len + 1, _Alignof(char));
    if (tmp.ptr == nullptr) {
        return sc_status_no_memory();
    }

    if (input.len > 0) {
        (void)memcpy(tmp.ptr, input.ptr, input.len);
    }
    tmp.ptr[input.len] = '\0';
    tmp.len = input.len;
    tmp.alloc = alloc;

    *out = tmp;
    return sc_status_ok();
}

sc_status sc_string_from_cstr(sc_allocator *alloc, const char *input, sc_string *out)
{
    return sc_string_from_str(alloc, sc_str_from_cstr(input), out);
}

sc_status sc_string_redacted(sc_allocator *alloc, sc_string *out)
{
    return sc_string_from_cstr(alloc, "[REDACTED]", out);
}

void sc_string_clear(sc_string *string)
{
    if (string == nullptr || string->ptr == nullptr) {
        if (string != nullptr) {
            *string = (sc_string){0};
        }
        return;
    }

    sc_free(string->alloc, string->ptr, string->len + 1, _Alignof(char));
    *string = (sc_string){0};
}

void sc_string_secure_clear(sc_string *string)
{
    if (string == nullptr || string->ptr == nullptr) {
        if (string != nullptr) {
            *string = (sc_string){0};
        }
        return;
    }

    sc_secure_zero(string->ptr, string->len);
    sc_string_clear(string);
}

sc_str sc_string_as_str(const sc_string *string)
{
    if (string == nullptr) {
        return sc_str_from_parts(nullptr, 0);
    }

    return sc_str_from_parts(string->ptr, string->len);
}

bool sc_string_is_empty(const sc_string *string)
{
    return string == nullptr || string->len == 0;
}

void sc_string_builder_init(sc_string_builder *builder, sc_allocator *alloc)
{
    if (builder == nullptr) {
        return;
    }

    sc_bytes_init(&builder->bytes, alloc);
}

sc_status sc_string_builder_append(sc_string_builder *builder, sc_str input)
{
    sc_status status;

    if (builder == nullptr) {
        return sc_status_invalid_argument("sc.string_builder.invalid_argument");
    }

    status = sc_bytes_append(&builder->bytes, sc_buf_from_parts(input.ptr, input.len));
    if (!sc_status_is_ok(status)) {
        return status;
    }

    return sc_status_ok();
}

sc_status sc_string_builder_append_cstr(sc_string_builder *builder, const char *input)
{
    return sc_string_builder_append(builder, sc_str_from_cstr(input));
}

sc_status sc_string_builder_finish(sc_string_builder *builder, sc_string *out)
{
    sc_string result = {0};

    if (builder == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.string_builder.invalid_argument");
    }

    result.ptr = (char *)builder->bytes.ptr;
    result.len = builder->bytes.len;
    result.alloc = builder->bytes.alloc;
    builder->bytes = (sc_bytes){0};
    *out = result;
    return sc_status_ok();
}

void sc_string_builder_clear(sc_string_builder *builder)
{
    if (builder == nullptr) {
        return;
    }

    sc_bytes_clear(&builder->bytes);
}
