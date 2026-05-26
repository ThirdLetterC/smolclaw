/*
 SPDX-License-Identifier: MIT

 Parson (https://github.com/kgabis/parson)
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
#define _CRT_SECURE_NO_WARNINGS
#endif

#if defined(__APPLE__) && defined(__clang__)
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
#endif

#include "parson/parson.h"

#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TEST(A)                                                                \
  do {                                                                         \
    if (A) {                                                                   \
      g_tests_passed++;                                                        \
    } else {                                                                   \
      printf("%d %-72s - FAILED\n", __LINE__, #A);                             \
      g_tests_failed++;                                                        \
    }                                                                          \
  } while (0)

#define STREQ(A, B) ((A) && (B) ? strcmp((A), (B)) == 0 : 0)
constexpr double DBL_EPSILON_VALUE = 2.2204460492503131e-16;
static inline bool dbl_eq(double a, double b) {
  return fabs(a - b) < DBL_EPSILON_VALUE;
}

void test_suite_1(); /* Test 3 files from json.org + serialization*/
void test_suite_2(JSON_Value *value); /* Test correctness of parsed values */
void test_suite_2_no_comments();
void test_suite_2_with_comments();
void test_suite_3();  /* Test parsing valid and invalid strings */
void test_suite_4();  /* Test deep copy function */
void test_suite_5();  /* Test building json values from scratch */
void test_suite_6();  /* Test value comparing verification */
void test_suite_7();  /* Test schema validation */
void test_suite_8();  /* Test serialization */
void test_suite_9();  /* Test serialization (pretty) */
void test_suite_10(); /* Testing for memory leaks */
void test_suite_11(); /* Additional things that require testing */
void test_memory_leaks();
void test_failing_allocations();
void test_custom_number_format();
void test_custom_number_serialization_function();
void test_object_clear();
void test_allocation_functions_switch();

void print_commits_info(const char *username, const char *repo);
void persistence_example();
void serialization_example();

static constexpr char default_tests_path[] = "tests";
static const char *g_tests_path = default_tests_path;

static int g_malloc_count = 0;
static void *counted_malloc(size_t size);
static void counted_free(void *ptr);

typedef struct failing_alloc {
  int allocation_to_fail;
  int alloc_count;
  int total_count;
  bool has_failed;
  bool should_fail;
} failing_alloc_t;

static failing_alloc_t g_failing_alloc;

static void *failing_malloc(size_t size);
static void failing_free(void *ptr);

[[nodiscard]] static char *read_file(const char *filename);
const char *get_file_path(const char *filename);

static int g_tests_passed;
static int g_tests_failed;

#ifdef TESTS_MAIN
int main(int argc, char *argv[]) {
#else
int tests_main(int argc, char *argv[]);
int tests_main(int argc, char *argv[]) {
#endif
#if 0 /* unconfuse xcode */
}
#endif
  /* Example functions from readme file:      */
  /* print_commits_info("torvalds", "linux"); */
  /* serialization_example(); */
  /* persistence_example(); */

  puts("#######################################################################"
       "#########");
  puts("Running parson tests");

  if (argc == 2) {
    g_tests_path = argv[1];
  } else {
    g_tests_path = "tests";
  }

  json_set_allocation_functions(counted_malloc, counted_free);
  test_suite_1();
  test_suite_2_no_comments();
  test_suite_2_with_comments();
  test_suite_3();
  test_suite_4();
  test_suite_5();
  test_suite_6();
  test_suite_7();
  test_suite_8();
  test_suite_9();
  test_suite_10();
  test_suite_11();
  test_memory_leaks();
  test_failing_allocations();
  test_custom_number_format();
  test_custom_number_serialization_function();
  test_object_clear();
  test_allocation_functions_switch();

  printf("Tests failed: %d\n", g_tests_failed);
  printf("Tests passed: %d\n", g_tests_passed);
  puts("#######################################################################"
       "#########");
  return 0;
}

void test_suite_1() {
  JSON_Value *val;
  TEST((val = json_parse_file(get_file_path("test_1_1.txt"))) != nullptr);
  TEST(
      json_value_equals(json_parse_string(json_serialize_to_string(val)), val));
  TEST(json_value_equals(
      json_parse_string(json_serialize_to_string_pretty(val)), val));
  if (val) {
    json_value_free(val);
  }

  TEST((val = json_parse_file(get_file_path("test_1_2.txt"))) ==
       nullptr); /* Over 2048 levels of nesting */
  if (val) {
    json_value_free(val);
  }

  TEST((val = json_parse_file(get_file_path("test_1_3.txt"))) != nullptr);
  TEST(
      json_value_equals(json_parse_string(json_serialize_to_string(val)), val));
  TEST(json_value_equals(
      json_parse_string(json_serialize_to_string_pretty(val)), val));
  if (val) {
    json_value_free(val);
  }

  TEST((val = json_parse_file_with_comments(get_file_path("test_1_1.txt"))) !=
       nullptr);
  TEST(
      json_value_equals(json_parse_string(json_serialize_to_string(val)), val));
  TEST(json_value_equals(
      json_parse_string(json_serialize_to_string_pretty(val)), val));
  if (val) {
    json_value_free(val);
  }

  TEST((val = json_parse_file_with_comments(get_file_path("test_1_2.txt"))) ==
       nullptr); /* Over 2048 levels of nesting */
  if (val) {
    json_value_free(val);
  }

  TEST((val = json_parse_file_with_comments(get_file_path("test_1_3.txt"))) !=
       nullptr);
  TEST(
      json_value_equals(json_parse_string(json_serialize_to_string(val)), val));
  TEST(json_value_equals(
      json_parse_string(json_serialize_to_string_pretty(val)), val));
  if (val) {
    json_value_free(val);
  }
}

void test_suite_2(JSON_Value *root_value) {
  JSON_Object *root_object;
  JSON_Array *array;
  JSON_Value *array_value;
  size_t len;
  size_t i;
  TEST(root_value);
  TEST(json_value_get_type(root_value) == JSONObject);
  root_object = json_value_get_object(root_value);

  TEST(json_object_has_value(root_object, "string"));
  TEST(!json_object_has_value(root_object, "_string"));
  TEST(json_object_has_value_of_type(root_object, "object", JSONObject));
  TEST(!json_object_has_value_of_type(root_object, "string array", JSONObject));
  TEST(json_object_has_value_of_type(root_object, "string array", JSONArray));
  TEST(!json_object_has_value_of_type(root_object, "object", JSONArray));
  TEST(json_object_has_value_of_type(root_object, "string", JSONString));
  TEST(!json_object_has_value_of_type(root_object, "positive one", JSONString));
  TEST(json_object_has_value_of_type(root_object, "positive one", JSONNumber));
  TEST(!json_object_has_value_of_type(root_object, "string", JSONNumber));
  TEST(json_object_has_value_of_type(root_object, "boolean true", JSONBoolean));
  TEST(
      !json_object_has_value_of_type(root_object, "positive one", JSONBoolean));
  TEST(json_object_has_value_of_type(root_object, "null", JSONNull));
  TEST(!json_object_has_value_of_type(root_object, "object", JSONNull));

  TEST(json_object_dothas_value(root_object, "object.nested array"));
  TEST(!json_object_dothas_value(root_object, "_object.nested array"));
  TEST(json_object_dothas_value_of_type(root_object, "object.nested object",
                                        JSONObject));
  TEST(!json_object_dothas_value_of_type(root_object, "object.nested array",
                                         JSONObject));
  TEST(json_object_dothas_value_of_type(root_object, "object.nested array",
                                        JSONArray));
  TEST(!json_object_dothas_value_of_type(root_object, "object.nested object",
                                         JSONArray));
  TEST(json_object_dothas_value_of_type(root_object, "object.nested string",
                                        JSONString));
  TEST(!json_object_dothas_value_of_type(root_object, "object.nested number",
                                         JSONString));
  TEST(json_object_dothas_value_of_type(root_object, "object.nested number",
                                        JSONNumber));
  TEST(!json_object_dothas_value_of_type(root_object, "_object.nested whatever",
                                         JSONNumber));
  TEST(json_object_dothas_value_of_type(root_object, "object.nested true",
                                        JSONBoolean));
  TEST(!json_object_dothas_value_of_type(root_object, "object.nested number",
                                         JSONBoolean));
  TEST(json_object_dothas_value_of_type(root_object, "object.nested null",
                                        JSONNull));
  TEST(!json_object_dothas_value_of_type(root_object, "object.nested object",
                                         JSONNull));

  TEST(STREQ(json_object_get_string(root_object, "string"), "lorem ipsum"));
  TEST(STREQ(json_object_get_string(root_object, "utf string"), "lorem ipsum"));
  TEST(
      STREQ(json_object_get_string(root_object, "utf-8 string"), "あいうえお"));
  TEST(STREQ(json_object_get_string(root_object, "surrogate string"),
             "lorem𝄞ipsum𝍧lorem"));

  len = json_object_get_string_len(root_object, "string with null");
  TEST(len == 7);
  TEST(memcmp(json_object_get_string(root_object, "string with null"),
              "abc\0def", len) == 0);

  TEST(dbl_eq(json_object_get_number(root_object, "positive one"), 1.0));
  TEST(dbl_eq(json_object_get_number(root_object, "negative one"), -1.0));
  TEST(dbl_eq(json_object_get_number(root_object, "hard to parse number"),
              -0.000314));
  TEST(json_object_get_boolean(root_object, "boolean true") == JSONBooleanTrue);
  TEST(json_object_get_boolean(root_object, "boolean false") ==
       JSONBooleanFalse);
  TEST(json_value_get_type(json_object_get_value(root_object, "null")) ==
       JSONNull);

  array = json_object_get_array(root_object, "string array");
  if (array != nullptr && json_array_get_count(array) > 1) {
    TEST(STREQ(json_array_get_string(array, 0), "lorem"));
    TEST(STREQ(json_array_get_string(array, 1), "ipsum"));
  } else {
    g_tests_failed++;
  }

  array = json_object_get_array(root_object, "x^2 array");
  if (array != nullptr) {
    for (i = 0; i < json_array_get_count(array); i++) {
      TEST(dbl_eq(json_array_get_number(array, i), (i * i)));
    }
  } else {
    g_tests_failed++;
  }

  TEST(json_object_get_array(root_object, "non existent array") == nullptr);
  TEST(STREQ(json_object_dotget_string(root_object, "object.nested string"),
             "str"));
  TEST(json_object_dotget_boolean(root_object, "object.nested true") ==
       JSONBooleanTrue);
  TEST(json_object_dotget_boolean(root_object, "object.nested false") ==
       JSONBooleanFalse);
  TEST(json_object_dotget_value(root_object, "object.nested null") != nullptr);
  TEST(dbl_eq(json_object_dotget_number(root_object, "object.nested number"),
              123));

  TEST(json_object_dotget_value(root_object, "should.be.null") == nullptr);
  TEST(json_object_dotget_value(root_object, "should.be.null.") == nullptr);
  TEST(json_object_dotget_value(root_object, ".") == nullptr);
  TEST(json_object_dotget_value(root_object, "") == nullptr);

  array = json_object_dotget_array(root_object, "object.nested array");
  TEST(array != nullptr);
  TEST(json_array_get_count(array) > 1);
  if (array != nullptr && json_array_get_count(array) > 1) {
    TEST(STREQ(json_array_get_string(array, 0), "lorem"));
    TEST(STREQ(json_array_get_string(array, 1), "ipsum"));
  }
  TEST(json_object_dotget_boolean(root_object, "object.nested true") ==
       JSONBooleanTrue);

  TEST(STREQ(json_object_get_string(root_object, "/**/"), "comment"));
  TEST(STREQ(json_object_get_string(root_object, "//"), "comment"));
  TEST(STREQ(json_object_get_string(root_object, "url"),
             "https://www.example.com/search?q=12345"));
  TEST(STREQ(json_object_get_string(root_object, "escaped chars"), "\" \\ /"));

  TEST(json_object_get_object(root_object, "empty object") != nullptr);
  TEST(json_object_get_array(root_object, "empty array") != nullptr);

  TEST(json_object_get_wrapping_value(root_object) == root_value);
  array = json_object_get_array(root_object, "string array");
  array_value = json_object_get_value(root_object, "string array");
  TEST(json_array_get_wrapping_value(array) == array_value);
  TEST(json_value_get_parent(array_value) == root_value);
  TEST(json_value_get_parent(root_value) == nullptr);
}

void test_suite_2_no_comments() {
  const char *filename = "test_2.txt";
  JSON_Value *root_value = nullptr;
  root_value = json_parse_file(get_file_path(filename));
  test_suite_2(root_value);
  TEST(json_value_equals(
      root_value, json_parse_string(json_serialize_to_string(root_value))));
  TEST(json_value_equals(
      root_value,
      json_parse_string(json_serialize_to_string_pretty(root_value))));
  json_value_free(root_value);
}

void test_suite_2_with_comments() {
  const char *filename = "test_2_comments.txt";
  JSON_Value *root_value = nullptr;
  root_value = json_parse_file_with_comments(get_file_path(filename));
  test_suite_2(root_value);
  TEST(json_value_equals(
      root_value, json_parse_string(json_serialize_to_string(root_value))));
  TEST(json_value_equals(
      root_value,
      json_parse_string(json_serialize_to_string_pretty(root_value))));
  json_value_free(root_value);
}

void test_suite_3() {
  /* Testing valid strings */
  TEST(json_parse_string("{\"lorem\":\"ipsum\"}") != nullptr);
  TEST(json_parse_string("[\"lorem\"]") != nullptr);
  TEST(json_parse_string("null") != nullptr);
  TEST(json_parse_string("true") != nullptr);
  TEST(json_parse_string("false") != nullptr);
  TEST(json_parse_string("\"string\"") != nullptr);
  TEST(json_parse_string("123") != nullptr);
  TEST(json_parse_string("[\"lorem\",]") != nullptr);
  TEST(json_parse_string("{\"lorem\":\"ipsum\",}") != nullptr);

  /* Test UTF-16 parsing */
  TEST(STREQ(json_string(json_parse_string("\"\\u0024x\"")), "$x"));
  TEST(STREQ(json_string(json_parse_string("\"\\u00A2x\"")), "¢x"));
  TEST(STREQ(json_string(json_parse_string("\"\\u20ACx\"")), "€x"));
  TEST(STREQ(json_string(json_parse_string("\"\\uD801\\uDC37x\"")), "𐐷x"));

  /* Testing invalid strings */
  g_malloc_count = 0;
  TEST(json_parse_string(nullptr) == nullptr);
  TEST(json_parse_string("") == nullptr); /* empty string */
  TEST(json_parse_string("{lorem:ipsum}") == nullptr);
  TEST(json_parse_string("{\"lorem\":\"ipsum\",]") == nullptr);
  TEST(json_parse_string("{\"lorem\":\"ipsum\",,}") == nullptr);
  TEST(json_parse_string("[,]") == nullptr);
  TEST(json_parse_string("[,") == nullptr);
  TEST(json_parse_string("[") == nullptr);
  TEST(json_parse_string("]") == nullptr);
  TEST(json_parse_string("{\"a\":0,\"a\":0}") == nullptr); /* duplicate keys */
  TEST(json_parse_string("{:,}") == nullptr);
  TEST(json_parse_string("{,}") == nullptr);
  TEST(json_parse_string("{,") == nullptr);
  TEST(json_parse_string("{:") == nullptr);
  TEST(json_parse_string("{") == nullptr);
  TEST(json_parse_string("}") == nullptr);
  TEST(json_parse_string("x") == nullptr);
  TEST(json_parse_string("{:\"no name\"}") == nullptr);
  TEST(json_parse_string("[,\"no first value\"]") == nullptr);
  TEST(json_parse_string("{\"key\"\"value\"}") == nullptr);
  TEST(json_parse_string("{\"a\"}") == nullptr);
  TEST(json_parse_string("[\"\\u00zz\"]") == nullptr); /* invalid utf value */
  TEST(json_parse_string("[\"\\u00\"]") == nullptr);   /* invalid utf value */
  TEST(json_parse_string("[\"\\u\"]") == nullptr);     /* invalid utf value */
  TEST(json_parse_string("[\"\\\"]") == nullptr);      /* control character */
  TEST(json_parse_string("[\"\"\"]") == nullptr);      /* control character */
  TEST(json_parse_string("[\"\0\"]") == nullptr);      /* control character */
  TEST(json_parse_string("[\"\a\"]") == nullptr);      /* control character */
  TEST(json_parse_string("[\"\b\"]") == nullptr);      /* control character */
  TEST(json_parse_string("[\"\t\"]") == nullptr);      /* control character */
  TEST(json_parse_string("[\"\n\"]") == nullptr);      /* control character */
  TEST(json_parse_string("[\"\f\"]") == nullptr);      /* control character */
  TEST(json_parse_string("[\"\r\"]") == nullptr);      /* control character */
  TEST(json_parse_string("[0x2]") == nullptr);         /* hex */
  TEST(json_parse_string("[0X2]") == nullptr);         /* HEX */
  TEST(json_parse_string("[07]") == nullptr);          /* octals */
  TEST(json_parse_string("[0070]") == nullptr);
  TEST(json_parse_string("[07.0]") == nullptr);
  TEST(json_parse_string("[-07]") == nullptr);
  TEST(json_parse_string("[-007]") == nullptr);
  TEST(json_parse_string("[-07.0]") == nullptr);
  TEST(json_parse_string("-") == nullptr);
  TEST(json_parse_string("-x") == nullptr);
  TEST(json_parse_string("[\"\\uDF67\\uD834\"]") ==
       nullptr); /* wrong order surrogate pair */
  TEST(json_parse_string("[1.7976931348623157e309]") == nullptr);
  TEST(json_parse_string("[-1.7976931348623157e309]") == nullptr);
  TEST(g_malloc_count == 0);
}

void test_suite_4() {
  const char *filename = "test_2.txt";
  JSON_Value *a = nullptr, *a_copy = nullptr;
  a = json_parse_file(get_file_path(filename));
  TEST(json_value_equals(a, a)); /* test equality test */
  a_copy = json_value_deep_copy(a);
  TEST(a_copy != nullptr);
  TEST(json_value_equals(a, a_copy));
}

void test_suite_5() {
  double zero = 0.0; /* msvc is silly (workaround for error C2124) */

  JSON_Value *val_from_file = json_parse_file(get_file_path("test_5.txt"));

  JSON_Value *val = nullptr, *val_with_parent;
  JSON_Object *obj = nullptr;
  JSON_Array *interests_arr = nullptr;

  JSON_Value *remove_test_val = nullptr;
  JSON_Array *remove_test_arr = nullptr;

  val = json_value_init_object();
  TEST(val != nullptr);

  obj = json_value_get_object(val);
  TEST(obj != nullptr);

  TEST(json_object_set_string(obj, "first", "John") == JSONSuccess);
  TEST(json_object_set_string(obj, "last", "Doe") == JSONSuccess);
  TEST(json_object_set_number(obj, "age", 25) == JSONSuccess);
  TEST(json_object_set_boolean(obj, "registered", true) == JSONSuccess);

  TEST(json_object_set_value(obj, "interests", json_value_init_array()) ==
       JSONSuccess);
  interests_arr = json_object_get_array(obj, "interests");
  TEST(interests_arr != nullptr);
  TEST(json_array_append_string(interests_arr, "Writing") == JSONSuccess);
  TEST(json_array_append_string(interests_arr, "Mountain Biking") ==
       JSONSuccess);
  TEST(json_array_replace_string(interests_arr, 0, "Reading") == JSONSuccess);

  TEST(json_object_dotset_string(obj, "favorites.color", "blue") ==
       JSONSuccess);
  TEST(json_object_dotset_string(obj, "favorites.sport", "running") ==
       JSONSuccess);
  TEST(json_object_dotset_string(obj, "favorites.fruit", "apple") ==
       JSONSuccess);
  TEST(json_object_dotremove(obj, "favorites.fruit") == JSONSuccess);
  TEST(json_object_set_string(obj, "utf string", "lorem ipsum") == JSONSuccess);
  TEST(json_object_set_string(obj, "utf-8 string", "あいうえお") ==
       JSONSuccess);
  TEST(json_object_set_string(obj, "surrogate string", "lorem𝄞ipsum𝍧lorem") ==
       JSONSuccess);
  TEST(json_object_set_string_with_len(obj, "string with null", "abc\0def",
                                       7) == JSONSuccess);
  TEST(json_object_set_string(obj, "windows path", "C:\\Windows\\Path") ==
       JSONSuccess);
  TEST(json_value_equals(val_from_file, val));

  TEST(json_object_set_string(obj, nullptr, "") == JSONFailure);
  TEST(json_object_set_string(obj, "last", nullptr) == JSONFailure);
  TEST(json_object_set_string(obj, nullptr, nullptr) == JSONFailure);
  TEST(json_object_set_value(obj, nullptr, nullptr) == JSONFailure);

  TEST(json_object_dotset_string(obj, nullptr, "") == JSONFailure);
  TEST(json_object_dotset_string(obj, "favorites.color", nullptr) ==
       JSONFailure);
  TEST(json_object_dotset_string(obj, nullptr, nullptr) == JSONFailure);
  TEST(json_object_dotset_value(obj, nullptr, nullptr) == JSONFailure);

  TEST(json_array_append_string(nullptr, "lorem") == JSONFailure);
  TEST(json_array_append_value(interests_arr, nullptr) == JSONFailure);
  TEST(json_array_append_value(nullptr, nullptr) == JSONFailure);

  TEST(json_array_remove(nullptr, 0) == JSONFailure);
  TEST(json_array_replace_value(interests_arr, 0, nullptr) == JSONFailure);
  TEST(json_array_replace_string(nullptr, 0, "lorem") == JSONFailure);
  TEST(json_array_replace_string(interests_arr, 100, "not existing") ==
       JSONFailure);

  TEST(json_array_append_string(json_object_get_array(obj, "interests"),
                                nullptr) == JSONFailure);

  TEST(json_array_append_string(interests_arr, "Writing") == JSONSuccess);
  TEST(json_array_remove(interests_arr, 0) == JSONSuccess);
  TEST(json_array_remove(interests_arr, 1) == JSONSuccess);
  TEST(json_array_remove(interests_arr, 0) == JSONSuccess);
  TEST(json_array_remove(interests_arr, 0) ==
       JSONFailure); /* should be empty by now */

  val_with_parent = json_value_init_null();
  TEST(json_object_set_value(obj, "x", val_with_parent) == JSONSuccess);
  TEST(json_object_set_value(obj, "x", val_with_parent) == JSONFailure);

  val_with_parent = json_value_init_null();
  TEST(json_array_append_value(interests_arr, val_with_parent) == JSONSuccess);
  TEST(json_array_append_value(interests_arr, val_with_parent) == JSONFailure);

  val_with_parent = json_value_init_null();
  TEST(json_array_replace_value(interests_arr, 0, val_with_parent) ==
       JSONSuccess);
  TEST(json_array_replace_value(interests_arr, 0, val_with_parent) ==
       JSONFailure);

  TEST(json_object_remove(obj, "interests") == JSONSuccess);

  /* UTF-8 tests */
  TEST(json_object_set_string(obj, "correct string", "κόσμε") == JSONSuccess);

  TEST(json_object_set_string(obj, "boundary 1", "\xed\x9f\xbf") ==
       JSONSuccess);
  TEST(json_object_set_string(obj, "boundary 2", "\xee\x80\x80") ==
       JSONSuccess);
  TEST(json_object_set_string(obj, "boundary 3", "\xef\xbf\xbd") ==
       JSONSuccess);
  TEST(json_object_set_string(obj, "boundary 4", "\xf4\x8f\xbf\xbf") ==
       JSONSuccess);

  TEST(json_object_set_string(obj, "first continuation byte", "\x80") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "last continuation byte", "\xbf") ==
       JSONFailure);

  TEST(json_object_set_string(obj, "impossible sequence 1", "\xfe") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "impossible sequence 2", "\xff") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "impossible sequence 3",
                              "\xfe\xfe\xff\xff") == JSONFailure);

  TEST(json_object_set_string(obj, "overlong 1", "\xc0\xaf") == JSONFailure);
  TEST(json_object_set_string(obj, "overlong 2", "\xc1\xbf") == JSONFailure);
  TEST(json_object_set_string(obj, "overlong 3", "\xe0\x80\xaf") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "overlong 4", "\xe0\x9f\xbf") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "overlong 5", "\xf0\x80\x80\xaf") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "overlong 6", "\xf0\x8f\xbf\xbf") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "overlong 7", "\xf0\x8f\xbf\xbf") ==
       JSONFailure);

  TEST(json_object_set_string(obj, "overlong null 1", "\xc0\x80") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "overlong null 2", "\xe0\x80\x80") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "overlong null 3", "\xf0\x80\x80\x80") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "overlong null 4", "\xf8\x80\x80\x80\x80") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "overlong null 5",
                              "\xfc\x80\x80\x80\x80\x80") == JSONFailure);

  TEST(json_object_set_string(obj, "single surrogate 1", "\xed\xa0\x80") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "single surrogate 2", "\xed\xaf\xbf") ==
       JSONFailure);
  TEST(json_object_set_string(obj, "single surrogate 3", "\xed\xbf\xbf") ==
       JSONFailure);

  /* Testing removing values from array, order of the elements should be
   * preserved */
  remove_test_val = json_parse_string("[1, 2, 3, 4, 5]");
  remove_test_arr = json_array(remove_test_val);
  json_array_remove(remove_test_arr, 2);
  TEST(json_value_equals(remove_test_val, json_parse_string("[1, 2, 4, 5]")));
  json_array_remove(remove_test_arr, 0);
  TEST(json_value_equals(remove_test_val, json_parse_string("[2, 4, 5]")));
  json_array_remove(remove_test_arr, 2);
  TEST(json_value_equals(remove_test_val, json_parse_string("[2, 4]")));

  /* Testing nan and inf */
  TEST(json_object_set_number(obj, "num", 0.0 / zero) == JSONFailure);
  TEST(json_object_set_number(obj, "num", 1.0 / zero) == JSONFailure);
}

void test_suite_6() {
  const char *filename = "test_2.txt";
  JSON_Value *a = nullptr;
  JSON_Value *b = nullptr;
  a = json_parse_file(get_file_path(filename));
  b = json_parse_file(get_file_path(filename));
  TEST(json_value_equals(a, b));
  json_object_set_string(json_object(a), "string", "eki");
  TEST(!json_value_equals(a, b));
  a = json_value_deep_copy(b);
  TEST(json_value_equals(a, b));
  json_array_append_number(
      json_object_get_array(json_object(b), "string array"), 1337);
  TEST(!json_value_equals(a, b));
}

void test_suite_7() {
  JSON_Value *val_from_file = json_parse_file(get_file_path("test_5.txt"));
  JSON_Value *schema = json_value_init_object();
  JSON_Object *schema_obj = json_value_get_object(schema);
  JSON_Array *interests_arr = nullptr;
  json_object_set_string(schema_obj, "first", "");
  json_object_set_string(schema_obj, "last", "");
  json_object_set_number(schema_obj, "age", 0);
  json_object_set_value(schema_obj, "interests", json_value_init_array());
  interests_arr = json_object_get_array(schema_obj, "interests");
  json_array_append_string(interests_arr, "");
  json_object_set_null(schema_obj, "favorites");
  TEST(json_validate(schema, val_from_file) == JSONSuccess);
  json_object_set_string(schema_obj, "age", "");
  TEST(json_validate(schema, val_from_file) == JSONFailure);
}

void test_suite_8() {
  constexpr char filename[] = "test_2.txt";
  constexpr char temp_filename[] = "test_2_serialized.txt";
  auto a = json_parse_file(get_file_path(filename));
  auto buf = json_serialize_to_string(a);
  const auto serialization_size = json_serialization_size(a);

  TEST(json_serialize_to_file(a, get_file_path(temp_filename)) == JSONSuccess);
  auto b = json_parse_file(get_file_path(temp_filename));
  TEST(json_value_equals(a, b));
  remove(temp_filename);
  TEST(buf != nullptr);
  if (buf != nullptr) {
    TEST((strlen(buf) + 1) == serialization_size);
  }

  json_free_serialized_string(buf);
  json_value_free(b);
  json_value_free(a);
}

void test_suite_9() {
  constexpr char filename[] = "test_2_pretty.txt";
  constexpr char temp_filename[] = "test_2_serialized_pretty.txt";
  auto a = json_parse_file(get_file_path(filename));

  TEST(json_serialize_to_file_pretty(a, get_file_path(temp_filename)) ==
       JSONSuccess);
  auto b = json_parse_file(get_file_path(temp_filename));
  TEST(json_value_equals(a, b));
  remove(temp_filename);
  const auto serialization_size = json_serialization_size_pretty(a);
  auto serialized = json_serialize_to_string_pretty(a);
  TEST(serialized != nullptr);
  if (serialized != nullptr) {
    TEST((strlen(serialized) + 1) == serialization_size);
  }

  auto file_contents = read_file(get_file_path(filename));
  TEST(file_contents != nullptr);
  if (serialized != nullptr && file_contents != nullptr) {
    TEST(STREQ(file_contents, serialized));
  }

  free(file_contents);
  json_free_serialized_string(serialized);
  json_value_free(b);
  json_value_free(a);
}

void test_suite_10() {
  JSON_Value *val;
  char *serialized;

  g_malloc_count = 0;

  val = json_parse_file(get_file_path("test_1_1.txt"));
  json_value_free(val);

  val = json_parse_file(get_file_path("test_1_3.txt"));
  json_value_free(val);

  val = json_parse_file(get_file_path("test_2.txt"));
  serialized = json_serialize_to_string_pretty(val);
  json_free_serialized_string(serialized);
  json_value_free(val);

  val = json_parse_file(get_file_path("test_2_pretty.txt"));
  json_value_free(val);

  TEST(g_malloc_count == 0);
}

void test_suite_11() {
  constexpr char array_with_slashes[] = "[\"a/b/c\"]";
  constexpr char array_with_escaped_slashes[] = "[\"a\\/b\\/c\"]";
  auto value = json_parse_string(array_with_slashes);
  TEST(value != nullptr);

  auto serialized = json_serialize_to_string(value);
  TEST(STREQ(array_with_escaped_slashes, serialized));
  json_free_serialized_string(serialized);

  json_set_escape_slashes(false);
  serialized = json_serialize_to_string(value);
  TEST(STREQ(array_with_slashes, serialized));
  json_free_serialized_string(serialized);

  json_set_escape_slashes(true);
  serialized = json_serialize_to_string(value);
  TEST(STREQ(array_with_escaped_slashes, serialized));
  json_free_serialized_string(serialized);
  json_value_free(value);
}

void test_memory_leaks() {
  g_malloc_count = 0;

  TEST(json_object_set_string(nullptr, "lorem", "ipsum") == JSONFailure);
  TEST(json_object_set_number(nullptr, "lorem", 42) == JSONFailure);
  TEST(json_object_set_boolean(nullptr, "lorem", false) == JSONFailure);
  TEST(json_object_set_null(nullptr, "lorem") == JSONFailure);

  TEST(json_parse_string("{\"\\u0000\"") == nullptr);

  TEST(g_malloc_count == 0);
}

void test_failing_allocations() {
  const char *filename = "test_2.txt";
  JSON_Value *root_value = nullptr;
  JSON_Object *root_object = nullptr;
  int i = 0;
  int n = 0;
  char key_val_buf[32];

  json_set_allocation_functions(failing_malloc, failing_free);

  printf("Testing failing allocations: ");

  while (1) {
    /*        printf("Failing at allocation %d\n", n); */
    g_failing_alloc.allocation_to_fail = n;
    g_failing_alloc.alloc_count = 0;
    g_failing_alloc.total_count = 0;
    g_failing_alloc.has_failed = false;
    g_failing_alloc.should_fail = true;
    n++;

    root_value = json_parse_file(get_file_path(filename));
    if (g_failing_alloc.has_failed) {
      if (root_value) {
        printf(
            "Allocation has failed but parsing succeeded after allocation %d\n",
            n - 1);
        g_tests_failed++;
        return;
      }
    }

    if (root_value) {
      root_object = json_object(root_value);
      for (i = 0; i < 64; i++) {
        snprintf(key_val_buf, sizeof key_val_buf, "%d", i);
        json_object_set_string(root_object, key_val_buf, key_val_buf);
      }

      for (i = 0; i < 64; i++) {
        snprintf(key_val_buf, sizeof key_val_buf, "%d", i);
        json_object_set_string(root_object, key_val_buf, key_val_buf);
      }

      json_object_dotset_number(root_object, "ala.ma.kota", 123);
      json_object_dotremove(root_object, "ala.ma.kota");
    }

    json_value_free(root_value);

    if (g_failing_alloc.alloc_count != 0) {
      printf("Leak after failing allocation %d\n", n - 1);
      g_tests_failed++;
      return;
    }

    if (!g_failing_alloc.has_failed) {
      break;
    }
  }

  json_set_allocation_functions(counted_malloc, counted_free);
  printf("OK (tested %d failing allocations)\n", n - 1);
  g_tests_passed++;
}

void test_custom_number_format() {
  g_malloc_count = 0;
  {
    auto val = json_value_init_number(0.6);
    json_set_float_serialization_format("%.1f");
    auto serialized = json_serialize_to_string(val);
    json_set_float_serialization_format(nullptr);
    TEST(STREQ(serialized, "0.6"));
    json_free_serialized_string(serialized);
    json_value_free(val);
  }
  TEST(g_malloc_count == 0);
}

static int custom_serialization_func_called = 0;
static int custom_serialization_func(double num, char *buf) {
  char num_buf[32];
  custom_serialization_func_called = 1;
  if (buf == nullptr) {
    buf = num_buf;
  }
  return snprintf(buf, sizeof num_buf, "%.1f", num);
}

void test_custom_number_serialization_function() {
  g_malloc_count = 0;
  {
    /* We just test that custom_serialization_func() gets called, not it's
     * performance */
    auto val = json_value_init_number(0.6);
    json_set_number_serialization_function(custom_serialization_func);
    auto serialized = json_serialize_to_string(val);
    TEST(STREQ(serialized, "0.6"));
    TEST(custom_serialization_func_called);
    json_set_number_serialization_function(nullptr);
    json_free_serialized_string(serialized);
    json_value_free(val);
  }
  TEST(g_malloc_count == 0);
}

void test_object_clear() {
  g_malloc_count = 0;
  {
    auto val = json_value_init_object();
    auto obj = json_value_get_object(val);
    json_object_set_string(obj, "foo", "bar");
    json_object_clear(obj);
    TEST(json_object_get_value(obj, "foo") == nullptr);
    json_value_free(val);
  }
  TEST(g_malloc_count == 0);
}

void test_allocation_functions_switch() {
  g_malloc_count = 0;
  json_set_float_serialization_format("%.1f");
  json_set_allocation_functions(malloc, free);
  json_set_allocation_functions(counted_malloc, counted_free);
  TEST(g_malloc_count == 0);
}

void print_commits_info([[maybe_unused]] const char *username,
                        [[maybe_unused]] const char *repo) {
  puts("print_commits_info() is intentionally left offline in this test "
       "harness.");
}

void persistence_example() {
  auto schema = json_parse_string("{\"name\":\"\"}");
  auto user_data = json_parse_file(get_file_path("user_data.json"));
  char buf[256];
  const char *name = nullptr;
  if (schema == nullptr) {
    json_value_free(user_data);
    return;
  }
  if (user_data == nullptr || json_validate(schema, user_data) != JSONSuccess) {
    puts("Enter your name:");
    if (fgets(buf, sizeof(buf), stdin) == nullptr) {
      json_value_free(schema);
      json_value_free(user_data);
      return;
    }
    buf[strcspn(buf, "\n")] = '\0';
    user_data = json_value_init_object();
    if (user_data == nullptr ||
        json_object_set_string(json_object(user_data), "name", buf) !=
            JSONSuccess ||
        json_serialize_to_file(user_data, "user_data.json") != JSONSuccess) {
      json_value_free(schema);
      json_value_free(user_data);
      return;
    }
  }
  name = json_object_get_string(json_object(user_data), "name");
  if (name != nullptr) {
    printf("Hello, %s.", name);
  }
  json_value_free(schema);
  json_value_free(user_data);
}

void serialization_example() {
  auto root_value = json_value_init_object();
  if (root_value == nullptr) {
    return;
  }

  auto root_object = json_value_get_object(root_value);
  auto emails = json_parse_string("[\"email@example.com\", "
                                  "\"email2@example.com\"]");
  if (root_object == nullptr || emails == nullptr ||
      json_object_set_string(root_object, "name", "John Smith") != JSONSuccess ||
      json_object_set_number(root_object, "age", 25) != JSONSuccess ||
      json_object_dotset_string(root_object, "address.city", "Cupertino") !=
          JSONSuccess ||
      json_object_dotset_value(root_object, "contact.emails", emails) !=
          JSONSuccess) {
    json_value_free(emails);
    json_value_free(root_value);
    return;
  }

  auto serialized_string = json_serialize_to_string_pretty(root_value);
  if (serialized_string == nullptr) {
    json_value_free(root_value);
    return;
  }

  puts(serialized_string);
  json_free_serialized_string(serialized_string);
  json_value_free(root_value);
}

static char *read_file(const char *file_path) {
  auto fp = fopen(file_path, "r");
  size_t size_to_read = 0;
  size_t size_read = 0;
  long pos = 0;
  char *file_contents = nullptr;
  if (fp == nullptr) {
    assert(0);
    return nullptr;
  }
  if (fseek(fp, 0L, SEEK_END) != 0) {
    fclose(fp);
    assert(0);
    return nullptr;
  }
  pos = ftell(fp);
  if (pos < 0) {
    fclose(fp);
    assert(0);
    return nullptr;
  }
  size_to_read = (size_t)pos;
  rewind(fp);
  file_contents = (char *)calloc(size_to_read + 1, sizeof(char));
  if (file_contents == nullptr) {
    fclose(fp);
    assert(0);
    return nullptr;
  }
  size_read = fread(file_contents, 1, size_to_read, fp);
  if (size_read == 0 || ferror(fp)) {
    fclose(fp);
    free(file_contents);
    assert(0);
    return nullptr;
  }
  fclose(fp);
  file_contents[size_read] = '\0';
  return file_contents;
}

const char *get_file_path(const char *filename) {
  constexpr size_t path_buf_length = 2'048;
  static char path_buf[path_buf_length] = {0};
  memset(path_buf, 0, sizeof(path_buf));
  snprintf(path_buf, sizeof path_buf, "%s/%s", g_tests_path, filename);
  return path_buf;
}

static void *counted_malloc(size_t size) {
  auto res = calloc(1, size);
  if (res != nullptr) {
    g_malloc_count++;
  }
  return res;
}

static void counted_free(void *ptr) {
  if (ptr != nullptr) {
    g_malloc_count--;
  }
  free(ptr);
}

static void *failing_malloc(size_t size) {
  if (g_failing_alloc.should_fail &&
      g_failing_alloc.total_count >= g_failing_alloc.allocation_to_fail) {
    g_failing_alloc.has_failed = true;
    return nullptr;
  }
  auto res = calloc(1, size);
  if (res != nullptr) {
    g_failing_alloc.total_count++;
    g_failing_alloc.alloc_count++;
  }
  return res;
}

static void failing_free(void *ptr) {
  if (ptr != nullptr) {
    g_failing_alloc.alloc_count--;
  }
  free(ptr);
}
