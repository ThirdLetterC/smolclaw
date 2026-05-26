#include "sc/json.h"

#include "json/json_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mjson_write.h"

#include "sc/vector.h"

typedef struct json_parser {
    sc_allocator *alloc;
    sc_str input;
    size_t pos;
    size_t depth;
    sc_json_parse_error *error;
} json_parser;

enum {
    JSON_MAX_DEPTH = 64,
    JSON_MAX_ARRAY_ITEMS = 100000
};

static sc_status parse_value(json_parser *parser, sc_json_value **out);
static sc_status parse_object(json_parser *parser, sc_json_value **out);
static sc_status parse_array(json_parser *parser, sc_json_value **out);
static sc_status parse_string(json_parser *parser, sc_string *out);
static sc_status parse_unicode_escape(json_parser *parser, uint32_t *out);
static sc_status append_utf8_codepoint(sc_string_builder *builder, uint32_t codepoint);
static int json_hex_value(char ch);
static sc_status parse_number(json_parser *parser, sc_json_value **out);
static sc_status parse_literal(json_parser *parser, const char *literal, sc_json_value *value, sc_json_value **out);
static void skip_ws(json_parser *parser);
static bool consume(json_parser *parser, char expected);
static sc_status parser_error(json_parser *parser, const char *key);
static sc_status json_alloc_value(sc_allocator *alloc, sc_json_type type, sc_json_value **out);
static sc_status json_emit_value(const sc_json_value *value, mjson_writer *writer);
static sc_status json_emit_status(int status);

sc_status sc_json_parse(sc_allocator *alloc,
                        sc_str input,
                        sc_json_value **out,
                        sc_json_parse_error *error)
{
    json_parser parser = {0};
    sc_status status;
    sc_json_value *value = nullptr;

    if (out == nullptr || (input.len > 0 && input.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.json.invalid_argument");
    }
    if (error != nullptr) {
        *error = (sc_json_parse_error){0};
    }
    if (!sc_str_is_valid_utf8(input)) {
        if (error != nullptr) {
            error->offset = 0;
            error->error_key = "sc.json.invalid_utf8";
        }
        return sc_status_parse("sc.json.invalid_utf8");
    }

    parser.alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    parser.input = input;
    parser.error = error;

    skip_ws(&parser);
    status = parse_value(&parser, &value);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    skip_ws(&parser);
    if (parser.pos != input.len) {
        sc_json_destroy(value);
        return parser_error(&parser, "sc.json.trailing_input");
    }

    *out = value;
    return sc_status_ok();
}

sc_status sc_json_serialize(const sc_json_value *value, sc_allocator *alloc, sc_string *out)
{
    sc_string result = {0};
    mjson_writer measure = {0};
    mjson_writer writer = {0};
    size_t len = 0;
    sc_status status;

    if (value == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.json.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;

    /*
     * mjson supports a measuring pass. Serialize once to learn the exact size,
     * then serialize again into the owned buffer and reject any drift.
     */
    mjson_writer_init(&measure, nullptr, 0);
    status = json_emit_value(value, &measure);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    len = mjson_writer_length(&measure);
    if (len == SIZE_MAX) {
        return sc_status_no_memory();
    }

    result.ptr = sc_alloc(alloc, len + 1, _Alignof(char));
    if (result.ptr == nullptr) {
        return sc_status_no_memory();
    }
    result.len = len;
    result.alloc = alloc;

    mjson_writer_init(&writer, result.ptr, result.len);
    status = json_emit_value(value, &writer);
    if (!sc_status_is_ok(status) || mjson_writer_truncated(&writer) || mjson_writer_length(&writer) != result.len) {
        sc_string_clear(&result);
        if (sc_status_is_ok(status)) {
            return sc_status_io("sc.json.serialize_size_mismatch");
        }
        return status;
    }
    result.ptr[result.len] = '\0';

    *out = result;
    return sc_status_ok();
}

sc_status sc_json_clone(const sc_json_value *value, sc_allocator *alloc, sc_json_value **out)
{
    sc_json_value *clone = nullptr;
    sc_status status;

    if (value == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.json.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;

    status = json_alloc_value(alloc, value->type, &clone);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    switch (value->type) {
    case SC_JSON_NULL:
        break;
    case SC_JSON_BOOL:
        clone->as.bool_value = value->as.bool_value;
        break;
    case SC_JSON_NUMBER:
        clone->as.number_value = value->as.number_value;
        break;
    case SC_JSON_STRING:
        status = sc_string_from_str(alloc, sc_string_as_str(&value->as.string_value), &clone->as.string_value);
        break;
    case SC_JSON_ARRAY:
        sc_ptr_vec_init(&clone->as.array, alloc);
        for (size_t i = 0; sc_status_is_ok(status) && i < value->as.array.inner.len; i += 1) {
            sc_json_value *child = nullptr;
            status = sc_json_clone(sc_ptr_vec_at((sc_ptr_vec *)&value->as.array, i), alloc, &child);
            if (sc_status_is_ok(status)) {
                status = sc_json_array_append(clone, child);
                if (!sc_status_is_ok(status)) {
                    sc_json_destroy(child);
                }
            }
        }
        break;
    case SC_JSON_OBJECT:
        sc_vec_init(&clone->as.object, alloc, sizeof(json_member));
        for (size_t i = 0; sc_status_is_ok(status) && i < value->as.object.len; i += 1) {
            const json_member *member = sc_vec_at_const(&value->as.object, i);
            sc_json_value *child = nullptr;
            status = sc_json_clone(member->value, alloc, &child);
            if (sc_status_is_ok(status)) {
                status = sc_json_object_set(clone, sc_string_as_str(&member->key), child);
                if (!sc_status_is_ok(status)) {
                    sc_json_destroy(child);
                }
            }
        }
        break;
    }

    if (!sc_status_is_ok(status)) {
        sc_json_destroy(clone);
        return status;
    }

    *out = clone;
    return sc_status_ok();
}

void sc_json_destroy(sc_json_value *value)
{
    if (value == nullptr) {
        return;
    }

    if (value->type == SC_JSON_STRING) {
        sc_string_clear(&value->as.string_value);
    } else if (value->type == SC_JSON_ARRAY) {
        for (size_t i = 0; i < value->as.array.inner.len; i += 1) {
            sc_json_destroy(sc_ptr_vec_at(&value->as.array, i));
        }
        sc_ptr_vec_clear(&value->as.array);
    } else if (value->type == SC_JSON_OBJECT) {
        for (size_t i = 0; i < value->as.object.len; i += 1) {
            json_member *member = sc_vec_at(&value->as.object, i);
            sc_string_clear(&member->key);
            sc_json_destroy(member->value);
        }
        sc_vec_clear(&value->as.object);
    }

    sc_free(value->alloc, value, sizeof(*value), _Alignof(sc_json_value));
}

sc_json_type sc_json_type_of(const sc_json_value *value)
{
    return value == nullptr ? SC_JSON_NULL : value->type;
}

size_t sc_json_object_len_internal(const sc_json_value *object)
{
    if (object == nullptr || object->type != SC_JSON_OBJECT) {
        return 0;
    }
    return object->as.object.len;
}

sc_status sc_json_object_entry_internal(const sc_json_value *object,
                                        size_t index,
                                        sc_str *key,
                                        sc_json_value **value)
{
    const json_member *member = nullptr;

    if (object == nullptr || object->type != SC_JSON_OBJECT || key == nullptr || value == nullptr ||
        index >= object->as.object.len) {
        return sc_status_invalid_argument("sc.json.object_entry_invalid_argument");
    }
    member = sc_vec_at_const(&object->as.object, index);
    if (member == nullptr) {
        return sc_status_invalid_argument("sc.json.object_entry_invalid_argument");
    }
    *key = sc_string_as_str(&member->key);
    *value = member->value;
    return sc_status_ok();
}

sc_json_value *sc_json_object_get(const sc_json_value *object, sc_str key)
{
    if (object == nullptr || object->type != SC_JSON_OBJECT) {
        return nullptr;
    }
    /* Later object members shadow earlier duplicates while preserving parse order for emit. */
    for (size_t i = object->as.object.len; i > 0; i -= 1) {
        const json_member *member = sc_vec_at_const(&object->as.object, i - 1);
        if (sc_str_equal(sc_string_as_str(&member->key), key)) {
            return member->value;
        }
    }
    return nullptr;
}

size_t sc_json_array_len(const sc_json_value *array)
{
    return array != nullptr && array->type == SC_JSON_ARRAY ? array->as.array.inner.len : 0;
}

sc_json_value *sc_json_array_get(const sc_json_value *array, size_t index)
{
    if (array == nullptr || array->type != SC_JSON_ARRAY) {
        return nullptr;
    }
    return sc_ptr_vec_at((sc_ptr_vec *)&array->as.array, index);
}

bool sc_json_as_bool(const sc_json_value *value, bool *out)
{
    if (value == nullptr || value->type != SC_JSON_BOOL || out == nullptr) {
        return false;
    }
    *out = value->as.bool_value;
    return true;
}

bool sc_json_as_number(const sc_json_value *value, double *out)
{
    if (value == nullptr || value->type != SC_JSON_NUMBER || out == nullptr) {
        return false;
    }
    *out = value->as.number_value;
    return true;
}

bool sc_json_as_str(const sc_json_value *value, sc_str *out)
{
    if (value == nullptr || value->type != SC_JSON_STRING || out == nullptr) {
        return false;
    }
    *out = sc_string_as_str(&value->as.string_value);
    return true;
}

bool sc_json_is_null(const sc_json_value *value)
{
    return value != nullptr && value->type == SC_JSON_NULL;
}

sc_status sc_json_object_new(sc_allocator *alloc, sc_json_value **out)
{
    sc_status status = json_alloc_value(alloc, SC_JSON_OBJECT, out);
    if (sc_status_is_ok(status)) {
        sc_vec_init(&(*out)->as.object, (*out)->alloc, sizeof(json_member));
    }
    return status;
}

sc_status sc_json_array_new(sc_allocator *alloc, sc_json_value **out)
{
    sc_status status = json_alloc_value(alloc, SC_JSON_ARRAY, out);
    if (sc_status_is_ok(status)) {
        sc_ptr_vec_init(&(*out)->as.array, (*out)->alloc);
    }
    return status;
}

sc_status sc_json_string_new(sc_allocator *alloc, sc_str value, sc_json_value **out)
{
    sc_status status = json_alloc_value(alloc, SC_JSON_STRING, out);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str((*out)->alloc, value, &(*out)->as.string_value);
        if (!sc_status_is_ok(status)) {
            sc_json_destroy(*out);
            *out = nullptr;
        }
    }
    return status;
}

sc_status sc_json_number_new(sc_allocator *alloc, double value, sc_json_value **out)
{
    sc_status status = json_alloc_value(alloc, SC_JSON_NUMBER, out);
    if (sc_status_is_ok(status)) {
        (*out)->as.number_value = value;
    }
    return status;
}

sc_status sc_json_bool_new(sc_allocator *alloc, bool value, sc_json_value **out)
{
    sc_status status = json_alloc_value(alloc, SC_JSON_BOOL, out);
    if (sc_status_is_ok(status)) {
        (*out)->as.bool_value = value;
    }
    return status;
}

sc_status sc_json_null_new(sc_allocator *alloc, sc_json_value **out)
{
    return json_alloc_value(alloc, SC_JSON_NULL, out);
}

sc_status sc_json_object_set(sc_json_value *object, sc_str key, sc_json_value *value)
{
    json_member member = {0};
    sc_status status;

    if (object == nullptr || object->type != SC_JSON_OBJECT || value == nullptr) {
        return sc_status_invalid_argument("sc.json.invalid_argument");
    }

    status = sc_string_from_str(object->alloc, key, &member.key);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    member.value = value;
    status = sc_vec_push(&object->as.object, &member);
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&member.key);
    }
    return status;
}

sc_status sc_json_array_append(sc_json_value *array, sc_json_value *value)
{
    if (array == nullptr || array->type != SC_JSON_ARRAY || value == nullptr) {
        return sc_status_invalid_argument("sc.json.invalid_argument");
    }
    if (array->as.array.inner.len >= JSON_MAX_ARRAY_ITEMS) {
        return sc_status_parse("sc.json.array_too_large");
    }
    return sc_ptr_vec_push(&array->as.array, value);
}

static sc_status parse_value(json_parser *parser, sc_json_value **out)
{
    skip_ws(parser);
    if (parser->depth > JSON_MAX_DEPTH) {
        return parser_error(parser, "sc.json.too_deep");
    }
    if (parser->pos >= parser->input.len) {
        return parser_error(parser, "sc.json.unexpected_eof");
    }

    switch (parser->input.ptr[parser->pos]) {
    case '{':
        return parse_object(parser, out);
    case '[':
        return parse_array(parser, out);
    case '"': {
        sc_string string = {0};
        sc_status status = parse_string(parser, &string);
        if (!sc_status_is_ok(status)) {
            return status;
        }
        status = json_alloc_value(parser->alloc, SC_JSON_STRING, out);
        if (!sc_status_is_ok(status)) {
            sc_string_clear(&string);
            return status;
        }
        (*out)->as.string_value = string;
        return sc_status_ok();
    }
    case 't': {
        sc_json_value tmp = {.type = SC_JSON_BOOL, .as.bool_value = true};
        return parse_literal(parser, "true", &tmp, out);
    }
    case 'f': {
        sc_json_value tmp = {.type = SC_JSON_BOOL, .as.bool_value = false};
        return parse_literal(parser, "false", &tmp, out);
    }
    case 'n': {
        sc_json_value tmp = {.type = SC_JSON_NULL};
        return parse_literal(parser, "null", &tmp, out);
    }
    default:
        if (parser->input.ptr[parser->pos] == '-' || isdigit((unsigned char)parser->input.ptr[parser->pos])) {
            return parse_number(parser, out);
        }
        return parser_error(parser, "sc.json.unexpected_token");
    }
}

static sc_status parse_object(json_parser *parser, sc_json_value **out)
{
    sc_json_value *object = nullptr;
    sc_status status = sc_json_object_new(parser->alloc, &object);

    if (!sc_status_is_ok(status)) {
        return status;
    }

    parser->depth += 1;
    (void)consume(parser, '{');
    skip_ws(parser);
    if (consume(parser, '}')) {
        parser->depth -= 1;
        *out = object;
        return sc_status_ok();
    }

    while (parser->pos < parser->input.len) {
        sc_string key = {0};
        sc_json_value *value = nullptr;

        if (parser->input.ptr[parser->pos] != '"') {
            sc_json_destroy(object);
            parser->depth -= 1;
            return parser_error(parser, "sc.json.expected_object_key");
        }
        status = parse_string(parser, &key);
        if (sc_status_is_ok(status)) {
            skip_ws(parser);
            if (!consume(parser, ':')) {
                status = parser_error(parser, "sc.json.expected_colon");
            }
        }
        if (sc_status_is_ok(status)) {
            status = parse_value(parser, &value);
        }
        if (sc_status_is_ok(status)) {
            status = sc_json_object_set(object, sc_string_as_str(&key), value);
            value = nullptr;
        }
        sc_string_clear(&key);
        sc_json_destroy(value);
        if (!sc_status_is_ok(status)) {
            sc_json_destroy(object);
            parser->depth -= 1;
            return status;
        }

        skip_ws(parser);
        if (consume(parser, '}')) {
            parser->depth -= 1;
            *out = object;
            return sc_status_ok();
        }
        if (!consume(parser, ',')) {
            sc_json_destroy(object);
            parser->depth -= 1;
            return parser_error(parser, "sc.json.expected_comma");
        }
        skip_ws(parser);
    }

    sc_json_destroy(object);
    parser->depth -= 1;
    return parser_error(parser, "sc.json.unexpected_eof");
}

static sc_status parse_array(json_parser *parser, sc_json_value **out)
{
    sc_json_value *array = nullptr;
    sc_status status = sc_json_array_new(parser->alloc, &array);

    if (!sc_status_is_ok(status)) {
        return status;
    }

    parser->depth += 1;
    (void)consume(parser, '[');
    skip_ws(parser);
    if (consume(parser, ']')) {
        parser->depth -= 1;
        *out = array;
        return sc_status_ok();
    }

    while (parser->pos < parser->input.len) {
        sc_json_value *value = nullptr;

        if (array->as.array.inner.len >= JSON_MAX_ARRAY_ITEMS) {
            sc_json_destroy(array);
            parser->depth -= 1;
            return parser_error(parser, "sc.json.array_too_large");
        }

        status = parse_value(parser, &value);
        if (sc_status_is_ok(status)) {
            status = sc_json_array_append(array, value);
            value = nullptr;
        }
        sc_json_destroy(value);
        if (!sc_status_is_ok(status)) {
            sc_json_destroy(array);
            parser->depth -= 1;
            return status;
        }

        skip_ws(parser);
        if (consume(parser, ']')) {
            parser->depth -= 1;
            *out = array;
            return sc_status_ok();
        }
        if (!consume(parser, ',')) {
            sc_json_destroy(array);
            parser->depth -= 1;
            return parser_error(parser, "sc.json.expected_comma");
        }
        skip_ws(parser);
    }

    sc_json_destroy(array);
    parser->depth -= 1;
    return parser_error(parser, "sc.json.unexpected_eof");
}

static sc_status parse_string(json_parser *parser, sc_string *out)
{
    sc_string_builder builder = {0};

    if (!consume(parser, '"')) {
        return parser_error(parser, "sc.json.expected_string");
    }

    sc_string_builder_init(&builder, parser->alloc);
    while (parser->pos < parser->input.len) {
        char ch = parser->input.ptr[parser->pos++];
        if (ch == '"') {
            return sc_string_builder_finish(&builder, out);
        }
        if ((unsigned char)ch < 0x20u) {
            sc_string_builder_clear(&builder);
            return parser_error(parser, "sc.json.control_in_string");
        }
        if (ch == '\\') {
            if (parser->pos >= parser->input.len) {
                sc_string_builder_clear(&builder);
                return parser_error(parser, "sc.json.bad_escape");
            }
            ch = parser->input.ptr[parser->pos++];
            if (ch == '"' || ch == '\\' || ch == '/') {
                /* use escaped character as-is */
            } else if (ch == 'b') {
                ch = '\b';
            } else if (ch == 'f') {
                ch = '\f';
            } else if (ch == 'n') {
                ch = '\n';
            } else if (ch == 'r') {
                ch = '\r';
            } else if (ch == 't') {
                ch = '\t';
            } else if (ch == 'u') {
                uint32_t codepoint = 0;
                sc_status status = parse_unicode_escape(parser, &codepoint);
                if (sc_status_is_ok(status)) {
                    status = append_utf8_codepoint(&builder, codepoint);
                }
                if (!sc_status_is_ok(status)) {
                    sc_string_builder_clear(&builder);
                    return status;
                }
                continue;
            } else {
                sc_string_builder_clear(&builder);
                return parser_error(parser, "sc.json.unsupported_escape");
            }
        }
        sc_status status = sc_string_builder_append(&builder, sc_str_from_parts(&ch, 1));
        if (!sc_status_is_ok(status)) {
            sc_string_builder_clear(&builder);
            return status;
        }
    }

    sc_string_builder_clear(&builder);
    return parser_error(parser, "sc.json.unexpected_eof");
}

static sc_status parse_unicode_escape(json_parser *parser, uint32_t *out)
{
    uint32_t codepoint = 0;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.json.invalid_argument");
    }
    if (parser->input.len - parser->pos < 4) {
        return parser_error(parser, "sc.json.bad_unicode_escape");
    }
    for (size_t i = 0; i < 4; i += 1) {
        int value = json_hex_value(parser->input.ptr[parser->pos++]);
        if (value < 0) {
            return parser_error(parser, "sc.json.bad_unicode_escape");
        }
        codepoint = (codepoint << 4u) | (uint32_t)value;
    }

    if (codepoint >= 0xD800u && codepoint <= 0xDBFFu) {
        uint32_t low = 0;
        /* JSON encodes non-BMP codepoints as UTF-16 surrogate pairs. */
        if (parser->input.len - parser->pos < 6 ||
            parser->input.ptr[parser->pos] != '\\' ||
            parser->input.ptr[parser->pos + 1] != 'u') {
            return parser_error(parser, "sc.json.bad_unicode_escape");
        }
        parser->pos += 2;
        if (parser->input.len - parser->pos < 4) {
            return parser_error(parser, "sc.json.bad_unicode_escape");
        }
        for (size_t i = 0; i < 4; i += 1) {
            int value = json_hex_value(parser->input.ptr[parser->pos++]);
            if (value < 0) {
                return parser_error(parser, "sc.json.bad_unicode_escape");
            }
            low = (low << 4u) | (uint32_t)value;
        }
        if (low < 0xDC00u || low > 0xDFFFu) {
            return parser_error(parser, "sc.json.bad_unicode_escape");
        }
        codepoint = 0x10000u + (((codepoint - 0xD800u) << 10u) | (low - 0xDC00u));
    } else if (codepoint >= 0xDC00u && codepoint <= 0xDFFFu) {
        return parser_error(parser, "sc.json.bad_unicode_escape");
    }

    *out = codepoint;
    return sc_status_ok();
}

static sc_status append_utf8_codepoint(sc_string_builder *builder, uint32_t codepoint)
{
    char bytes[4] = {0};
    size_t len = 0;

    if (codepoint <= 0x7Fu) {
        bytes[0] = (char)codepoint;
        len = 1;
    } else if (codepoint <= 0x7FFu) {
        bytes[0] = (char)(0xC0u | (codepoint >> 6u));
        bytes[1] = (char)(0x80u | (codepoint & 0x3Fu));
        len = 2;
    } else if (codepoint <= 0xFFFFu) {
        bytes[0] = (char)(0xE0u | (codepoint >> 12u));
        bytes[1] = (char)(0x80u | ((codepoint >> 6u) & 0x3Fu));
        bytes[2] = (char)(0x80u | (codepoint & 0x3Fu));
        len = 3;
    } else if (codepoint <= 0x10FFFFu) {
        bytes[0] = (char)(0xF0u | (codepoint >> 18u));
        bytes[1] = (char)(0x80u | ((codepoint >> 12u) & 0x3Fu));
        bytes[2] = (char)(0x80u | ((codepoint >> 6u) & 0x3Fu));
        bytes[3] = (char)(0x80u | (codepoint & 0x3Fu));
        len = 4;
    } else {
        return sc_status_parse("sc.json.bad_unicode_escape");
    }

    return sc_string_builder_append(builder, sc_str_from_parts(bytes, len));
}

static int json_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return ch - 'a' + 10;
    }
    if (ch >= 'A' && ch <= 'F') {
        return ch - 'A' + 10;
    }
    return -1;
}

static sc_status parse_number(json_parser *parser, sc_json_value **out)
{
    char buffer[128] = {0};
    size_t start = parser->pos;
    size_t len = 0;
    char *end = nullptr;
    double number = 0.0;

    if (parser->input.ptr[parser->pos] == '-') {
        parser->pos += 1;
    }
    while (parser->pos < parser->input.len && isdigit((unsigned char)parser->input.ptr[parser->pos])) {
        parser->pos += 1;
    }
    if (parser->pos < parser->input.len && parser->input.ptr[parser->pos] == '.') {
        parser->pos += 1;
        while (parser->pos < parser->input.len && isdigit((unsigned char)parser->input.ptr[parser->pos])) {
            parser->pos += 1;
        }
    }
    if (parser->pos < parser->input.len &&
        (parser->input.ptr[parser->pos] == 'e' || parser->input.ptr[parser->pos] == 'E')) {
        parser->pos += 1;
        if (parser->pos < parser->input.len &&
            (parser->input.ptr[parser->pos] == '+' || parser->input.ptr[parser->pos] == '-')) {
            parser->pos += 1;
        }
        while (parser->pos < parser->input.len && isdigit((unsigned char)parser->input.ptr[parser->pos])) {
            parser->pos += 1;
        }
    }

    len = parser->pos - start;
    if (len == 0 || len >= sizeof(buffer)) {
        return parser_error(parser, "sc.json.number_too_large");
    }
    (void)memcpy(buffer, &parser->input.ptr[start], len);
    number = strtod(buffer, &end);
    if (end == buffer || !isfinite(number)) {
        return parser_error(parser, "sc.json.invalid_number");
    }

    return sc_json_number_new(parser->alloc, number, out);
}

static sc_status parse_literal(json_parser *parser, const char *literal, sc_json_value *value, sc_json_value **out)
{
    size_t len = strlen(literal);
    sc_status status;

    if (parser->input.len - parser->pos < len ||
        memcmp(&parser->input.ptr[parser->pos], literal, len) != 0) {
        return parser_error(parser, "sc.json.invalid_literal");
    }

    parser->pos += len;
    status = json_alloc_value(parser->alloc, value->type, out);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (value->type == SC_JSON_BOOL) {
        (*out)->as.bool_value = value->as.bool_value;
    }
    return sc_status_ok();
}

static void skip_ws(json_parser *parser)
{
    while (parser->pos < parser->input.len &&
           (parser->input.ptr[parser->pos] == ' ' ||
            parser->input.ptr[parser->pos] == '\n' ||
            parser->input.ptr[parser->pos] == '\r' ||
            parser->input.ptr[parser->pos] == '\t')) {
        parser->pos += 1;
    }
}

static bool consume(json_parser *parser, char expected)
{
    if (parser->pos < parser->input.len && parser->input.ptr[parser->pos] == expected) {
        parser->pos += 1;
        return true;
    }
    return false;
}

static sc_status parser_error(json_parser *parser, const char *key)
{
    if (parser->error != nullptr) {
        parser->error->offset = parser->pos;
        parser->error->error_key = key;
    }
    return sc_status_json(key);
}

static sc_status json_alloc_value(sc_allocator *alloc, sc_json_type type, sc_json_value **out)
{
    sc_json_value *value = nullptr;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.json.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    value = sc_alloc(alloc, sizeof(*value), _Alignof(sc_json_value));
    if (value == nullptr) {
        return sc_status_no_memory();
    }
    *value = (sc_json_value){.alloc = alloc, .type = type};
    *out = value;
    return sc_status_ok();
}

static sc_status json_emit_value(const sc_json_value *value, mjson_writer *writer)
{
    sc_status status = sc_status_ok();

    if (value == nullptr || writer == nullptr) {
        return sc_status_invalid_argument("sc.json.invalid_argument");
    }
    switch (value->type) {
    case SC_JSON_NULL:
        return json_emit_status(mjson_write_null(writer));
    case SC_JSON_BOOL:
        return json_emit_status(mjson_write_bool(writer, value->as.bool_value));
    case SC_JSON_NUMBER:
        return json_emit_status(mjson_write_number_double(writer, value->as.number_value));
    case SC_JSON_STRING: {
        sc_str input = sc_string_as_str(&value->as.string_value);
        return json_emit_status(mjson_write_string(writer, input.ptr, input.len));
    }
    case SC_JSON_ARRAY:
        status = json_emit_status(mjson_write_cstr(writer, "["));
        for (size_t i = 0; sc_status_is_ok(status) && i < value->as.array.inner.len; i += 1) {
            if (i > 0) {
                status = json_emit_status(mjson_write_cstr(writer, ","));
            }
            if (sc_status_is_ok(status)) {
                status = json_emit_value(sc_ptr_vec_at((sc_ptr_vec *)&value->as.array, i), writer);
            }
        }
        if (sc_status_is_ok(status)) {
            status = json_emit_status(mjson_write_cstr(writer, "]"));
        }
        return status;
    case SC_JSON_OBJECT:
        status = json_emit_status(mjson_write_cstr(writer, "{"));
        for (size_t i = 0; sc_status_is_ok(status) && i < value->as.object.len; i += 1) {
            const json_member *member = sc_vec_at_const(&value->as.object, i);
            sc_str key = sc_string_as_str(&member->key);
            if (i > 0) {
                status = json_emit_status(mjson_write_cstr(writer, ","));
            }
            if (sc_status_is_ok(status)) {
                status = json_emit_status(mjson_write_string(writer, key.ptr, key.len));
            }
            if (sc_status_is_ok(status)) {
                status = json_emit_status(mjson_write_cstr(writer, ":"));
            }
            if (sc_status_is_ok(status)) {
                status = json_emit_value(member->value, writer);
            }
        }
        if (sc_status_is_ok(status)) {
            status = json_emit_status(mjson_write_cstr(writer, "}"));
        }
        return status;
    }
    return sc_status_invalid_argument("sc.json.invalid_type");
}

static sc_status json_emit_status(int status)
{
    switch (status) {
    case MJSON_WRITE_OK:
        return sc_status_ok();
    case MJSON_WRITE_INVALID:
        return sc_status_invalid_argument("sc.json.serialize_invalid_argument");
    case MJSON_WRITE_OVERFLOW:
        return sc_status_no_memory();
    case MJSON_WRITE_FORMAT:
        return sc_status_io("sc.json.serialize_number_failed");
    default:
        return sc_status_io("sc.json.serialize_failed");
    }
}
