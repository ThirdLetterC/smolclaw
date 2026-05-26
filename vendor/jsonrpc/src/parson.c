/*
 SPDX-License-Identifier: MIT

 Parson 1.5.3 (https://github.com/kgabis/parson)
 Copyright (c) 2012 - 2023 Krzysztof Gabis

 Permission is hereby granted, free of charge, to any person obtaining a copy
 of this software and associated documentation files (the "Software"), to deal
 in the Software without restriction, including without limitation the rights
 to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 copies of the Software, and to permit persons to whom the Software is
 furnished to do so, subject to the following conditions:

 The above copyright notice and this permission notice shall be included in
 all copies or substantial portions of the Software.

 THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 THE SOFTWARE.
*/
#ifdef _MSC_VER
#ifndef _CRT_SECURE_NO_WARNINGS
#define _CRT_SECURE_NO_WARNINGS
#endif /* _CRT_SECURE_NO_WARNINGS */
#endif /* _MSC_VER */

#include "jsonrpc/parson.h"

static constexpr int PARSON_IMPL_VERSION_MAJOR = 1;
static constexpr int PARSON_IMPL_VERSION_MINOR = 5;
static constexpr int PARSON_IMPL_VERSION_PATCH = 3;

#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static_assert(PARSON_VERSION_MAJOR == PARSON_IMPL_VERSION_MAJOR,
              "parson version mismatch between parson.c and parson.h");
static_assert(PARSON_VERSION_MINOR == PARSON_IMPL_VERSION_MINOR,
              "parson version mismatch between parson.c and parson.h");
static_assert(PARSON_VERSION_PATCH == PARSON_IMPL_VERSION_PATCH,
              "parson version mismatch between parson.c and parson.h");

/* Apparently sscanf is not implemented in some "standard" libraries, so don't
 * use it, if you don't have to. */
#ifdef sscanf
#undef sscanf
#define sscanf THINK_TWICE_ABOUT_USING_SSCANF
#endif

/* strcpy is unsafe */
#ifdef strcpy
#undef strcpy
#endif
#define strcpy USE_MEMCPY_INSTEAD_OF_STRCPY

static constexpr size_t starting_capacity = 16;
static constexpr size_t max_nesting = 2'048;
static constexpr char parson_default_float_format[] =
    "%1.17g"; /* do not increase precision without incresing NUM_BUF_SIZE */
static constexpr size_t parson_num_buf_size =
    64; /* double printed with "%1.17g" shouldn't be longer than 25 bytes so
           let's be paranoid and use 64 */
static constexpr char parson_indent_str[] = "    ";
static constexpr double json_number_epsilon = 0.000'001;

static constexpr size_t object_invalid_ix = SIZE_MAX;

static inline size_t max_size(size_t a, size_t b) { return a > b ? a : b; }

static inline void skip_char(const char **str) { ++(*str); }

static inline void skip_whitespaces(const char **str) {
  while (isspace((unsigned char)(**str))) {
    skip_char(str);
  }
}

static inline bool is_continuation_byte(unsigned char byte) {
  return (byte & 0xC0U) == 0x80U;
}

static inline bool is_number_invalid(double value) {
#if defined(isnan) && defined(isinf)
  return isnan(value) || isinf(value);
#else
  return (value * 0.0) != 0.0;
#endif
}

static JSON_Malloc_Function parson_malloc = malloc;
static JSON_Free_Function parson_free = free;

static bool parson_escape_slashes = true;

static char *parson_float_format = nullptr;

static JSON_Number_Serialization_Function parson_number_serialization_function =
    nullptr;

typedef struct json_string {
  char *chars;
  size_t length;
} JSON_String;

/* Type definitions */
typedef union json_value_value {
  JSON_String string;
  double number;
  JSON_Object *object;
  JSON_Array *array;
  bool boolean;
} JSON_Value_Value;

struct json_value_t {
  JSON_Value *parent;
  JSON_Value_Type type;
  JSON_Value_Value value;
};

struct json_object_t {
  JSON_Value *wrapping_value;
  size_t *cells;
  unsigned long *hashes;
  char **names;
  JSON_Value **values;
  size_t *cell_ixs;
  size_t count;
  size_t item_capacity;
  size_t cell_capacity;
};

struct json_array_t {
  JSON_Value *wrapping_value;
  JSON_Value **items;
  size_t count;
  size_t capacity;
};

/* Various */
[[nodiscard]] static char *read_file(const char *filename);
static void remove_comments(char *string, const char *start_token,
                            const char *end_token);
[[nodiscard]] static char *parson_strndup(const char *string, size_t n);
[[nodiscard]] static char *parson_strdup(const char *string);
static int parson_sprintf(char *s, size_t size, const char *format, ...);

static int hex_char_to_int(char c);
static JSON_Status parse_utf16_hex(const char *string, unsigned int *result);
static int num_bytes_in_utf8_sequence(unsigned char c);
static JSON_Status verify_utf8_sequence(const unsigned char *string, int *len);
static bool is_valid_utf8(const char *string, size_t string_len);
static bool is_decimal(const char *string, size_t length);
static unsigned long hash_string(const char *string, size_t n);

/* JSON Object */
[[nodiscard]] static JSON_Object *json_object_make(JSON_Value *wrapping_value);
static JSON_Status json_object_init(JSON_Object *object, size_t capacity);
static void json_object_deinit(JSON_Object *object, bool free_keys,
                               bool free_values);
static JSON_Status json_object_grow_and_rehash(JSON_Object *object);
static size_t json_object_get_cell_ix(const JSON_Object *object,
                                      const char *key, size_t key_len,
                                      unsigned long hash, bool *out_found);
static JSON_Status json_object_add(JSON_Object *object, char *name,
                                   JSON_Value *value);
static JSON_Value *json_object_getn_value(const JSON_Object *object,
                                          const char *name, size_t name_len);
static JSON_Status json_object_remove_internal(JSON_Object *object,
                                               const char *name,
                                               bool free_value);
static JSON_Status json_object_dotremove_internal(JSON_Object *object,
                                                  const char *name,
                                                  bool free_value);
static void json_object_free(JSON_Object *object);

/* JSON Array */
[[nodiscard]] static JSON_Array *json_array_make(JSON_Value *wrapping_value);
static JSON_Status json_array_add(JSON_Array *array, JSON_Value *value);
static JSON_Status json_array_resize(JSON_Array *array, size_t new_capacity);
static void json_array_free(JSON_Array *array);

/* JSON Value */
[[nodiscard]] static JSON_Value *json_value_init_string_no_copy(char *string,
                                                                size_t length);
static const JSON_String *json_value_get_string_desc(const JSON_Value *value);

/* Parser */
static JSON_Status skip_quotes(const char **string);
static JSON_Status parse_utf16(const char **unprocessed, char **processed);
[[nodiscard]] static char *process_string(const char *input, size_t input_len,
                                          size_t *output_len);
[[nodiscard]] static char *get_quoted_string(const char **string,
                                             size_t *output_string_len);
[[nodiscard]] static JSON_Value *parse_object_value(const char **string,
                                                    size_t nesting);
[[nodiscard]] static JSON_Value *parse_array_value(const char **string,
                                                   size_t nesting);
[[nodiscard]] static JSON_Value *parse_string_value(const char **string);
[[nodiscard]] static JSON_Value *parse_boolean_value(const char **string);
[[nodiscard]] static JSON_Value *parse_number_value(const char **string);
[[nodiscard]] static JSON_Value *parse_null_value(const char **string);
[[nodiscard]] static JSON_Value *parse_value(const char **string,
                                             size_t nesting);

/* Serialization */
static int json_serialize_to_buffer_r(const JSON_Value *value, char *buf,
                                      int level, bool is_pretty, char *num_buf);
static int json_serialize_string(const char *string, size_t len, char *buf);

/* Various */
[[nodiscard]] static void *parson_calloc(size_t count, size_t size) {
  if (count != 0U && size > SIZE_MAX / count) {
    return nullptr;
  }
  const size_t total_size = count * size;
  auto memory = parson_malloc(total_size);
  if (memory == nullptr) {
    return nullptr;
  }
  memset(memory, 0, total_size);
  return memory;
}

[[nodiscard]] static char *read_file(const char *filename) {
  auto fp = fopen(filename, "r");
  size_t size_to_read = 0;
  size_t size_read = 0;
  long pos = 0;
  char *file_contents = nullptr;
  if (fp == nullptr) {
    return nullptr;
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    fclose(fp);
    return nullptr;
  }
  pos = ftell(fp);
  if (pos < 0) {
    fclose(fp);
    return nullptr;
  }
  size_to_read = (size_t)pos;
  if (size_to_read == SIZE_MAX) {
    fclose(fp);
    return nullptr;
  }
  rewind(fp);
  file_contents = (char *)parson_calloc(size_to_read + 1, sizeof(char));
  if (file_contents == nullptr) {
    fclose(fp);
    return nullptr;
  }
  size_read = fread(file_contents, 1, size_to_read, fp);
  if (size_read == 0 || ferror(fp)) {
    fclose(fp);
    parson_free(file_contents);
    return nullptr;
  }
  fclose(fp);
  file_contents[size_read] = '\0';
  return file_contents;
}

static void remove_comments(char *string, const char *start_token,
                            const char *end_token) {
  bool in_string = false, escaped = false;
  size_t i;
  char *ptr = nullptr, current_char;
  size_t start_token_len = strlen(start_token);
  size_t end_token_len = strlen(end_token);
  if (start_token_len == 0 || end_token_len == 0) {
    return;
  }
  while ((current_char = *string) != '\0') {
    if (current_char == '\\' && !escaped) {
      escaped = true;
      string++;
      continue;
    } else if (current_char == '\"' && !escaped) {
      in_string = !in_string;
    } else if (!in_string &&
               strncmp(string, start_token, start_token_len) == 0) {
      for (i = 0; i < start_token_len; i++) {
        string[i] = ' ';
      }
      string = string + start_token_len;
      ptr = strstr(string, end_token);
      if (ptr == nullptr) {
        return;
      }
      for (i = 0; i < (ptr - string) + end_token_len; i++) {
        string[i] = ' ';
      }
      string = ptr + end_token_len - 1;
    }
    escaped = false;
    string++;
  }
}

[[nodiscard]] static char *parson_strndup(const char *string, size_t n) {
  /* We expect the caller has validated that 'n' fits within the input buffer.
   */
  auto output_string = (char *)parson_calloc(n + 1, sizeof(char));
  if (output_string == nullptr) {
    return nullptr;
  }
  output_string[n] = '\0';
  memcpy(output_string, string, n);
  return output_string;
}

[[nodiscard]] static char *parson_strdup(const char *string) {
  return parson_strndup(string, strlen(string));
}

static int parson_sprintf(char *s, size_t size, const char *format, ...) {
  va_list args;
  va_start(args, format);
  const int result = vsnprintf(s, size, format, args);
  va_end(args);
  return result;
}

static int hex_char_to_int(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  } else if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  } else if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static JSON_Status parse_utf16_hex(const char *s, unsigned int *result) {
  int x1, x2, x3, x4;
  if (s[0] == '\0' || s[1] == '\0' || s[2] == '\0' || s[3] == '\0') {
    return JSONFailure;
  }
  x1 = hex_char_to_int(s[0]);
  x2 = hex_char_to_int(s[1]);
  x3 = hex_char_to_int(s[2]);
  x4 = hex_char_to_int(s[3]);
  if (x1 == -1 || x2 == -1 || x3 == -1 || x4 == -1) {
    return JSONFailure;
  }
  *result = (unsigned int)((x1 << 12) | (x2 << 8) | (x3 << 4) | x4);
  return JSONSuccess;
}

static int num_bytes_in_utf8_sequence(unsigned char c) {
  if (c == 0xC0 || c == 0xC1 || c > 0xF4 || is_continuation_byte(c)) {
    return 0;
  } else if ((c & 0x80) == 0) { /* 0xxxxxxx */
    return 1;
  } else if ((c & 0xE0) == 0xC0) { /* 110xxxxx */
    return 2;
  } else if ((c & 0xF0) == 0xE0) { /* 1110xxxx */
    return 3;
  } else if ((c & 0xF8) == 0xF0) { /* 11110xxx */
    return 4;
  }
  return 0; /* won't happen */
}

static JSON_Status verify_utf8_sequence(const unsigned char *string, int *len) {
  unsigned int cp = 0;
  *len = num_bytes_in_utf8_sequence(string[0]);

  if (*len == 1) {
    cp = string[0];
  } else if (*len == 2 && is_continuation_byte(string[1])) {
    cp = string[0] & 0x1F;
    cp = (cp << 6) | (string[1] & 0x3F);
  } else if (*len == 3 && is_continuation_byte(string[1]) &&
             is_continuation_byte(string[2])) {
    cp = ((unsigned char)string[0]) & 0xF;
    cp = (cp << 6) | (string[1] & 0x3F);
    cp = (cp << 6) | (string[2] & 0x3F);
  } else if (*len == 4 && is_continuation_byte(string[1]) &&
             is_continuation_byte(string[2]) &&
             is_continuation_byte(string[3])) {
    cp = string[0] & 0x7;
    cp = (cp << 6) | (string[1] & 0x3F);
    cp = (cp << 6) | (string[2] & 0x3F);
    cp = (cp << 6) | (string[3] & 0x3F);
  } else {
    return JSONFailure;
  }

  /* overlong encodings */
  if ((cp < 0x80 && *len > 1) || (cp < 0x800 && *len > 2) ||
      (cp < 0x10000 && *len > 3)) {
    return JSONFailure;
  }

  /* invalid unicode */
  if (cp > 0x10FFFF) {
    return JSONFailure;
  }

  /* surrogate halves */
  if (cp >= 0xD800 && cp <= 0xDFFF) {
    return JSONFailure;
  }

  return JSONSuccess;
}

static bool is_valid_utf8(const char *string, size_t string_len) {
  int len = 0;
  const char *string_end = string + string_len;
  while (string < string_end) {
    if (verify_utf8_sequence((const unsigned char *)string, &len) !=
        JSONSuccess) {
      return false;
    }
    string += len;
  }
  return true;
}

static bool is_decimal(const char *string, size_t length) {
  if (length > 1 && string[0] == '0' && string[1] != '.') {
    return false;
  }
  if (length > 2 && !strncmp(string, "-0", 2) && string[2] != '.') {
    return false;
  }
  while (length--) {
    if (strchr("xX", string[length])) {
      return false;
    }
  }
  return true;
}

static unsigned long hash_string(const char *string, size_t n) {
#ifdef PARSON_FORCE_HASH_COLLISIONS
  (void)string;
  (void)n;
  return 0;
#else
  unsigned long hash = 5381;
  unsigned char c;
  size_t i = 0;
  for (i = 0; i < n; i++) {
    c = string[i];
    if (c == '\0') {
      break;
    }
    hash = ((hash << 5) + hash) + c; /* hash * 33 + c */
  }
  return hash;
#endif
}

/* JSON Object */
[[nodiscard]] static JSON_Object *json_object_make(JSON_Value *wrapping_value) {
  JSON_Status res = JSONFailure;
  auto new_obj = (JSON_Object *)parson_calloc(1, sizeof(JSON_Object));
  if (new_obj == nullptr) {
    return nullptr;
  }
  new_obj->wrapping_value = wrapping_value;
  res = json_object_init(new_obj, 0);
  if (res != JSONSuccess) {
    parson_free(new_obj);
    return nullptr;
  }
  return new_obj;
}

static JSON_Status json_object_init(JSON_Object *object, size_t capacity) {
  size_t i = 0;

  object->cells = nullptr;
  object->names = nullptr;
  object->values = nullptr;
  object->cell_ixs = nullptr;
  object->hashes = nullptr;

  object->count = 0;
  object->cell_capacity = capacity;
  object->item_capacity = (capacity * 7U) / 10U;

  if (capacity == 0) {
    return JSONSuccess;
  }

  object->cells =
      (size_t *)parson_calloc(object->cell_capacity, sizeof(*object->cells));
  object->names =
      (char **)parson_calloc(object->item_capacity, sizeof(*object->names));
  object->values = (JSON_Value **)parson_calloc(object->item_capacity,
                                                sizeof(*object->values));
  object->cell_ixs =
      (size_t *)parson_calloc(object->item_capacity, sizeof(*object->cell_ixs));
  object->hashes = (unsigned long *)parson_calloc(object->item_capacity,
                                                  sizeof(*object->hashes));
  if (object->cells == nullptr || object->names == nullptr ||
      object->values == nullptr || object->cell_ixs == nullptr ||
      object->hashes == nullptr) {
    goto error;
  }
  for (i = 0; i < object->cell_capacity; i++) {
    object->cells[i] = object_invalid_ix;
  }
  return JSONSuccess;
error:
  parson_free(object->cells);
  parson_free(object->names);
  parson_free(object->values);
  parson_free(object->cell_ixs);
  parson_free(object->hashes);
  return JSONFailure;
}

static void json_object_deinit(JSON_Object *object, bool free_keys,
                               bool free_values) {
  unsigned int i = 0;
  for (i = 0; i < object->count; i++) {
    if (free_keys) {
      parson_free(object->names[i]);
    }
    if (free_values) {
      json_value_free(object->values[i]);
    }
  }

  object->count = 0;
  object->item_capacity = 0;
  object->cell_capacity = 0;

  parson_free(object->cells);
  parson_free(object->names);
  parson_free(object->values);
  parson_free(object->cell_ixs);
  parson_free(object->hashes);

  object->cells = nullptr;
  object->names = nullptr;
  object->values = nullptr;
  object->cell_ixs = nullptr;
  object->hashes = nullptr;
}

static JSON_Status json_object_grow_and_rehash(JSON_Object *object) {
  JSON_Value *wrapping_value = nullptr;
  JSON_Object new_object;
  char *key = nullptr;
  JSON_Value *value = nullptr;
  unsigned int i = 0;
  size_t new_capacity = max_size(object->cell_capacity * 2, starting_capacity);
  JSON_Status res = json_object_init(&new_object, new_capacity);
  if (res != JSONSuccess) {
    return JSONFailure;
  }

  wrapping_value = json_object_get_wrapping_value(object);
  new_object.wrapping_value = wrapping_value;

  for (i = 0; i < object->count; i++) {
    key = object->names[i];
    value = object->values[i];
    res = json_object_add(&new_object, key, value);
    if (res != JSONSuccess) {
      json_object_deinit(&new_object, false, false);
      return JSONFailure;
    }
    value->parent = wrapping_value;
  }
  json_object_deinit(object, false, false);
  *object = new_object;
  return JSONSuccess;
}

static size_t json_object_get_cell_ix(const JSON_Object *object,
                                      const char *key, size_t key_len,
                                      unsigned long hash, bool *out_found) {
  size_t cell_ix = hash & (object->cell_capacity - 1);
  size_t cell = 0;
  size_t ix = 0;
  unsigned int i = 0;
  unsigned long hash_to_check = 0;
  const char *key_to_check = nullptr;
  size_t key_to_check_len = 0;

  *out_found = false;

  for (i = 0; i < object->cell_capacity; i++) {
    ix = (cell_ix + i) & (object->cell_capacity - 1);
    cell = object->cells[ix];
    if (cell == object_invalid_ix) {
      return ix;
    }
    hash_to_check = object->hashes[cell];
    if (hash != hash_to_check) {
      continue;
    }
    key_to_check = object->names[cell];
    key_to_check_len = strlen(key_to_check);
    if (key_to_check_len == key_len &&
        strncmp(key, key_to_check, key_len) == 0) {
      *out_found = true;
      return ix;
    }
  }
  return object_invalid_ix;
}

static JSON_Status json_object_add(JSON_Object *object, char *name,
                                   JSON_Value *value) {
  unsigned long hash = 0;
  bool found = false;
  size_t cell_ix = 0;
  JSON_Status res = JSONFailure;

  if (object == nullptr || name == nullptr || value == nullptr) {
    return JSONFailure;
  }

  hash = hash_string(name, strlen(name));
  found = false;
  cell_ix = json_object_get_cell_ix(object, name, strlen(name), hash, &found);
  if (found) {
    return JSONFailure;
  }

  if (object->count >= object->item_capacity) {
    res = json_object_grow_and_rehash(object);
    if (res != JSONSuccess) {
      return JSONFailure;
    }
    cell_ix = json_object_get_cell_ix(object, name, strlen(name), hash, &found);
  }

  object->names[object->count] = name;
  object->cells[cell_ix] = object->count;
  object->values[object->count] = value;
  object->cell_ixs[object->count] = cell_ix;
  object->hashes[object->count] = hash;
  object->count++;
  value->parent = json_object_get_wrapping_value(object);

  return JSONSuccess;
}

static JSON_Value *json_object_getn_value(const JSON_Object *object,
                                          const char *name, size_t name_len) {
  unsigned long hash = 0;
  bool found = false;
  size_t cell_ix = 0;
  size_t item_ix = 0;
  if (object == nullptr || name == nullptr) {
    return nullptr;
  }
  hash = hash_string(name, name_len);
  found = false;
  cell_ix = json_object_get_cell_ix(object, name, name_len, hash, &found);
  if (!found) {
    return nullptr;
  }
  item_ix = object->cells[cell_ix];
  return object->values[item_ix];
}

static JSON_Status json_object_remove_internal(JSON_Object *object,
                                               const char *name,
                                               bool free_value) {
  unsigned long hash = 0;
  bool found = false;
  size_t cell = 0;
  size_t item_ix = 0;
  size_t last_item_ix = 0;
  size_t i = 0;
  size_t j = 0;
  size_t x = 0;
  size_t k = 0;
  JSON_Value *val = nullptr;

  if (object == nullptr) {
    return JSONFailure;
  }

  hash = hash_string(name, strlen(name));
  found = false;
  cell = json_object_get_cell_ix(object, name, strlen(name), hash, &found);
  if (!found) {
    return JSONFailure;
  }

  item_ix = object->cells[cell];
  if (free_value) {
    val = object->values[item_ix];
    json_value_free(val);
    val = nullptr;
  }

  parson_free(object->names[item_ix]);
  last_item_ix = object->count - 1;
  if (item_ix < last_item_ix) {
    object->names[item_ix] = object->names[last_item_ix];
    object->values[item_ix] = object->values[last_item_ix];
    object->cell_ixs[item_ix] = object->cell_ixs[last_item_ix];
    object->hashes[item_ix] = object->hashes[last_item_ix];
    object->cells[object->cell_ixs[item_ix]] = item_ix;
  }
  object->count--;

  i = cell;
  j = i;
  for (x = 0; x < (object->cell_capacity - 1); x++) {
    j = (j + 1) & (object->cell_capacity - 1);
    if (object->cells[j] == object_invalid_ix) {
      break;
    }
    k = object->hashes[object->cells[j]] & (object->cell_capacity - 1);
    if ((j > i && (k <= i || k > j)) || (j < i && (k <= i && k > j))) {
      object->cell_ixs[object->cells[j]] = i;
      object->cells[i] = object->cells[j];
      i = j;
    }
  }
  object->cells[i] = object_invalid_ix;
  return JSONSuccess;
}

static JSON_Status json_object_dotremove_internal(JSON_Object *object,
                                                  const char *name,
                                                  bool free_value) {
  JSON_Value *temp_value = nullptr;
  JSON_Object *temp_object = nullptr;
  const char *dot_pos = strchr(name, '.');
  if (dot_pos == nullptr) {
    return json_object_remove_internal(object, name, free_value);
  }
  temp_value = json_object_getn_value(object, name, dot_pos - name);
  if (json_value_get_type(temp_value) != JSONObject) {
    return JSONFailure;
  }
  temp_object = json_value_get_object(temp_value);
  return json_object_dotremove_internal(temp_object, dot_pos + 1, free_value);
}

static void json_object_free(JSON_Object *object) {
  json_object_deinit(object, true, true);
  parson_free(object);
}

/* JSON Array */
[[nodiscard]] static JSON_Array *json_array_make(JSON_Value *wrapping_value) {
  auto new_array = (JSON_Array *)parson_calloc(1, sizeof(JSON_Array));
  if (new_array == nullptr) {
    return nullptr;
  }
  new_array->wrapping_value = wrapping_value;
  return new_array;
}

static JSON_Status json_array_add(JSON_Array *array, JSON_Value *value) {
  if (array->count >= array->capacity) {
    size_t new_capacity = max_size(array->capacity * 2, starting_capacity);
    if (json_array_resize(array, new_capacity) != JSONSuccess) {
      return JSONFailure;
    }
  }
  value->parent = json_array_get_wrapping_value(array);
  array->items[array->count] = value;
  array->count++;
  return JSONSuccess;
}

static JSON_Status json_array_resize(JSON_Array *array, size_t new_capacity) {
  JSON_Value **new_items = nullptr;
  if (new_capacity == 0) {
    return JSONFailure;
  }
  new_items = (JSON_Value **)parson_calloc(new_capacity, sizeof(JSON_Value *));
  if (new_items == nullptr) {
    return JSONFailure;
  }
  if (array->items != nullptr && array->count > 0) {
    memcpy(new_items, array->items, array->count * sizeof(JSON_Value *));
  }
  parson_free(array->items);
  array->items = new_items;
  array->capacity = new_capacity;
  return JSONSuccess;
}

static void json_array_free(JSON_Array *array) {
  size_t i;
  for (i = 0; i < array->count; i++) {
    json_value_free(array->items[i]);
  }
  parson_free(array->items);
  parson_free(array);
}

/* JSON Value */
[[nodiscard]] static JSON_Value *json_value_init_string_no_copy(char *string,
                                                                size_t length) {
  auto new_value = (JSON_Value *)parson_calloc(1, sizeof(JSON_Value));
  if (new_value == nullptr) {
    return nullptr;
  }
  new_value->parent = nullptr;
  new_value->type = JSONString;
  new_value->value.string.chars = string;
  new_value->value.string.length = length;
  return new_value;
}

/* Parser */
static JSON_Status skip_quotes(const char **string) {
  if (**string != '\"') {
    return JSONFailure;
  }
  skip_char(string);
  while (**string != '\"') {
    if (**string == '\0') {
      return JSONFailure;
    } else if (**string == '\\') {
      skip_char(string);
      if (**string == '\0') {
        return JSONFailure;
      }
    }
    skip_char(string);
  }
  skip_char(string);
  return JSONSuccess;
}

static JSON_Status parse_utf16(const char **unprocessed, char **processed) {
  unsigned int cp, lead, trail;
  char *processed_ptr = *processed;
  const char *unprocessed_ptr = *unprocessed;
  JSON_Status status = JSONFailure;
  unprocessed_ptr++; /* skips u */
  status = parse_utf16_hex(unprocessed_ptr, &cp);
  if (status != JSONSuccess) {
    return JSONFailure;
  }
  if (cp < 0x80) {
    processed_ptr[0] = (char)cp; /* 0xxxxxxx */
  } else if (cp < 0x800) {
    processed_ptr[0] = ((cp >> 6) & 0x1F) | 0xC0; /* 110xxxxx */
    processed_ptr[1] = ((cp) & 0x3F) | 0x80;      /* 10xxxxxx */
    processed_ptr += 1;
  } else if (cp < 0xD800 || cp > 0xDFFF) {
    processed_ptr[0] = ((cp >> 12) & 0x0F) | 0xE0; /* 1110xxxx */
    processed_ptr[1] = ((cp >> 6) & 0x3F) | 0x80;  /* 10xxxxxx */
    processed_ptr[2] = ((cp) & 0x3F) | 0x80;       /* 10xxxxxx */
    processed_ptr += 2;
  } else if (cp >= 0xD800 &&
             cp <= 0xDBFF) { /* lead surrogate (0xD800..0xDBFF) */
    lead = cp;
    unprocessed_ptr += 4; /* should always be within the buffer, otherwise
                             previous sscanf would fail */
    if (*unprocessed_ptr++ != '\\' || *unprocessed_ptr++ != 'u') {
      return JSONFailure;
    }
    status = parse_utf16_hex(unprocessed_ptr, &trail);
    if (status != JSONSuccess || trail < 0xDC00 ||
        trail > 0xDFFF) { /* valid trail surrogate? (0xDC00..0xDFFF) */
      return JSONFailure;
    }
    cp = ((((lead - 0xD800) & 0x3FF) << 10) | ((trail - 0xDC00) & 0x3FF)) +
         0x010000;
    processed_ptr[0] = (((cp >> 18) & 0x07) | 0xF0); /* 11110xxx */
    processed_ptr[1] = (((cp >> 12) & 0x3F) | 0x80); /* 10xxxxxx */
    processed_ptr[2] = (((cp >> 6) & 0x3F) | 0x80);  /* 10xxxxxx */
    processed_ptr[3] = (((cp) & 0x3F) | 0x80);       /* 10xxxxxx */
    processed_ptr += 3;
  } else { /* trail surrogate before lead surrogate */
    return JSONFailure;
  }
  unprocessed_ptr += 3;
  *processed = processed_ptr;
  *unprocessed = unprocessed_ptr;
  return JSONSuccess;
}

/* Copies and processes passed string up to supplied length.
Example: "\u006Corem ipsum" -> lorem ipsum */
[[nodiscard]] static char *process_string(const char *input, size_t input_len,
                                          size_t *output_len) {
  const char *input_ptr = input;
  const size_t initial_size = input_len + 1;
  size_t final_size = 0;
  char *output = nullptr, *output_ptr = nullptr, *resized_output = nullptr;
  output = (char *)parson_calloc(initial_size, sizeof(char));
  if (output == nullptr) {
    goto error;
  }
  output_ptr = output;
  while ((*input_ptr != '\0') && (size_t)(input_ptr - input) < input_len) {
    if (*input_ptr == '\\') {
      input_ptr++;
      switch (*input_ptr) {
      case '\"':
        *output_ptr = '\"';
        break;
      case '\\':
        *output_ptr = '\\';
        break;
      case '/':
        *output_ptr = '/';
        break;
      case 'b':
        *output_ptr = '\b';
        break;
      case 'f':
        *output_ptr = '\f';
        break;
      case 'n':
        *output_ptr = '\n';
        break;
      case 'r':
        *output_ptr = '\r';
        break;
      case 't':
        *output_ptr = '\t';
        break;
      case 'u':
        if (parse_utf16(&input_ptr, &output_ptr) != JSONSuccess) {
          goto error;
        }
        break;
      default:
        goto error;
      }
    } else if ((unsigned char)*input_ptr < 0x20) {
      goto error; /* 0x00-0x19 are invalid characters for json string
                     (http://www.ietf.org/rfc/rfc4627.txt) */
    } else {
      *output_ptr = *input_ptr;
    }
    output_ptr++;
    input_ptr++;
  }
  *output_ptr = '\0';
  /* resize to new length */
  final_size = (size_t)(output_ptr - output) + 1;
  /* todo: don't resize if final_size == initial_size */
  resized_output = (char *)parson_calloc(final_size, sizeof(char));
  if (resized_output == nullptr) {
    goto error;
  }
  memcpy(resized_output, output, final_size);
  *output_len = final_size - 1;
  parson_free(output);
  return resized_output;
error:
  parson_free(output);
  return nullptr;
}

/* Return processed contents of a string between quotes and
   skips passed argument to a matching quote. */
[[nodiscard]] static char *get_quoted_string(const char **string,
                                             size_t *output_string_len) {
  const char *string_start = *string;
  size_t input_string_len = 0;
  JSON_Status status = skip_quotes(string);
  if (status != JSONSuccess) {
    return nullptr;
  }
  input_string_len = *string - string_start - 2; /* length without quotes */
  return process_string(string_start + 1, input_string_len, output_string_len);
}

[[nodiscard]] static JSON_Value *parse_value(const char **string,
                                             size_t nesting) {
  if (nesting > max_nesting) {
    return nullptr;
  }
  skip_whitespaces(string);
  switch (**string) {
  case '{':
    return parse_object_value(string, nesting + 1);
  case '[':
    return parse_array_value(string, nesting + 1);
  case '\"':
    return parse_string_value(string);
  case 'f':
  case 't':
    return parse_boolean_value(string);
  case '-':
  case '0':
  case '1':
  case '2':
  case '3':
  case '4':
  case '5':
  case '6':
  case '7':
  case '8':
  case '9':
    return parse_number_value(string);
  case 'n':
    return parse_null_value(string);
  default:
    return nullptr;
  }
}

[[nodiscard]] static JSON_Value *parse_object_value(const char **string,
                                                    size_t nesting) {
  JSON_Status status = JSONFailure;
  JSON_Value *output_value = nullptr, *new_value = nullptr;
  JSON_Object *output_object = nullptr;
  char *new_key = nullptr;

  output_value = json_value_init_object();
  if (output_value == nullptr) {
    return nullptr;
  }
  if (**string != '{') {
    json_value_free(output_value);
    return nullptr;
  }
  output_object = json_value_get_object(output_value);
  skip_char(string);
  skip_whitespaces(string);
  if (**string == '}') { /* empty object */
    skip_char(string);
    return output_value;
  }
  while (**string != '\0') {
    size_t key_len = 0;
    new_key = get_quoted_string(string, &key_len);
    /* We do not support key names with embedded \0 chars */
    if (new_key == nullptr) {
      json_value_free(output_value);
      return nullptr;
    }
    if (key_len != strlen(new_key)) {
      parson_free(new_key);
      json_value_free(output_value);
      return nullptr;
    }
    skip_whitespaces(string);
    if (**string != ':') {
      parson_free(new_key);
      json_value_free(output_value);
      return nullptr;
    }
    skip_char(string);
    new_value = parse_value(string, nesting);
    if (new_value == nullptr) {
      parson_free(new_key);
      json_value_free(output_value);
      return nullptr;
    }
    status = json_object_add(output_object, new_key, new_value);
    if (status != JSONSuccess) {
      parson_free(new_key);
      json_value_free(new_value);
      json_value_free(output_value);
      return nullptr;
    }
    skip_whitespaces(string);
    if (**string != ',') {
      break;
    }
    skip_char(string);
    skip_whitespaces(string);
    if (**string == '}') {
      break;
    }
  }
  skip_whitespaces(string);
  if (**string != '}') {
    json_value_free(output_value);
    return nullptr;
  }
  skip_char(string);
  return output_value;
}

[[nodiscard]] static JSON_Value *parse_array_value(const char **string,
                                                   size_t nesting) {
  JSON_Value *output_value = nullptr, *new_array_value = nullptr;
  JSON_Array *output_array = nullptr;
  output_value = json_value_init_array();
  if (output_value == nullptr) {
    return nullptr;
  }
  if (**string != '[') {
    json_value_free(output_value);
    return nullptr;
  }
  output_array = json_value_get_array(output_value);
  skip_char(string);
  skip_whitespaces(string);
  if (**string == ']') { /* empty array */
    skip_char(string);
    return output_value;
  }
  while (**string != '\0') {
    new_array_value = parse_value(string, nesting);
    if (new_array_value == nullptr) {
      json_value_free(output_value);
      return nullptr;
    }
    if (json_array_add(output_array, new_array_value) != JSONSuccess) {
      json_value_free(new_array_value);
      json_value_free(output_value);
      return nullptr;
    }
    skip_whitespaces(string);
    if (**string != ',') {
      break;
    }
    skip_char(string);
    skip_whitespaces(string);
    if (**string == ']') {
      break;
    }
  }
  skip_whitespaces(string);
  if (**string != ']' || /* Trim array after parsing is over */
      json_array_resize(output_array, json_array_get_count(output_array)) !=
          JSONSuccess) {
    json_value_free(output_value);
    return nullptr;
  }
  skip_char(string);
  return output_value;
}

[[nodiscard]] static JSON_Value *parse_string_value(const char **string) {
  JSON_Value *value = nullptr;
  size_t new_string_len = 0;
  char *new_string = get_quoted_string(string, &new_string_len);
  if (new_string == nullptr) {
    return nullptr;
  }
  value = json_value_init_string_no_copy(new_string, new_string_len);
  if (value == nullptr) {
    parson_free(new_string);
    return nullptr;
  }
  return value;
}

[[nodiscard]] static JSON_Value *parse_boolean_value(const char **string) {
  constexpr size_t true_token_size = sizeof("true") - 1;
  constexpr size_t false_token_size = sizeof("false") - 1;
  if (strncmp("true", *string, true_token_size) == 0) {
    *string += true_token_size;
    return json_value_init_boolean(true);
  } else if (strncmp("false", *string, false_token_size) == 0) {
    *string += false_token_size;
    return json_value_init_boolean(false);
  }
  return nullptr;
}

[[nodiscard]] static JSON_Value *parse_number_value(const char **string) {
  char *end;
  double number = 0;
  errno = 0;
  number = strtod(*string, &end);
  if (errno == ERANGE && (number <= -HUGE_VAL || number >= HUGE_VAL)) {
    return nullptr;
  }
  if ((errno && errno != ERANGE) || !is_decimal(*string, end - *string)) {
    return nullptr;
  }
  *string = end;
  return json_value_init_number(number);
}

[[nodiscard]] static JSON_Value *parse_null_value(const char **string) {
  constexpr size_t token_size = sizeof("null") - 1;
  if (strncmp("null", *string, token_size) == 0) {
    *string += token_size;
    return json_value_init_null();
  }
  return nullptr;
}

/* Serialization */

typedef struct serialization_buffer {
  char *cursor;
  int written_total;
} serialization_buffer;

static inline void append_literal(serialization_buffer *buffer,
                                  const char *literal) {
  const size_t written = strlen(literal);
  if (buffer->cursor != nullptr) {
    memcpy(buffer->cursor, literal, written);
    buffer->cursor[written] = '\0';
    buffer->cursor += written;
  }
  buffer->written_total += (int)written;
}

static inline void append_indent(serialization_buffer *buffer, int level) {
  for (int level_i = 0; level_i < level; level_i++) {
    append_literal(buffer, parson_indent_str);
  }
}

static int json_serialize_to_buffer_r(const JSON_Value *value, char *buf,
                                      int level, bool is_pretty,
                                      char *num_buf) {
  const char *key = nullptr, *string = nullptr;
  JSON_Value *temp_value = nullptr;
  JSON_Array *array = nullptr;
  JSON_Object *object = nullptr;
  size_t i = 0, count = 0;
  double num = 0.0;
  int written = -1;
  size_t len = 0;
  serialization_buffer out = {
      .cursor = buf,
      .written_total = 0,
  };

  switch (json_value_get_type(value)) {
  case JSONArray:
    array = json_value_get_array(value);
    count = json_array_get_count(array);
    append_literal(&out, "[");
    if (count > 0 && is_pretty) {
      append_literal(&out, "\n");
    }
    for (i = 0; i < count; i++) {
      if (is_pretty) {
        append_indent(&out, level + 1);
      }
      temp_value = json_array_get_value(array, i);
      written = json_serialize_to_buffer_r(temp_value, out.cursor, level + 1,
                                           is_pretty, num_buf);
      if (written < 0) {
        return -1;
      }
      if (out.cursor != nullptr) {
        out.cursor += written;
      }
      out.written_total += written;
      if (i < (count - 1)) {
        append_literal(&out, ",");
      }
      if (is_pretty) {
        append_literal(&out, "\n");
      }
    }
    if (count > 0 && is_pretty) {
      append_indent(&out, level);
    }
    append_literal(&out, "]");
    return out.written_total;
  case JSONObject:
    object = json_value_get_object(value);
    count = json_object_get_count(object);
    append_literal(&out, "{");
    if (count > 0 && is_pretty) {
      append_literal(&out, "\n");
    }
    for (i = 0; i < count; i++) {
      key = json_object_get_name(object, i);
      if (key == nullptr) {
        return -1;
      }
      if (is_pretty) {
        append_indent(&out, level + 1);
      }
      /* We do not support key names with embedded \\0 chars */
      written = json_serialize_string(key, strlen(key), out.cursor);
      if (written < 0) {
        return -1;
      }
      if (out.cursor != nullptr) {
        out.cursor += written;
      }
      out.written_total += written;
      append_literal(&out, ":");
      if (is_pretty) {
        append_literal(&out, " ");
      }
      temp_value = json_object_get_value_at(object, i);
      written = json_serialize_to_buffer_r(temp_value, out.cursor, level + 1,
                                           is_pretty, num_buf);
      if (written < 0) {
        return -1;
      }
      if (out.cursor != nullptr) {
        out.cursor += written;
      }
      out.written_total += written;
      if (i < (count - 1)) {
        append_literal(&out, ",");
      }
      if (is_pretty) {
        append_literal(&out, "\n");
      }
    }
    if (count > 0 && is_pretty) {
      append_indent(&out, level);
    }
    append_literal(&out, "}");
    return out.written_total;
  case JSONString:
    string = json_value_get_string(value);
    if (string == nullptr) {
      return -1;
    }
    len = json_value_get_string_len(value);
    written = json_serialize_string(string, len, out.cursor);
    if (written < 0) {
      return -1;
    }
    if (out.cursor != nullptr) {
      out.cursor += written;
    }
    out.written_total += written;
    return out.written_total;
  case JSONBoolean: {
    const JSON_Boolean boolean_value = json_value_get_boolean(value);
    if (boolean_value == JSONBooleanError) {
      return -1;
    }
    append_literal(&out, boolean_value == JSONBooleanTrue ? "true" : "false");
    return out.written_total;
  }
  case JSONNumber: {
    num = json_value_get_number(value);
    char *local_num_buf = num_buf;
    if (out.cursor != nullptr) {
      local_num_buf = out.cursor;
    }
    if (parson_number_serialization_function) {
      written = parson_number_serialization_function(num, local_num_buf);
    } else {
      const char *float_format = parson_float_format
                                     ? parson_float_format
                                     : parson_default_float_format;
      written =
          parson_sprintf(local_num_buf, parson_num_buf_size, float_format, num);
    }
    if (written < 0) {
      return -1;
    }
    if (out.cursor != nullptr) {
      out.cursor += written;
    }
    out.written_total += written;
    return out.written_total;
  }
  case JSONNull:
    append_literal(&out, "null");
    return out.written_total;
  case JSONError:
    return -1;
  default:
    return -1;
  }
}

static int json_serialize_string(const char *string, size_t len, char *buf) {
  size_t i = 0;
  char c = '\0';
  serialization_buffer out = {
      .cursor = buf,
      .written_total = 0,
  };
  append_literal(&out, "\"");
  for (i = 0; i < len; i++) {
    c = string[i];
    switch (c) {
    case '\"':
      append_literal(&out, "\\\"");
      break;
    case '\\':
      append_literal(&out, "\\\\");
      break;
    case '\b':
      append_literal(&out, "\\b");
      break;
    case '\f':
      append_literal(&out, "\\f");
      break;
    case '\n':
      append_literal(&out, "\\n");
      break;
    case '\r':
      append_literal(&out, "\\r");
      break;
    case '\t':
      append_literal(&out, "\\t");
      break;
    case '\x00':
      append_literal(&out, "\\u0000");
      break;
    case '\x01':
      append_literal(&out, "\\u0001");
      break;
    case '\x02':
      append_literal(&out, "\\u0002");
      break;
    case '\x03':
      append_literal(&out, "\\u0003");
      break;
    case '\x04':
      append_literal(&out, "\\u0004");
      break;
    case '\x05':
      append_literal(&out, "\\u0005");
      break;
    case '\x06':
      append_literal(&out, "\\u0006");
      break;
    case '\x07':
      append_literal(&out, "\\u0007");
      break;
    case '\x0b':
      append_literal(&out, "\\u000b");
      break;
    case '\x0e':
      append_literal(&out, "\\u000e");
      break;
    case '\x0f':
      append_literal(&out, "\\u000f");
      break;
    case '\x10':
      append_literal(&out, "\\u0010");
      break;
    case '\x11':
      append_literal(&out, "\\u0011");
      break;
    case '\x12':
      append_literal(&out, "\\u0012");
      break;
    case '\x13':
      append_literal(&out, "\\u0013");
      break;
    case '\x14':
      append_literal(&out, "\\u0014");
      break;
    case '\x15':
      append_literal(&out, "\\u0015");
      break;
    case '\x16':
      append_literal(&out, "\\u0016");
      break;
    case '\x17':
      append_literal(&out, "\\u0017");
      break;
    case '\x18':
      append_literal(&out, "\\u0018");
      break;
    case '\x19':
      append_literal(&out, "\\u0019");
      break;
    case '\x1a':
      append_literal(&out, "\\u001a");
      break;
    case '\x1b':
      append_literal(&out, "\\u001b");
      break;
    case '\x1c':
      append_literal(&out, "\\u001c");
      break;
    case '\x1d':
      append_literal(&out, "\\u001d");
      break;
    case '\x1e':
      append_literal(&out, "\\u001e");
      break;
    case '\x1f':
      append_literal(&out, "\\u001f");
      break;
    case '/':
      if (parson_escape_slashes) {
        append_literal(&out, "\\/");
      } else {
        append_literal(&out, "/");
      }
      break;
    default:
      if (out.cursor != nullptr) {
        out.cursor[0] = c;
        out.cursor += 1;
      }
      out.written_total += 1;
      break;
    }
  }
  append_literal(&out, "\"");
  return out.written_total;
}

/* Parser API */
JSON_Value *json_parse_file(const char *filename) {
  char *file_contents = read_file(filename);
  JSON_Value *output_value = nullptr;
  if (file_contents == nullptr) {
    return nullptr;
  }
  output_value = json_parse_string(file_contents);
  parson_free(file_contents);
  return output_value;
}

JSON_Value *json_parse_file_with_comments(const char *filename) {
  char *file_contents = read_file(filename);
  JSON_Value *output_value = nullptr;
  if (file_contents == nullptr) {
    return nullptr;
  }
  output_value = json_parse_string_with_comments(file_contents);
  parson_free(file_contents);
  return output_value;
}

JSON_Value *json_parse_string(const char *string) {
  if (string == nullptr) {
    return nullptr;
  }
  if (string[0] == '\xEF' && string[1] == '\xBB' && string[2] == '\xBF') {
    string = string + 3; /* Support for UTF-8 BOM */
  }
  return parse_value((const char **)&string, 0);
}

JSON_Value *json_parse_string_with_comments(const char *string) {
  JSON_Value *result = nullptr;
  char *string_mutable_copy = nullptr, *string_mutable_copy_ptr = nullptr;
  if (string == nullptr) {
    return nullptr;
  }
  string_mutable_copy = parson_strdup(string);
  if (string_mutable_copy == nullptr) {
    return nullptr;
  }
  remove_comments(string_mutable_copy, "/*", "*/");
  remove_comments(string_mutable_copy, "//", "\n");
  string_mutable_copy_ptr = string_mutable_copy;
  result = parse_value((const char **)&string_mutable_copy_ptr, 0);
  parson_free(string_mutable_copy);
  return result;
}

/* JSON Object API */

JSON_Value *json_object_get_value(const JSON_Object *object, const char *name) {
  if (object == nullptr || name == nullptr) {
    return nullptr;
  }
  return json_object_getn_value(object, name, strlen(name));
}

const char *json_object_get_string(const JSON_Object *object,
                                   const char *name) {
  return json_value_get_string(json_object_get_value(object, name));
}

size_t json_object_get_string_len(const JSON_Object *object, const char *name) {
  return json_value_get_string_len(json_object_get_value(object, name));
}

double json_object_get_number(const JSON_Object *object, const char *name) {
  return json_value_get_number(json_object_get_value(object, name));
}

JSON_Object *json_object_get_object(const JSON_Object *object,
                                    const char *name) {
  return json_value_get_object(json_object_get_value(object, name));
}

JSON_Array *json_object_get_array(const JSON_Object *object, const char *name) {
  return json_value_get_array(json_object_get_value(object, name));
}

JSON_Boolean json_object_get_boolean(const JSON_Object *object,
                                     const char *name) {
  return json_value_get_boolean(json_object_get_value(object, name));
}

JSON_Value *json_object_dotget_value(const JSON_Object *object,
                                     const char *name) {
  if (object == nullptr || name == nullptr) {
    return nullptr;
  }
  const char *dot_position = strchr(name, '.');
  if (dot_position == nullptr) {
    return json_object_get_value(object, name);
  }
  object = json_value_get_object(
      json_object_getn_value(object, name, dot_position - name));
  return json_object_dotget_value(object, dot_position + 1);
}

const char *json_object_dotget_string(const JSON_Object *object,
                                      const char *name) {
  return json_value_get_string(json_object_dotget_value(object, name));
}

size_t json_object_dotget_string_len(const JSON_Object *object,
                                     const char *name) {
  return json_value_get_string_len(json_object_dotget_value(object, name));
}

double json_object_dotget_number(const JSON_Object *object, const char *name) {
  return json_value_get_number(json_object_dotget_value(object, name));
}

JSON_Object *json_object_dotget_object(const JSON_Object *object,
                                       const char *name) {
  return json_value_get_object(json_object_dotget_value(object, name));
}

JSON_Array *json_object_dotget_array(const JSON_Object *object,
                                     const char *name) {
  return json_value_get_array(json_object_dotget_value(object, name));
}

JSON_Boolean json_object_dotget_boolean(const JSON_Object *object,
                                        const char *name) {
  return json_value_get_boolean(json_object_dotget_value(object, name));
}

size_t json_object_get_count(const JSON_Object *object) {
  return object == nullptr ? 0 : object->count;
}

const char *json_object_get_name(const JSON_Object *object, size_t index) {
  if (object == nullptr || index >= json_object_get_count(object)) {
    return nullptr;
  }
  return object->names[index];
}

JSON_Value *json_object_get_value_at(const JSON_Object *object, size_t index) {
  if (object == nullptr || index >= json_object_get_count(object)) {
    return nullptr;
  }
  return object->values[index];
}

JSON_Value *json_object_get_wrapping_value(const JSON_Object *object) {
  if (object == nullptr) {
    return nullptr;
  }
  return object->wrapping_value;
}

bool json_object_has_value(const JSON_Object *object, const char *name) {
  return json_object_get_value(object, name) != nullptr;
}

bool json_object_has_value_of_type(const JSON_Object *object, const char *name,
                                   JSON_Value_Type type) {
  JSON_Value *val = json_object_get_value(object, name);
  return val != nullptr && json_value_get_type(val) == type;
}

bool json_object_dothas_value(const JSON_Object *object, const char *name) {
  return json_object_dotget_value(object, name) != nullptr;
}

bool json_object_dothas_value_of_type(const JSON_Object *object,
                                      const char *name, JSON_Value_Type type) {
  JSON_Value *val = json_object_dotget_value(object, name);
  return val != nullptr && json_value_get_type(val) == type;
}

/* JSON Array API */
JSON_Value *json_array_get_value(const JSON_Array *array, size_t index) {
  if (array == nullptr || index >= json_array_get_count(array)) {
    return nullptr;
  }
  return array->items[index];
}

const char *json_array_get_string(const JSON_Array *array, size_t index) {
  return json_value_get_string(json_array_get_value(array, index));
}

size_t json_array_get_string_len(const JSON_Array *array, size_t index) {
  return json_value_get_string_len(json_array_get_value(array, index));
}

double json_array_get_number(const JSON_Array *array, size_t index) {
  return json_value_get_number(json_array_get_value(array, index));
}

JSON_Object *json_array_get_object(const JSON_Array *array, size_t index) {
  return json_value_get_object(json_array_get_value(array, index));
}

JSON_Array *json_array_get_array(const JSON_Array *array, size_t index) {
  return json_value_get_array(json_array_get_value(array, index));
}

JSON_Boolean json_array_get_boolean(const JSON_Array *array, size_t index) {
  return json_value_get_boolean(json_array_get_value(array, index));
}

size_t json_array_get_count(const JSON_Array *array) {
  return array == nullptr ? 0 : array->count;
}

JSON_Value *json_array_get_wrapping_value(const JSON_Array *array) {
  if (array == nullptr) {
    return nullptr;
  }
  return array->wrapping_value;
}

/* JSON Value API */
JSON_Value_Type json_value_get_type(const JSON_Value *value) {
  return value == nullptr ? JSONError : value->type;
}

JSON_Object *json_value_get_object(const JSON_Value *value) {
  return json_value_get_type(value) == JSONObject ? value->value.object
                                                  : nullptr;
}

JSON_Array *json_value_get_array(const JSON_Value *value) {
  return json_value_get_type(value) == JSONArray ? value->value.array : nullptr;
}

static const JSON_String *json_value_get_string_desc(const JSON_Value *value) {
  return json_value_get_type(value) == JSONString ? &value->value.string
                                                  : nullptr;
}

const char *json_value_get_string(const JSON_Value *value) {
  const JSON_String *str = json_value_get_string_desc(value);
  return str ? str->chars : nullptr;
}

size_t json_value_get_string_len(const JSON_Value *value) {
  const JSON_String *str = json_value_get_string_desc(value);
  return str ? str->length : 0;
}

double json_value_get_number(const JSON_Value *value) {
  return json_value_get_type(value) == JSONNumber ? value->value.number : 0;
}

JSON_Boolean json_value_get_boolean(const JSON_Value *value) {
  if (json_value_get_type(value) != JSONBoolean) {
    return JSONBooleanError;
  }
  return value->value.boolean ? JSONBooleanTrue : JSONBooleanFalse;
}

JSON_Value *json_value_get_parent(const JSON_Value *value) {
  return value == nullptr ? nullptr : value->parent;
}

void json_value_free(JSON_Value *value) {
  switch (json_value_get_type(value)) {
  case JSONObject:
    json_object_free(value->value.object);
    break;
  case JSONString:
    parson_free(value->value.string.chars);
    break;
  case JSONArray:
    json_array_free(value->value.array);
    break;
  default:
    break;
  }
  parson_free(value);
}

JSON_Value *json_value_init_object() {
  auto new_value = (JSON_Value *)parson_calloc(1, sizeof(JSON_Value));
  if (new_value == nullptr) {
    return nullptr;
  }
  new_value->parent = nullptr;
  new_value->type = JSONObject;
  new_value->value.object = json_object_make(new_value);
  if (new_value->value.object == nullptr) {
    parson_free(new_value);
    return nullptr;
  }
  return new_value;
}

JSON_Value *json_value_init_array() {
  auto new_value = (JSON_Value *)parson_calloc(1, sizeof(JSON_Value));
  if (new_value == nullptr) {
    return nullptr;
  }
  new_value->parent = nullptr;
  new_value->type = JSONArray;
  new_value->value.array = json_array_make(new_value);
  if (new_value->value.array == nullptr) {
    parson_free(new_value);
    return nullptr;
  }
  return new_value;
}

JSON_Value *json_value_init_string(const char *string) {
  if (string == nullptr) {
    return nullptr;
  }
  return json_value_init_string_with_len(string, strlen(string));
}

JSON_Value *json_value_init_string_with_len(const char *string, size_t length) {
  char *copy = nullptr;
  JSON_Value *value;
  if (string == nullptr) {
    return nullptr;
  }
  if (!is_valid_utf8(string, length)) {
    return nullptr;
  }
  copy = parson_strndup(string, length);
  if (copy == nullptr) {
    return nullptr;
  }
  value = json_value_init_string_no_copy(copy, length);
  if (value == nullptr) {
    parson_free(copy);
  }
  return value;
}

JSON_Value *json_value_init_number(double number) {
  JSON_Value *new_value = nullptr;
  if (is_number_invalid(number)) {
    return nullptr;
  }
  new_value = (JSON_Value *)parson_calloc(1, sizeof(JSON_Value));
  if (new_value == nullptr) {
    return nullptr;
  }
  new_value->parent = nullptr;
  new_value->type = JSONNumber;
  new_value->value.number = number;
  return new_value;
}

JSON_Value *json_value_init_boolean(bool boolean) {
  auto new_value = (JSON_Value *)parson_calloc(1, sizeof(JSON_Value));
  if (new_value == nullptr) {
    return nullptr;
  }
  new_value->parent = nullptr;
  new_value->type = JSONBoolean;
  new_value->value.boolean = boolean;
  return new_value;
}

JSON_Value *json_value_init_null() {
  auto new_value = (JSON_Value *)parson_calloc(1, sizeof(JSON_Value));
  if (new_value == nullptr) {
    return nullptr;
  }
  new_value->parent = nullptr;
  new_value->type = JSONNull;
  return new_value;
}

JSON_Value *json_value_deep_copy(const JSON_Value *value) {
  size_t i = 0;
  JSON_Value *return_value = nullptr, *temp_value_copy = nullptr,
             *temp_value = nullptr;
  const JSON_String *temp_string = nullptr;
  const char *temp_key = nullptr;
  char *temp_string_copy = nullptr;
  JSON_Array *temp_array = nullptr, *temp_array_copy = nullptr;
  JSON_Object *temp_object = nullptr, *temp_object_copy = nullptr;
  JSON_Status res = JSONFailure;
  char *key_copy = nullptr;

  switch (json_value_get_type(value)) {
  case JSONArray:
    temp_array = json_value_get_array(value);
    return_value = json_value_init_array();
    if (return_value == nullptr) {
      return nullptr;
    }
    temp_array_copy = json_value_get_array(return_value);
    for (i = 0; i < json_array_get_count(temp_array); i++) {
      temp_value = json_array_get_value(temp_array, i);
      temp_value_copy = json_value_deep_copy(temp_value);
      if (temp_value_copy == nullptr) {
        json_value_free(return_value);
        return nullptr;
      }
      if (json_array_add(temp_array_copy, temp_value_copy) != JSONSuccess) {
        json_value_free(return_value);
        json_value_free(temp_value_copy);
        return nullptr;
      }
    }
    return return_value;
  case JSONObject:
    temp_object = json_value_get_object(value);
    return_value = json_value_init_object();
    if (return_value == nullptr) {
      return nullptr;
    }
    temp_object_copy = json_value_get_object(return_value);
    for (i = 0; i < json_object_get_count(temp_object); i++) {
      temp_key = json_object_get_name(temp_object, i);
      temp_value = json_object_get_value(temp_object, temp_key);
      temp_value_copy = json_value_deep_copy(temp_value);
      if (temp_value_copy == nullptr) {
        json_value_free(return_value);
        return nullptr;
      }
      key_copy = parson_strdup(temp_key);
      if (key_copy == nullptr) {
        json_value_free(temp_value_copy);
        json_value_free(return_value);
        return nullptr;
      }
      res = json_object_add(temp_object_copy, key_copy, temp_value_copy);
      if (res != JSONSuccess) {
        parson_free(key_copy);
        json_value_free(temp_value_copy);
        json_value_free(return_value);
        return nullptr;
      }
    }
    return return_value;
  case JSONBoolean:
    return json_value_init_boolean(json_value_get_boolean(value));
  case JSONNumber:
    return json_value_init_number(json_value_get_number(value));
  case JSONString:
    temp_string = json_value_get_string_desc(value);
    if (temp_string == nullptr) {
      return nullptr;
    }
    temp_string_copy = parson_strndup(temp_string->chars, temp_string->length);
    if (temp_string_copy == nullptr) {
      return nullptr;
    }
    return_value =
        json_value_init_string_no_copy(temp_string_copy, temp_string->length);
    if (return_value == nullptr) {
      parson_free(temp_string_copy);
    }
    return return_value;
  case JSONNull:
    return json_value_init_null();
  case JSONError:
    return nullptr;
  default:
    return nullptr;
  }
}

size_t json_serialization_size(const JSON_Value *value) {
  char
      num_buf[parson_num_buf_size]; /* recursively allocating buffer on stack is
                                       a bad idea, so let's do it only once */
  int res = json_serialize_to_buffer_r(value, nullptr, 0, false, num_buf);
  return res < 0 ? 0 : (size_t)(res) + 1;
}

JSON_Status json_serialize_to_buffer(const JSON_Value *value, char *buf,
                                     size_t buf_size_in_bytes) {
  int written = -1;
  size_t needed_size_in_bytes = json_serialization_size(value);
  if (needed_size_in_bytes == 0 || buf_size_in_bytes < needed_size_in_bytes) {
    return JSONFailure;
  }
  written = json_serialize_to_buffer_r(value, buf, 0, false, nullptr);
  if (written < 0) {
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_serialize_to_file(const JSON_Value *value,
                                   const char *filename) {
  JSON_Status return_code = JSONSuccess;
  FILE *fp = nullptr;
  char *serialized_string = json_serialize_to_string(value);
  if (serialized_string == nullptr) {
    return JSONFailure;
  }
  fp = fopen(filename, "w");
  if (fp == nullptr) {
    json_free_serialized_string(serialized_string);
    return JSONFailure;
  }
  if (fputs(serialized_string, fp) == EOF) {
    return_code = JSONFailure;
  }
  if (fclose(fp) == EOF) {
    return_code = JSONFailure;
  }
  json_free_serialized_string(serialized_string);
  return return_code;
}

char *json_serialize_to_string(const JSON_Value *value) {
  JSON_Status serialization_result = JSONFailure;
  const size_t buf_size_bytes = json_serialization_size(value);
  char *buf = nullptr;
  if (buf_size_bytes == 0) {
    return nullptr;
  }
  buf = (char *)parson_calloc(buf_size_bytes, sizeof(char));
  if (buf == nullptr) {
    return nullptr;
  }
  serialization_result = json_serialize_to_buffer(value, buf, buf_size_bytes);
  if (serialization_result != JSONSuccess) {
    json_free_serialized_string(buf);
    return nullptr;
  }
  return buf;
}

size_t json_serialization_size_pretty(const JSON_Value *value) {
  char
      num_buf[parson_num_buf_size]; /* recursively allocating buffer on stack is
                                       a bad idea, so let's do it only once */
  int res = json_serialize_to_buffer_r(value, nullptr, 0, true, num_buf);
  return res < 0 ? 0 : (size_t)(res) + 1;
}

JSON_Status json_serialize_to_buffer_pretty(const JSON_Value *value, char *buf,
                                            size_t buf_size_in_bytes) {
  int written = -1;
  size_t needed_size_in_bytes = json_serialization_size_pretty(value);
  if (needed_size_in_bytes == 0 || buf_size_in_bytes < needed_size_in_bytes) {
    return JSONFailure;
  }
  written = json_serialize_to_buffer_r(value, buf, 0, true, nullptr);
  if (written < 0) {
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_serialize_to_file_pretty(const JSON_Value *value,
                                          const char *filename) {
  JSON_Status return_code = JSONSuccess;
  FILE *fp = nullptr;
  char *serialized_string = json_serialize_to_string_pretty(value);
  if (serialized_string == nullptr) {
    return JSONFailure;
  }
  fp = fopen(filename, "w");
  if (fp == nullptr) {
    json_free_serialized_string(serialized_string);
    return JSONFailure;
  }
  if (fputs(serialized_string, fp) == EOF) {
    return_code = JSONFailure;
  }
  if (fclose(fp) == EOF) {
    return_code = JSONFailure;
  }
  json_free_serialized_string(serialized_string);
  return return_code;
}

char *json_serialize_to_string_pretty(const JSON_Value *value) {
  JSON_Status serialization_result = JSONFailure;
  const size_t buf_size_bytes = json_serialization_size_pretty(value);
  char *buf = nullptr;
  if (buf_size_bytes == 0) {
    return nullptr;
  }
  buf = (char *)parson_calloc(buf_size_bytes, sizeof(char));
  if (buf == nullptr) {
    return nullptr;
  }
  serialization_result =
      json_serialize_to_buffer_pretty(value, buf, buf_size_bytes);
  if (serialization_result != JSONSuccess) {
    json_free_serialized_string(buf);
    return nullptr;
  }
  return buf;
}

void json_free_serialized_string(char *string) { parson_free(string); }

JSON_Status json_array_remove(JSON_Array *array, size_t ix) {
  size_t to_move_bytes = 0;
  if (array == nullptr || ix >= json_array_get_count(array)) {
    return JSONFailure;
  }
  json_value_free(json_array_get_value(array, ix));
  to_move_bytes = (json_array_get_count(array) - 1 - ix) * sizeof(JSON_Value *);
  memmove(array->items + ix, array->items + ix + 1, to_move_bytes);
  array->count -= 1;
  return JSONSuccess;
}

JSON_Status json_array_replace_value(JSON_Array *array, size_t ix,
                                     JSON_Value *value) {
  if (array == nullptr || value == nullptr || value->parent != nullptr ||
      ix >= json_array_get_count(array)) {
    return JSONFailure;
  }
  json_value_free(json_array_get_value(array, ix));
  value->parent = json_array_get_wrapping_value(array);
  array->items[ix] = value;
  return JSONSuccess;
}

JSON_Status json_array_replace_string(JSON_Array *array, size_t i,
                                      const char *string) {
  JSON_Value *value = json_value_init_string(string);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_array_replace_value(array, i, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_array_replace_string_with_len(JSON_Array *array, size_t i,
                                               const char *string, size_t len) {
  JSON_Value *value = json_value_init_string_with_len(string, len);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_array_replace_value(array, i, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_array_replace_number(JSON_Array *array, size_t i,
                                      double number) {
  JSON_Value *value = json_value_init_number(number);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_array_replace_value(array, i, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_array_replace_boolean(JSON_Array *array, size_t i,
                                       bool boolean) {
  JSON_Value *value = json_value_init_boolean(boolean);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_array_replace_value(array, i, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_array_replace_null(JSON_Array *array, size_t i) {
  JSON_Value *value = json_value_init_null();
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_array_replace_value(array, i, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_array_clear(JSON_Array *array) {
  size_t i = 0;
  if (array == nullptr) {
    return JSONFailure;
  }
  for (i = 0; i < json_array_get_count(array); i++) {
    json_value_free(json_array_get_value(array, i));
  }
  array->count = 0;
  return JSONSuccess;
}

JSON_Status json_array_append_value(JSON_Array *array, JSON_Value *value) {
  if (array == nullptr || value == nullptr || value->parent != nullptr) {
    return JSONFailure;
  }
  return json_array_add(array, value);
}

JSON_Status json_array_append_string(JSON_Array *array, const char *string) {
  JSON_Value *value = json_value_init_string(string);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_array_append_value(array, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_array_append_string_with_len(JSON_Array *array,
                                              const char *string, size_t len) {
  JSON_Value *value = json_value_init_string_with_len(string, len);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_array_append_value(array, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_array_append_number(JSON_Array *array, double number) {
  JSON_Value *value = json_value_init_number(number);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_array_append_value(array, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_array_append_boolean(JSON_Array *array, bool boolean) {
  JSON_Value *value = json_value_init_boolean(boolean);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_array_append_value(array, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_array_append_null(JSON_Array *array) {
  JSON_Value *value = json_value_init_null();
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_array_append_value(array, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_object_set_value(JSON_Object *object, const char *name,
                                  JSON_Value *value) {
  unsigned long hash = 0;
  bool found = false;
  size_t cell_ix = 0;
  size_t item_ix = 0;
  JSON_Value *old_value = nullptr;
  char *key_copy = nullptr;

  if (object == nullptr || name == nullptr || value == nullptr ||
      value->parent != nullptr) {
    return JSONFailure;
  }
  hash = hash_string(name, strlen(name));
  found = false;
  cell_ix = json_object_get_cell_ix(object, name, strlen(name), hash, &found);
  if (found) {
    item_ix = object->cells[cell_ix];
    old_value = object->values[item_ix];
    json_value_free(old_value);
    object->values[item_ix] = value;
    value->parent = json_object_get_wrapping_value(object);
    return JSONSuccess;
  }
  if (object->count >= object->item_capacity) {
    JSON_Status res = json_object_grow_and_rehash(object);
    if (res != JSONSuccess) {
      return JSONFailure;
    }
    cell_ix = json_object_get_cell_ix(object, name, strlen(name), hash, &found);
  }
  key_copy = parson_strdup(name);
  if (key_copy == nullptr) {
    return JSONFailure;
  }
  object->names[object->count] = key_copy;
  object->cells[cell_ix] = object->count;
  object->values[object->count] = value;
  object->cell_ixs[object->count] = cell_ix;
  object->hashes[object->count] = hash;
  object->count++;
  value->parent = json_object_get_wrapping_value(object);
  return JSONSuccess;
}

JSON_Status json_object_set_string(JSON_Object *object, const char *name,
                                   const char *string) {
  JSON_Value *value = json_value_init_string(string);
  JSON_Status status = json_object_set_value(object, name, value);
  if (status != JSONSuccess) {
    json_value_free(value);
  }
  return status;
}

JSON_Status json_object_set_string_with_len(JSON_Object *object,
                                            const char *name,
                                            const char *string, size_t len) {
  JSON_Value *value = json_value_init_string_with_len(string, len);
  JSON_Status status = json_object_set_value(object, name, value);
  if (status != JSONSuccess) {
    json_value_free(value);
  }
  return status;
}

JSON_Status json_object_set_number(JSON_Object *object, const char *name,
                                   double number) {
  JSON_Value *value = json_value_init_number(number);
  JSON_Status status = json_object_set_value(object, name, value);
  if (status != JSONSuccess) {
    json_value_free(value);
  }
  return status;
}

JSON_Status json_object_set_boolean(JSON_Object *object, const char *name,
                                    bool boolean) {
  JSON_Value *value = json_value_init_boolean(boolean);
  JSON_Status status = json_object_set_value(object, name, value);
  if (status != JSONSuccess) {
    json_value_free(value);
  }
  return status;
}

JSON_Status json_object_set_null(JSON_Object *object, const char *name) {
  JSON_Value *value = json_value_init_null();
  JSON_Status status = json_object_set_value(object, name, value);
  if (status != JSONSuccess) {
    json_value_free(value);
  }
  return status;
}

JSON_Status json_object_dotset_value(JSON_Object *object, const char *name,
                                     JSON_Value *value) {
  const char *dot_pos = nullptr;
  JSON_Value *temp_value = nullptr, *new_value = nullptr;
  JSON_Object *temp_object = nullptr, *new_object = nullptr;
  JSON_Status status = JSONFailure;
  size_t name_len = 0;
  char *name_copy = nullptr;

  if (object == nullptr || name == nullptr || value == nullptr ||
      value->parent != nullptr) {
    return JSONFailure;
  }
  dot_pos = strchr(name, '.');
  if (dot_pos == nullptr) {
    return json_object_set_value(object, name, value);
  }
  name_len = dot_pos - name;
  temp_value = json_object_getn_value(object, name, name_len);
  if (temp_value != nullptr) {
    /* Don't overwrite existing non-object (unlike json_object_set_value, but it
     * shouldn't be changed at this point) */
    if (json_value_get_type(temp_value) != JSONObject) {
      return JSONFailure;
    }
    temp_object = json_value_get_object(temp_value);
    return json_object_dotset_value(temp_object, dot_pos + 1, value);
  }
  new_value = json_value_init_object();
  if (new_value == nullptr) {
    return JSONFailure;
  }
  new_object = json_value_get_object(new_value);
  status = json_object_dotset_value(new_object, dot_pos + 1, value);
  if (status != JSONSuccess) {
    json_value_free(new_value);
    return JSONFailure;
  }
  name_copy = parson_strndup(name, name_len);
  if (name_copy == nullptr) {
    json_object_dotremove_internal(new_object, dot_pos + 1, false);
    json_value_free(new_value);
    return JSONFailure;
  }
  status = json_object_add(object, name_copy, new_value);
  if (status != JSONSuccess) {
    parson_free(name_copy);
    json_object_dotremove_internal(new_object, dot_pos + 1, false);
    json_value_free(new_value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_object_dotset_string(JSON_Object *object, const char *name,
                                      const char *string) {
  JSON_Value *value = json_value_init_string(string);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_object_dotset_value(object, name, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_object_dotset_string_with_len(JSON_Object *object,
                                               const char *name,
                                               const char *string, size_t len) {
  JSON_Value *value = json_value_init_string_with_len(string, len);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_object_dotset_value(object, name, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_object_dotset_number(JSON_Object *object, const char *name,
                                      double number) {
  JSON_Value *value = json_value_init_number(number);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_object_dotset_value(object, name, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_object_dotset_boolean(JSON_Object *object, const char *name,
                                       bool boolean) {
  JSON_Value *value = json_value_init_boolean(boolean);
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_object_dotset_value(object, name, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_object_dotset_null(JSON_Object *object, const char *name) {
  JSON_Value *value = json_value_init_null();
  if (value == nullptr) {
    return JSONFailure;
  }
  if (json_object_dotset_value(object, name, value) != JSONSuccess) {
    json_value_free(value);
    return JSONFailure;
  }
  return JSONSuccess;
}

JSON_Status json_object_remove(JSON_Object *object, const char *name) {
  return json_object_remove_internal(object, name, true);
}

JSON_Status json_object_dotremove(JSON_Object *object, const char *name) {
  return json_object_dotremove_internal(object, name, true);
}

JSON_Status json_object_clear(JSON_Object *object) {
  size_t i = 0;
  if (object == nullptr) {
    return JSONFailure;
  }
  for (i = 0; i < json_object_get_count(object); i++) {
    parson_free(object->names[i]);
    object->names[i] = nullptr;

    json_value_free(object->values[i]);
    object->values[i] = nullptr;
  }
  object->count = 0;
  for (i = 0; i < object->cell_capacity; i++) {
    object->cells[i] = object_invalid_ix;
  }
  return JSONSuccess;
}

JSON_Status json_validate(const JSON_Value *schema, const JSON_Value *value) {
  JSON_Value *temp_schema_value = nullptr, *temp_value = nullptr;
  JSON_Array *schema_array = nullptr, *value_array = nullptr;
  JSON_Object *schema_object = nullptr, *value_object = nullptr;
  JSON_Value_Type schema_type = JSONError, value_type = JSONError;
  const char *key = nullptr;
  size_t i = 0, count = 0;
  if (schema == nullptr || value == nullptr) {
    return JSONFailure;
  }
  schema_type = json_value_get_type(schema);
  value_type = json_value_get_type(value);
  if (schema_type != value_type &&
      schema_type != JSONNull) { /* null represents all values */
    return JSONFailure;
  }
  switch (schema_type) {
  case JSONArray:
    schema_array = json_value_get_array(schema);
    value_array = json_value_get_array(value);
    count = json_array_get_count(schema_array);
    if (count == 0) {
      return JSONSuccess; /* Empty array allows all types */
    }
    /* Get first value from array, rest is ignored */
    temp_schema_value = json_array_get_value(schema_array, 0);
    for (i = 0; i < json_array_get_count(value_array); i++) {
      temp_value = json_array_get_value(value_array, i);
      if (json_validate(temp_schema_value, temp_value) != JSONSuccess) {
        return JSONFailure;
      }
    }
    return JSONSuccess;
  case JSONObject:
    schema_object = json_value_get_object(schema);
    value_object = json_value_get_object(value);
    count = json_object_get_count(schema_object);
    if (count == 0) {
      return JSONSuccess; /* Empty object allows all objects */
    } else if (json_object_get_count(value_object) < count) {
      return JSONFailure; /* Tested object mustn't have less name-value pairs
                             than schema */
    }
    for (i = 0; i < count; i++) {
      key = json_object_get_name(schema_object, i);
      temp_schema_value = json_object_get_value(schema_object, key);
      temp_value = json_object_get_value(value_object, key);
      if (temp_value == nullptr) {
        return JSONFailure;
      }
      if (json_validate(temp_schema_value, temp_value) != JSONSuccess) {
        return JSONFailure;
      }
    }
    return JSONSuccess;
  case JSONString:
  case JSONNumber:
  case JSONBoolean:
  case JSONNull:
    return JSONSuccess; /* equality already tested before switch */
  case JSONError:
  default:
    return JSONFailure;
  }
}

bool json_value_equals(const JSON_Value *a, const JSON_Value *b) {
  JSON_Object *a_object = nullptr, *b_object = nullptr;
  JSON_Array *a_array = nullptr, *b_array = nullptr;
  const JSON_String *a_string = nullptr, *b_string = nullptr;
  const char *key = nullptr;
  size_t a_count = 0, b_count = 0, i = 0;
  JSON_Value_Type a_type, b_type;
  a_type = json_value_get_type(a);
  b_type = json_value_get_type(b);
  if (a_type != b_type) {
    return false;
  }
  switch (a_type) {
  case JSONArray:
    a_array = json_value_get_array(a);
    b_array = json_value_get_array(b);
    a_count = json_array_get_count(a_array);
    b_count = json_array_get_count(b_array);
    if (a_count != b_count) {
      return false;
    }
    for (i = 0; i < a_count; i++) {
      if (!json_value_equals(json_array_get_value(a_array, i),
                             json_array_get_value(b_array, i))) {
        return false;
      }
    }
    return true;
  case JSONObject:
    a_object = json_value_get_object(a);
    b_object = json_value_get_object(b);
    a_count = json_object_get_count(a_object);
    b_count = json_object_get_count(b_object);
    if (a_count != b_count) {
      return false;
    }
    for (i = 0; i < a_count; i++) {
      key = json_object_get_name(a_object, i);
      if (!json_value_equals(json_object_get_value(a_object, key),
                             json_object_get_value(b_object, key))) {
        return false;
      }
    }
    return true;
  case JSONString:
    a_string = json_value_get_string_desc(a);
    b_string = json_value_get_string_desc(b);
    if (a_string == nullptr || b_string == nullptr) {
      return false; /* shouldn't happen */
    }
    return a_string->length == b_string->length &&
           memcmp(a_string->chars, b_string->chars, a_string->length) == 0;
  case JSONBoolean:
    return json_value_get_boolean(a) == json_value_get_boolean(b);
  case JSONNumber:
    return fabs(json_value_get_number(a) - json_value_get_number(b)) <
           json_number_epsilon;
  case JSONError:
    return true;
  case JSONNull:
    return true;
  default:
    return true;
  }
}

JSON_Value_Type json_type(const JSON_Value *value) {
  return json_value_get_type(value);
}

JSON_Object *json_object(const JSON_Value *value) {
  return json_value_get_object(value);
}

JSON_Array *json_array(const JSON_Value *value) {
  return json_value_get_array(value);
}

const char *json_string(const JSON_Value *value) {
  return json_value_get_string(value);
}

size_t json_string_len(const JSON_Value *value) {
  return json_value_get_string_len(value);
}

double json_number(const JSON_Value *value) {
  return json_value_get_number(value);
}

JSON_Boolean json_boolean(const JSON_Value *value) {
  return json_value_get_boolean(value);
}

void json_set_allocation_functions(JSON_Malloc_Function malloc_fun,
                                   JSON_Free_Function free_fun) {
  if (malloc_fun == nullptr || free_fun == nullptr) {
    return;
  }
  parson_malloc = malloc_fun;
  parson_free = free_fun;
}

void json_set_escape_slashes(bool escape_slashes) {
  parson_escape_slashes = escape_slashes;
}

void json_set_float_serialization_format(const char *format) {
  if (parson_float_format != nullptr) {
    parson_free(parson_float_format);
    parson_float_format = nullptr;
  }
  if (format == nullptr) {
    parson_float_format = nullptr;
    return;
  }
  parson_float_format = parson_strdup(format);
}

void json_set_number_serialization_function(
    JSON_Number_Serialization_Function func) {
  parson_number_serialization_function = func;
}
