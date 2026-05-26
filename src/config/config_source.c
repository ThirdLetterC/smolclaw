#include "sc/toml.h"

#include <ctype.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef SC_HAVE_TOML
#include "toml/toml.h"
#endif

static sc_str trim(sc_str input);
#ifdef SC_HAVE_TOML
static sc_status parse_with_vendored_toml(sc_allocator *alloc,
                                          sc_str source_path,
                                          sc_str input,
                                          sc_toml_source *out,
                                          sc_toml_diag *diag);
static sc_status flatten_toml_table(sc_toml_source *source,
                                    sc_str input,
                                    toml_datum_t table,
                                    sc_str prefix,
                                    sc_toml_diag *diag);
static sc_status flatten_toml_array_tables(sc_toml_source *source,
                                           sc_str input,
                                           toml_datum_t array,
                                           sc_str prefix,
                                           sc_toml_diag *diag);
static sc_status add_vendored_toml_value(sc_toml_source *source,
                                         sc_str input,
                                         sc_str dotted_key,
                                         sc_str leaf_key,
                                         toml_datum_t datum,
                                         sc_toml_diag *diag);
static sc_status serialize_toml_datum(sc_allocator *alloc, toml_datum_t datum, sc_string *out);
static sc_status append_serialized_toml_datum(sc_string_builder *builder, toml_datum_t datum);
static sc_status append_serialized_string(sc_string_builder *builder, sc_str input);
static bool find_assignment_position(sc_str input, sc_str key, size_t *line, size_t *column);
static bool find_section_assignment_position(sc_str input,
                                             sc_str section,
                                             sc_str key,
                                             size_t *line,
                                             size_t *column);
static void find_value_position(sc_str input,
                                sc_str dotted_key,
                                sc_str leaf_key,
                                size_t *line,
                                size_t *column);
static size_t parse_toml_error_line(const char *message);
#else
static sc_status parse_line(sc_toml_source *source,
                            sc_str section,
                            sc_str line,
                            size_t line_no,
                            sc_toml_diag *diag);
#endif
static sc_status set_diag(sc_toml_diag *diag,
                          sc_allocator *alloc,
                          sc_str source_path,
                          size_t line,
                          size_t column,
                          const char *key);
static sc_status join_key(sc_allocator *alloc, sc_str section, sc_str key, sc_string *out);
static void toml_value_destroy(sc_allocator *alloc, void *ptr);

void sc_toml_source_init(sc_toml_source *source, sc_allocator *alloc)
{
    if (source == nullptr) {
        return;
    }

    *source = (sc_toml_source){.alloc = alloc == nullptr ? sc_allocator_heap() : alloc};
    sc_map_init(&source->values, source->alloc);
}

sc_status sc_toml_parse_source(sc_allocator *alloc,
                               sc_str source_path,
                               sc_str input,
                               sc_toml_source *out,
                               sc_toml_diag *diag)
{
#ifdef SC_HAVE_TOML
    return parse_with_vendored_toml(alloc, source_path, input, out, diag);
#else
    sc_toml_source tmp = {0};
    sc_string section = {0};
    size_t line_no = 1;
    size_t start = 0;
    sc_status status;

    if (out == nullptr || (input.len > 0 && input.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.toml.invalid_argument");
    }
    if (diag != nullptr) {
        *diag = (sc_toml_diag){0};
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_toml_source_init(&tmp, alloc);
    status = sc_string_from_str(alloc, source_path, &tmp.source_path);
    if (!sc_status_is_ok(status)) {
        sc_toml_source_clear(&tmp);
        return status;
    }
    sc_string_init(&section);

    for (size_t i = 0; i <= input.len; i += 1) {
        if (i == input.len || input.ptr[i] == '\n') {
            sc_str raw = sc_str_from_parts(&input.ptr[start], i - start);
            sc_str line = trim(raw);
            if (line.len > 0 && line.ptr[0] != '#') {
                if (line.ptr[0] == '[') {
                    if (line.len < 3 || line.ptr[line.len - 1] != ']') {
                        status = set_diag(diag, alloc, source_path, line_no, 1, "sc.toml.invalid_section");
                    } else {
                        sc_string_clear(&section);
                        status = sc_string_from_str(alloc,
                                                    trim(sc_str_from_parts(&line.ptr[1], line.len - 2)),
                                                    &section);
                    }
                } else {
                    status = parse_line(&tmp, sc_string_as_str(&section), line, line_no, diag);
                }
                if (!sc_status_is_ok(status)) {
                    sc_string_clear(&section);
                    sc_toml_source_clear(&tmp);
                    return status;
                }
            }
            start = i + 1;
            line_no += 1;
        }
    }

    sc_string_clear(&section);
    *out = tmp;
    return sc_status_ok();
#endif
}

const sc_toml_value *sc_toml_get(const sc_toml_source *source, sc_str dotted_key)
{
    return source == nullptr ? nullptr : sc_map_get(&source->values, dotted_key);
}

void sc_toml_source_clear(sc_toml_source *source)
{
    if (source == nullptr) {
        return;
    }

    for (size_t i = 0; i < source->values.cap; i += 1) {
        if (source->values.entries != nullptr && source->values.entries[i].occupied) {
            toml_value_destroy(source->alloc, source->values.entries[i].value);
        }
    }
    sc_map_clear(&source->values);
    sc_string_clear(&source->source_path);
    *source = (sc_toml_source){0};
}

void sc_toml_diag_clear(sc_toml_diag *diag)
{
    if (diag == nullptr) {
        return;
    }
    sc_string_clear(&diag->source_path);
    *diag = (sc_toml_diag){0};
}

static sc_str trim(sc_str input)
{
    size_t start = 0;
    size_t end = input.len;

    while (start < end && isspace((unsigned char)input.ptr[start])) {
        start += 1;
    }
    while (end > start && isspace((unsigned char)input.ptr[end - 1])) {
        end -= 1;
    }
    return sc_str_from_parts(&input.ptr[start], end - start);
}

#ifdef SC_HAVE_TOML
static sc_status parse_with_vendored_toml(sc_allocator *alloc,
                                          sc_str source_path,
                                          sc_str input,
                                          sc_toml_source *out,
                                          sc_toml_diag *diag)
{
    sc_toml_source tmp = {0};
    char *body = nullptr;
    toml_result_t parsed = {0};
    sc_status status;

    if (out == nullptr || (input.len > 0 && input.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.toml.invalid_argument");
    }
    if (input.len > (size_t)INT_MAX) {
        return sc_status_invalid_argument("sc.toml.input_too_large");
    }
    if (diag != nullptr) {
        *diag = (sc_toml_diag){0};
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_toml_source_init(&tmp, alloc);
    status = sc_string_from_str(alloc, source_path, &tmp.source_path);
    if (!sc_status_is_ok(status)) {
        sc_toml_source_clear(&tmp);
        return status;
    }

    body = sc_alloc(alloc, input.len + 1, _Alignof(char));
    if (body == nullptr) {
        sc_toml_source_clear(&tmp);
        return sc_status_no_memory();
    }
    if (input.len > 0) {
        (void)memcpy(body, input.ptr, input.len);
    }
    body[input.len] = '\0';

    parsed = toml_parse(body, (int)input.len);
    if (!parsed.ok) {
        size_t line = parse_toml_error_line(parsed.errmsg);
        sc_free(alloc, body, input.len + 1, _Alignof(char));
        toml_free(parsed);
        sc_toml_source_clear(&tmp);
        return set_diag(diag,
                        alloc,
                        source_path,
                        line == 0 ? 1 : line,
                        1,
                        "sc.toml.parse_error");
    }

    status = flatten_toml_table(&tmp, input, parsed.toptab, sc_str_from_cstr(""), diag);
    toml_free(parsed);
    sc_free(alloc, body, input.len + 1, _Alignof(char));
    if (!sc_status_is_ok(status)) {
        sc_toml_source_clear(&tmp);
        return status;
    }

    *out = tmp;
    return sc_status_ok();
}

static sc_status flatten_toml_table(sc_toml_source *source,
                                    sc_str input,
                                    toml_datum_t table,
                                    sc_str prefix,
                                    sc_toml_diag *diag)
{
    if (table.type != TOML_TABLE) {
        return set_diag(diag,
                        source->alloc,
                        sc_string_as_str(&source->source_path),
                        1,
                        1,
                        "sc.toml.expected_table");
    }

    for (int32_t i = 0; i < table.u.tab.size; i += 1) {
        sc_str key = sc_str_from_parts(table.u.tab.key[i], (size_t)table.u.tab.len[i]);
        toml_datum_t datum = table.u.tab.value[i];
        sc_string dotted = {0};
        sc_status status = join_key(source->alloc, prefix, key, &dotted);
        if (!sc_status_is_ok(status)) {
            return status;
        }

        if (datum.type == TOML_TABLE) {
            status = flatten_toml_table(source, input, datum, sc_string_as_str(&dotted), diag);
        } else if (datum.type == TOML_ARRAY && datum.u.arr.size > 0 && datum.u.arr.elem[0].type == TOML_TABLE) {
            status = flatten_toml_array_tables(source, input, datum, sc_string_as_str(&dotted), diag);
        } else {
            status = add_vendored_toml_value(source, input, sc_string_as_str(&dotted), key, datum, diag);
        }
        sc_string_clear(&dotted);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }

    return sc_status_ok();
}

static sc_status flatten_toml_array_tables(sc_toml_source *source,
                                           sc_str input,
                                           toml_datum_t array,
                                           sc_str prefix,
                                           sc_toml_diag *diag)
{
    sc_status status = sc_status_ok();

    for (int32_t i = 0; sc_status_is_ok(status) && i < array.u.arr.size; i += 1) {
        char index_text[32] = {0};
        int written = snprintf(index_text, sizeof(index_text), "%d", i);
        sc_string indexed = {0};
        if (written < 0 || (size_t)written >= sizeof(index_text)) {
            return sc_status_no_memory();
        }
        status = join_key(source->alloc,
                          prefix,
                          sc_str_from_parts(index_text, (size_t)written),
                          &indexed);
        if (sc_status_is_ok(status)) {
            if (array.u.arr.elem[i].type != TOML_TABLE) {
                status = set_diag(diag,
                                  source->alloc,
                                  sc_string_as_str(&source->source_path),
                                  1,
                                  1,
                                  "sc.toml.mixed_array_table");
            } else {
                status = flatten_toml_table(source, input, array.u.arr.elem[i], sc_string_as_str(&indexed), diag);
            }
        }
        sc_string_clear(&indexed);
    }

    return status;
}

static sc_status add_vendored_toml_value(sc_toml_source *source,
                                         sc_str input,
                                         sc_str dotted_key,
                                         sc_str leaf_key,
                                         toml_datum_t datum,
                                         sc_toml_diag *diag)
{
    sc_toml_value *value = nullptr;
    sc_status status = sc_status_ok();
    size_t line = 1;
    size_t column = 1;

    find_value_position(input, dotted_key, leaf_key, &line, &column);
    if (datum.type != TOML_STRING && datum.type != TOML_INT64 && datum.type != TOML_BOOLEAN &&
        datum.type != TOML_FP64 && datum.type != TOML_ARRAY && datum.type != TOML_DATE &&
        datum.type != TOML_TIME && datum.type != TOML_DATETIME && datum.type != TOML_DATETIMETZ) {
        return set_diag(diag,
                        source->alloc,
                        sc_string_as_str(&source->source_path),
                        line,
                        column,
                        "sc.toml.unsupported_value");
    }

    value = sc_alloc(source->alloc, sizeof(*value), _Alignof(sc_toml_value));
    if (value == nullptr) {
        return sc_status_no_memory();
    }
    *value = (sc_toml_value){.alloc = source->alloc, .line = line, .column = column};

    if (datum.type == TOML_STRING) {
        value->type = SC_TOML_STRING;
        status = sc_string_from_str(source->alloc,
                                    sc_str_from_parts(datum.u.str.ptr, (size_t)datum.u.str.len),
                                    &value->string_value);
    } else if (datum.type == TOML_INT64) {
        value->type = SC_TOML_INTEGER;
        value->integer_value = datum.u.int64;
    } else if (datum.type == TOML_BOOLEAN) {
        value->type = SC_TOML_BOOL;
        value->bool_value = datum.u.boolean;
    } else if (datum.type == TOML_FP64) {
        char buffer[64] = {0};
        int written = snprintf(buffer, sizeof(buffer), "%.17g", datum.u.fp64);
        if (written < 0 || (size_t)written >= sizeof(buffer)) {
            status = sc_status_no_memory();
        } else {
            value->type = SC_TOML_FLOAT;
            value->float_value = datum.u.fp64;
            status = sc_string_from_str(source->alloc, sc_str_from_parts(buffer, (size_t)written), &value->string_value);
        }
    } else {
        value->type = datum.type == TOML_ARRAY ? SC_TOML_ARRAY : SC_TOML_RAW;
        status = serialize_toml_datum(source->alloc, datum, &value->string_value);
    }

    if (sc_status_is_ok(status)) {
        status = sc_map_put(&source->values, dotted_key, value);
    }
    if (!sc_status_is_ok(status)) {
        toml_value_destroy(source->alloc, value);
    }
    return status;
}

static sc_status serialize_toml_datum(sc_allocator *alloc, toml_datum_t datum, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    status = append_serialized_toml_datum(&builder, datum);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status append_serialized_toml_datum(sc_string_builder *builder, toml_datum_t datum)
{
    char buffer[96] = {0};
    int written = 0;
    sc_status status = sc_status_ok();

    switch (datum.type) {
    case TOML_STRING:
        return append_serialized_string(builder, sc_str_from_parts(datum.u.str.ptr, (size_t)datum.u.str.len));
    case TOML_INT64:
        written = snprintf(buffer, sizeof(buffer), "%lld", (long long)datum.u.int64);
        break;
    case TOML_FP64:
        written = snprintf(buffer, sizeof(buffer), "%.17g", datum.u.fp64);
        break;
    case TOML_BOOLEAN:
        return sc_string_builder_append_cstr(builder, datum.u.boolean ? "true" : "false");
    case TOML_DATE:
        written = snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d", datum.u.ts.year, datum.u.ts.month, datum.u.ts.day);
        break;
    case TOML_TIME:
        written = snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", datum.u.ts.hour, datum.u.ts.minute, datum.u.ts.second);
        break;
    case TOML_DATETIME:
    case TOML_DATETIMETZ:
        written = snprintf(buffer,
                           sizeof(buffer),
                           "%04d-%02d-%02dT%02d:%02d:%02d",
                           datum.u.ts.year,
                           datum.u.ts.month,
                           datum.u.ts.day,
                           datum.u.ts.hour,
                           datum.u.ts.minute,
                           datum.u.ts.second);
        break;
    case TOML_ARRAY:
        status = sc_string_builder_append_cstr(builder, "[");
        for (int32_t i = 0; sc_status_is_ok(status) && i < datum.u.arr.size; i += 1) {
            if (i > 0) {
                status = sc_string_builder_append_cstr(builder, ",");
            }
            if (sc_status_is_ok(status)) {
                status = append_serialized_toml_datum(builder, datum.u.arr.elem[i]);
            }
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(builder, "]");
        }
        return status;
    case TOML_TABLE:
    case TOML_UNKNOWN:
        return sc_string_builder_append_cstr(builder, "");
    }
    if (written < 0 || (size_t)written >= sizeof(buffer)) {
        return sc_status_no_memory();
    }
    return sc_string_builder_append(builder, sc_str_from_parts(buffer, (size_t)written));
}

static sc_status append_serialized_string(sc_string_builder *builder, sc_str input)
{
    sc_status status = sc_string_builder_append_cstr(builder, "\"");
    for (size_t i = 0; sc_status_is_ok(status) && i < input.len; i += 1) {
        char ch = input.ptr[i];
        if (ch == '"' || ch == '\\') {
            char escaped[2] = {'\\', ch};
            status = sc_string_builder_append(builder, sc_str_from_parts(escaped, sizeof(escaped)));
        } else if (ch == '\n') {
            status = sc_string_builder_append_cstr(builder, "\\n");
        } else {
            status = sc_string_builder_append(builder, sc_str_from_parts(&ch, 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\"");
    }
    return status;
}

static bool find_assignment_position(sc_str input, sc_str key, size_t *line, size_t *column)
{
    size_t start = 0;
    size_t line_no = 1;

    for (size_t i = 0; i <= input.len; i += 1) {
        if (i == input.len || input.ptr[i] == '\n') {
            sc_str raw = sc_str_from_parts(&input.ptr[start], i - start);
            sc_str trimmed = trim(raw);
            size_t leading = (size_t)(trimmed.ptr - raw.ptr);
            if (trimmed.len > key.len && trimmed.ptr[0] != '#' &&
                memcmp(trimmed.ptr, key.ptr, key.len) == 0) {
                size_t offset = key.len;
                while (offset < trimmed.len && isspace((unsigned char)trimmed.ptr[offset])) {
                    offset += 1;
                }
                if (offset < trimmed.len && trimmed.ptr[offset] == '=') {
                    *line = line_no;
                    *column = leading + offset + 2;
                    return true;
                }
            }
            start = i + 1;
            line_no += 1;
        }
    }

    return false;
}

static bool find_section_assignment_position(sc_str input,
                                             sc_str section,
                                             sc_str key,
                                             size_t *line,
                                             size_t *column)
{
    bool in_section = false;
    size_t start = 0;
    size_t line_no = 1;

    for (size_t i = 0; i <= input.len; i += 1) {
        if (i == input.len || input.ptr[i] == '\n') {
            sc_str raw = sc_str_from_parts(&input.ptr[start], i - start);
            sc_str trimmed = trim(raw);
            size_t leading = (size_t)(trimmed.ptr - raw.ptr);

            if (trimmed.len >= 2 && trimmed.ptr[0] == '[') {
                sc_str section_name = {0};
                if (trimmed.len >= 4 && trimmed.ptr[1] == '[' && trimmed.ptr[trimmed.len - 2] == ']' &&
                    trimmed.ptr[trimmed.len - 1] == ']') {
                    section_name = trim(sc_str_from_parts(&trimmed.ptr[2], trimmed.len - 4));
                } else if (trimmed.ptr[trimmed.len - 1] == ']') {
                    section_name = trim(sc_str_from_parts(&trimmed.ptr[1], trimmed.len - 2));
                }
                in_section = sc_str_equal(section_name, section);
            } else if (in_section && trimmed.len > key.len && trimmed.ptr[0] != '#' &&
                       memcmp(trimmed.ptr, key.ptr, key.len) == 0) {
                size_t offset = key.len;
                while (offset < trimmed.len && isspace((unsigned char)trimmed.ptr[offset])) {
                    offset += 1;
                }
                if (offset < trimmed.len && trimmed.ptr[offset] == '=') {
                    *line = line_no;
                    *column = leading + offset + 2;
                    return true;
                }
            }

            start = i + 1;
            line_no += 1;
        }
    }

    return false;
}

static void find_value_position(sc_str input,
                                sc_str dotted_key,
                                sc_str leaf_key,
                                size_t *line,
                                size_t *column)
{
    *line = 1;
    *column = 1;
    if (find_assignment_position(input, dotted_key, line, column)) {
        return;
    }
    if (dotted_key.len > leaf_key.len + 1 && dotted_key.ptr[dotted_key.len - leaf_key.len - 1] == '.') {
        size_t section_len = dotted_key.len - leaf_key.len - 1;
        if (find_section_assignment_position(input,
                                             sc_str_from_parts(dotted_key.ptr, section_len),
                                             leaf_key,
                                             line,
                                             column)) {
            return;
        }
    }
    (void)find_assignment_position(input, leaf_key, line, column);
}

static size_t parse_toml_error_line(const char *message)
{
    const char *cursor = message == nullptr ? nullptr : strstr(message, "line ");
    char *end = nullptr;
    unsigned long parsed = 0;

    if (cursor == nullptr) {
        return 0;
    }
    cursor += strlen("line ");
    parsed = strtoul(cursor, &end, 10);
    if (cursor == end) {
        return 0;
    }
    return (size_t)parsed;
}
#else
static sc_status parse_line(sc_toml_source *source,
                            sc_str section,
                            sc_str line,
                            size_t line_no,
                            sc_toml_diag *diag)
{
    size_t equal = SIZE_MAX;
    sc_string full_key = {0};
    sc_toml_value *value = nullptr;
    sc_status status;
    sc_str key = {0};
    sc_str raw_value = {0};

    for (size_t i = 0; i < line.len; i += 1) {
        if (line.ptr[i] == '=') {
            equal = i;
            break;
        }
    }
    if (equal == SIZE_MAX) {
        return set_diag(diag, source->alloc, sc_string_as_str(&source->source_path), line_no, 1, "sc.toml.expected_equals");
    }

    key = trim(sc_str_from_parts(line.ptr, equal));
    raw_value = trim(sc_str_from_parts(&line.ptr[equal + 1], line.len - equal - 1));
    if (key.len == 0 || raw_value.len == 0) {
        return set_diag(diag, source->alloc, sc_string_as_str(&source->source_path), line_no, equal + 1, "sc.toml.empty_key_or_value");
    }

    status = join_key(source->alloc, section, key, &full_key);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    value = sc_alloc(source->alloc, sizeof(*value), _Alignof(sc_toml_value));
    if (value == nullptr) {
        sc_string_clear(&full_key);
        return sc_status_no_memory();
    }
    *value = (sc_toml_value){.alloc = source->alloc};
    value->line = line_no;
    value->column = equal + 2;

    if (raw_value.len >= 2 && raw_value.ptr[0] == '"' && raw_value.ptr[raw_value.len - 1] == '"') {
        value->type = SC_TOML_STRING;
        status = sc_string_from_str(source->alloc,
                                    sc_str_from_parts(&raw_value.ptr[1], raw_value.len - 2),
                                    &value->string_value);
    } else if (sc_str_equal(raw_value, sc_str_from_cstr("true")) ||
               sc_str_equal(raw_value, sc_str_from_cstr("false"))) {
        value->type = SC_TOML_BOOL;
        value->bool_value = sc_str_equal(raw_value, sc_str_from_cstr("true"));
    } else {
        char number[64] = {0};
        char *end = nullptr;
        if (raw_value.len >= sizeof(number)) {
            status = set_diag(diag, source->alloc, sc_string_as_str(&source->source_path), line_no, equal + 2, "sc.toml.number_too_large");
        } else {
            long parsed = 0;
            (void)memcpy(number, raw_value.ptr, raw_value.len);
            parsed = strtol(number, &end, 10);
            if (end == number || *end != '\0') {
                status = set_diag(diag, source->alloc, sc_string_as_str(&source->source_path), line_no, equal + 2, "sc.toml.unsupported_value");
            } else {
                value->type = SC_TOML_INTEGER;
                value->integer_value = (int64_t)parsed;
            }
        }
    }

    if (sc_status_is_ok(status)) {
        status = sc_map_put(&source->values, sc_string_as_str(&full_key), value);
    }
    if (!sc_status_is_ok(status)) {
        toml_value_destroy(source->alloc, value);
    }
    sc_string_clear(&full_key);
    return status;
}
#endif

static sc_status set_diag(sc_toml_diag *diag,
                          sc_allocator *alloc,
                          sc_str source_path,
                          size_t line,
                          size_t column,
                          const char *key)
{
    if (diag != nullptr) {
        diag->line = line;
        diag->column = column;
        diag->error_key = key;
        (void)sc_string_from_str(alloc, source_path, &diag->source_path);
    }
    return sc_status_parse(key);
}

static sc_status join_key(sc_allocator *alloc, sc_str section, sc_str key, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    sc_string_builder_init(&builder, alloc);
    if (section.len > 0) {
        status = sc_string_builder_append(&builder, section);
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, ".");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static void toml_value_destroy(sc_allocator *alloc, void *ptr)
{
    sc_toml_value *value = ptr;
    if (value == nullptr) {
        return;
    }
    sc_string_clear(&value->string_value);
    sc_free(alloc, value, sizeof(*value), _Alignof(sc_toml_value));
}
