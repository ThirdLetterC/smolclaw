/*
 SPDX-License-Identifier: MIT
*/

#include "parson/parson.h"

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

static int g_tests_passed;
static int g_tests_failed;
static constexpr size_t security_item_count = 4'096;

[[nodiscard]] static JSON_Value *build_large_security_fixture();
[[nodiscard]] static char *serialize_fixture(const JSON_Value *value);
[[nodiscard]] static bool set_large_object_members(JSON_Object *object,
                                                   size_t item_count);
[[nodiscard]] static bool append_large_array_members(JSON_Array *array,
                                                     size_t item_count);

static void test_trailing_data_behavior();
static void test_large_fixture_round_trip();
static void test_invalid_unicode_edges();
static void test_bounded_serialization_contract();
static void test_comment_parser_regressions();
static void test_custom_number_serialization_failures();

static int failing_number_serialization([[maybe_unused]] double number,
                                        [[maybe_unused]] char *buf) {
  return -1;
}

#ifdef TESTS_MAIN
int main() {
#else
int tests_main();
int tests_main() {
#endif
#if 0
}
#endif
  puts("#######################################################################"
       "#########");
  puts("Running parson security tests");

  test_trailing_data_behavior();
  test_large_fixture_round_trip();
  test_invalid_unicode_edges();
  test_bounded_serialization_contract();
  test_comment_parser_regressions();
  test_custom_number_serialization_failures();

  printf("Tests failed: %d\n", g_tests_failed);
  printf("Tests passed: %d\n", g_tests_passed);
  puts("#######################################################################"
       "#########");
  return g_tests_failed == 0 ? 0 : 1;
}

static void test_trailing_data_behavior() {
  auto value = json_parse_string("{\"ok\":true} trailing");
  TEST(value != nullptr);
  TEST(json_value_get_type(value) == JSONObject);
  TEST(json_object_get_boolean(json_value_get_object(value), "ok") ==
       JSONBooleanTrue);
  json_value_free(value);

  value = json_parse_string_with_comments("{\"ok\":true}// ignored by caller\n"
                                          "{\"second\":false}");
  TEST(value != nullptr);
  TEST(json_value_get_type(value) == JSONObject);
  TEST(json_object_get_boolean(json_value_get_object(value), "ok") ==
       JSONBooleanTrue);
  TEST(json_object_get_value(json_value_get_object(value), "second") ==
       nullptr);
  json_value_free(value);
}

static void test_large_fixture_round_trip() {
  auto fixture = build_large_security_fixture();
  auto serialized = serialize_fixture(fixture);
  auto parsed = json_parse_string(serialized);

  TEST(fixture != nullptr);
  TEST(serialized != nullptr);
  TEST(parsed != nullptr);

  if (fixture != nullptr) {
    auto root_object = json_value_get_object(fixture);
    auto map = json_object_get_object(root_object, "map");
    auto items = json_object_get_array(root_object, "items");

    TEST(json_object_get_count(map) == security_item_count);
    TEST(json_array_get_count(items) == security_item_count);
    TEST(json_value_equals(fixture, parsed));
  }

  json_value_free(parsed);
  json_free_serialized_string(serialized);
  json_value_free(fixture);
}

static void test_invalid_unicode_edges() {
  TEST(json_parse_string("[\"\\uD800\"]") == nullptr);
  TEST(json_parse_string("[\"\\uDC00\"]") == nullptr);
  TEST(json_parse_string("[\"\\uD834x\"]") == nullptr);
  TEST(json_parse_string("[\"\\uD834\\u0000\"]") == nullptr);

  TEST(json_value_init_string_with_len("\xF0\x9F\x92", 3) == nullptr);
  TEST(json_value_init_string_with_len("\xF4\x90\x80\x80", 4) == nullptr);
  TEST(json_value_init_string_with_len("\xED\xA0\x80", 3) == nullptr);
  TEST(json_value_init_string_with_len("\x80", 1) == nullptr);
}

static void test_bounded_serialization_contract() {
  auto value = json_parse_string("{\"k\":\"value\",\"n\":42}");
  TEST(value != nullptr);

  if (value != nullptr) {
    const size_t required_size = json_serialization_size(value);
    auto small_buffer =
        (char *)calloc(required_size == 0 ? 1 : required_size, sizeof(char));
    TEST(small_buffer != nullptr);
    if (small_buffer != nullptr) {
      memset(small_buffer, 'X', required_size == 0 ? 1 : required_size);
      TEST(json_serialize_to_buffer(
               value, small_buffer,
               required_size == 0 ? 0 : required_size - 1) == JSONFailure);
      TEST(small_buffer[0] == 'X');
      free(small_buffer);
    }
  }

  json_value_free(value);
}

static void test_comment_parser_regressions() {
  TEST(json_parse_string_with_comments("/* unterminated") == nullptr);
  TEST(json_parse_string_with_comments(
           "{\"url\":\"https://example.test/a//b\"}") != nullptr);
  TEST(json_parse_string_with_comments("{\"block\":\"/* literal */\"}") !=
       nullptr);
}

static void test_custom_number_serialization_failures() {
  auto value = json_value_init_number(1.25);
  TEST(value != nullptr);

  json_set_number_serialization_function(failing_number_serialization);
  TEST(json_serialization_size(value) == 0);
  TEST(json_serialize_to_string(value) == nullptr);
  TEST(json_serialize_to_string_pretty(value) == nullptr);
  json_set_number_serialization_function(nullptr);

  json_value_free(value);
}

[[nodiscard]] static JSON_Value *build_large_security_fixture() {
  auto root_value = json_value_init_object();
  if (root_value == nullptr) {
    return nullptr;
  }

  auto root_object = json_value_get_object(root_value);
  if (json_object_set_value(root_object, "map", json_value_init_object()) !=
          JSONSuccess ||
      json_object_set_value(root_object, "items", json_value_init_array()) !=
          JSONSuccess) {
    json_value_free(root_value);
    return nullptr;
  }

  auto map = json_object_get_object(root_object, "map");
  auto items = json_object_get_array(root_object, "items");

  if (!set_large_object_members(map, security_item_count) ||
      !append_large_array_members(items, security_item_count)) {
    json_value_free(root_value);
    return nullptr;
  }

  return root_value;
}

[[nodiscard]] static bool set_large_object_members(JSON_Object *object,
                                                   size_t item_count) {
  char key[32];
  char value[32];

  if (object == nullptr) {
    return false;
  }

  for (size_t i = 0; i < item_count; i++) {
    const int key_length = snprintf(key, sizeof(key), "key_%04zu", i);
    const int value_length = snprintf(value, sizeof(value), "value_%04zu", i);
    if (key_length < 0 || (size_t)key_length >= sizeof(key) ||
        value_length < 0 || (size_t)value_length >= sizeof(value)) {
      return false;
    }
    if (json_object_set_string(object, key, value) != JSONSuccess) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] static bool append_large_array_members(JSON_Array *array,
                                                     size_t item_count) {
  if (array == nullptr) {
    return false;
  }

  for (size_t i = 0; i < item_count; i++) {
    if (json_array_append_number(array, (double)i) != JSONSuccess) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] static char *serialize_fixture(const JSON_Value *value) {
  if (value == nullptr) {
    return nullptr;
  }
  return json_serialize_to_string(value);
}
