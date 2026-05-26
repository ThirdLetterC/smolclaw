#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsonrpc/arena.h"
#include "jsonrpc/jsonrpc.h"

constexpr int32_t JSONRPC_ERR_PARSE = -32'700;
constexpr int32_t JSONRPC_ERR_INVALID_REQUEST = -32'600;
constexpr int32_t JSONRPC_ERR_METHOD_NOT_FOUND = -32'601;
constexpr int32_t JSONRPC_ERR_INVALID_PARAMS = -32'602;
constexpr int32_t JSONRPC_ERR_INTERNAL = -32'603;

constexpr size_t TEST_MAX_MESSAGES = 32U;
constexpr size_t JSONRPC_MAX_MESSAGE_BYTES = 65'536U;
constexpr size_t JSONRPC_MAX_BUFFER_BYTES = 131'072U;

typedef struct {
  char *messages[TEST_MAX_MESSAGES];
  size_t message_count;
  bool fail_send;
  size_t close_calls;
} test_transport_state_t;

typedef struct {
  size_t open_count;
  size_t close_count;
  size_t request_count;
  size_t notification_count;
  const char *last_method;
} test_callback_state_t;

typedef struct {
  test_transport_state_t transport_state;
  test_callback_state_t callback_state;
} test_context_t;

static test_context_t *g_active_test_context = nullptr;

#define ASSERT_TRUE(expr)                                                        \
  do {                                                                           \
    if (!(expr)) {                                                               \
      fprintf(stderr, "assertion failed: %s (%s:%d)\n", #expr, __FILE__,        \
              __LINE__);                                                         \
      return false;                                                              \
    }                                                                            \
  } while (false)

static void test_transport_state_reset(test_transport_state_t *state) {
  if (state == nullptr) {
    return;
  }
  for (size_t i = 0U; i < state->message_count; ++i) {
    free(state->messages[i]);
    state->messages[i] = nullptr;
  }
  state->message_count = 0U;
  state->fail_send = false;
  state->close_calls = 0U;
}

static bool test_send_raw(jsonrpc_transport_t *self, const uint8_t *data,
                          size_t len) {
  if (self == nullptr || self->user_data == nullptr || data == nullptr ||
      len == 0U) {
    return false;
  }

  auto state = (test_transport_state_t *)self->user_data;
  if (state->fail_send) {
    return false;
  }

  if (state->message_count >= TEST_MAX_MESSAGES) {
    return false;
  }

  auto copy = (char *)calloc(len + 1U, sizeof(char));
  if (copy == nullptr) {
    return false;
  }

  memcpy(copy, data, len);
  copy[len] = '\0';
  state->messages[state->message_count] = copy;
  state->message_count += 1U;
  return true;
}

static void test_close(jsonrpc_transport_t *self) {
  if (self == nullptr || self->user_data == nullptr) {
    return;
  }
  auto state = (test_transport_state_t *)self->user_data;
  state->close_calls += 1U;
}

static void on_open(jsonrpc_conn_t *conn) {
  auto context = (test_context_t *)jsonrpc_conn_get_context(conn);
  if (context == nullptr || context != g_active_test_context) {
    return;
  }
  context->callback_state.open_count += 1U;
}

static void on_open_close_immediately(jsonrpc_conn_t *conn) {
  jsonrpc_conn_free(conn);
}

static void on_close(jsonrpc_conn_t *conn) {
  (void)conn;
  if (g_active_test_context == nullptr) {
    return;
  }
  g_active_test_context->callback_state.close_count += 1U;
}

static bool on_request(jsonrpc_conn_t *conn, const char *method,
                       const JSON_Value *params [[maybe_unused]],
                       jsonrpc_response_t *response) {
  auto context = (test_context_t *)jsonrpc_conn_get_context(conn);
  if (context == nullptr || context != g_active_test_context || method == nullptr ||
      response == nullptr) {
    return false;
  }

  context->callback_state.request_count += 1U;
  context->callback_state.last_method = method;

  if (strcmp(method, "ping") == 0) {
    response->result = json_value_init_string("pong");
    return response->result != nullptr;
  }
  if (strcmp(method, "raise") == 0) {
    response->error_code = JSONRPC_ERR_INTERNAL;
    response->error_message = "boom";
    return true;
  }
  if (strcmp(method, "raise_default") == 0) {
    response->error_code = JSONRPC_ERR_INTERNAL;
    response->error_message = nullptr;
    return true;
  }
  if (strcmp(method, "no_result") == 0) {
    return true;
  }
  if (strcmp(method, "close_with_result") == 0) {
    response->result = json_value_init_string("ignored");
    jsonrpc_conn_free(conn);
    return true;
  }

  return false;
}

static void on_notification(jsonrpc_conn_t *conn, const char *method,
                            const JSON_Value *params [[maybe_unused]]) {
  auto context = (test_context_t *)jsonrpc_conn_get_context(conn);
  if (context == nullptr || context != g_active_test_context) {
    return;
  }
  context->callback_state.notification_count += 1U;
  context->callback_state.last_method = method;
}

[[nodiscard]] static jsonrpc_conn_t *test_conn_new(test_context_t *context) {
  if (context == nullptr) {
    return nullptr;
  }

  jsonrpc_transport_t transport = {.user_data = &context->transport_state,
                                   .send_raw = test_send_raw,
                                   .close = test_close};

  jsonrpc_callbacks_t callbacks = {.on_open = on_open,
                                   .on_close = on_close,
                                   .on_request = on_request,
                                   .on_notification = on_notification};

  return jsonrpc_conn_new(transport, callbacks, context);
}

[[nodiscard]] static jsonrpc_conn_t *
test_conn_new_with_callbacks(test_context_t *context, jsonrpc_callbacks_t callbacks) {
  if (context == nullptr) {
    return nullptr;
  }
  jsonrpc_transport_t transport = {.user_data = &context->transport_state,
                                   .send_raw = test_send_raw,
                                   .close = test_close};
  return jsonrpc_conn_new(transport, callbacks, context);
}

[[nodiscard]] static JSON_Value *test_parse_sent_json(
    const test_transport_state_t *state, size_t index) {
  if (state == nullptr || index >= state->message_count) {
    return nullptr;
  }

  const char *message = state->messages[index];
  if (message == nullptr) {
    return nullptr;
  }

  const size_t message_len = strlen(message);
  if (message_len == 0U || message[message_len - 1U] != '\n') {
    return nullptr;
  }

  auto copy = (char *)calloc(message_len, sizeof(char));
  if (copy == nullptr) {
    return nullptr;
  }

  memcpy(copy, message, message_len - 1U);
  copy[message_len - 1U] = '\0';
  auto value = json_parse_string(copy);
  free(copy);
  return value;
}

static bool test_basic_request_response() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);
  ASSERT_TRUE(context.callback_state.open_count == 1U);

  const char *request = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.callback_state.request_count == 1U);
  ASSERT_TRUE(context.transport_state.message_count == 1U);

  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  ASSERT_TRUE(json_value_get_type(response) == JSONObject);

  auto response_obj = json_value_get_object(response);
  ASSERT_TRUE(strcmp(json_object_get_string(response_obj, "jsonrpc"), "2.0") ==
              0);
  ASSERT_TRUE(json_object_get_number(response_obj, "id") == 1.0);
  ASSERT_TRUE(strcmp(json_object_get_string(response_obj, "result"), "pong") ==
              0);

  json_value_free(response);
  jsonrpc_conn_free(conn);
  ASSERT_TRUE(context.callback_state.close_count == 1U);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_notification_has_no_response() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *request = "{\"jsonrpc\":\"2.0\",\"method\":\"note\"}\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.callback_state.notification_count == 1U);
  ASSERT_TRUE(strcmp(context.callback_state.last_method, "note") == 0);
  ASSERT_TRUE(context.transport_state.message_count == 0U);

  jsonrpc_conn_free(conn);
  ASSERT_TRUE(context.callback_state.close_count == 1U);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_parse_error_response() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *request = "{not-valid-json}\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.transport_state.message_count == 1U);
  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  auto response_obj = json_value_get_object(response);
  auto error_obj = json_object_get_object(response_obj, "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_PARSE);
  ASSERT_TRUE(strcmp(json_object_get_string(error_obj, "message"),
                     "Parse error") == 0);
  ASSERT_TRUE(json_object_get_value(response_obj, "id") != nullptr);
  ASSERT_TRUE(json_value_get_type(json_object_get_value(response_obj, "id")) ==
              JSONNull);

  json_value_free(response);
  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_method_not_found_response() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *request = "{\"jsonrpc\":\"2.0\",\"id\":\"abc\",\"method\":"
                        "\"unknown\"}\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.callback_state.request_count == 1U);
  ASSERT_TRUE(context.transport_state.message_count == 1U);

  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  auto response_obj = json_value_get_object(response);
  auto error_obj = json_object_get_object(response_obj, "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_METHOD_NOT_FOUND);
  ASSERT_TRUE(strcmp(json_object_get_string(error_obj, "message"),
                     "Method not found") == 0);
  ASSERT_TRUE(strcmp(json_object_get_string(response_obj, "id"), "abc") == 0);

  json_value_free(response);
  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_invalid_params_response() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *request =
      "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\",\"params\":123}\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.transport_state.message_count == 1U);
  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  auto response_obj = json_value_get_object(response);
  auto error_obj = json_object_get_object(response_obj, "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_INVALID_PARAMS);
  ASSERT_TRUE(strcmp(json_object_get_string(error_obj, "message"),
                     "Invalid params") == 0);

  json_value_free(response);
  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_batch_mixed_notification_and_request() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *request =
      "[{\"jsonrpc\":\"2.0\",\"method\":\"note\"},"
      "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}]\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.callback_state.notification_count == 1U);
  ASSERT_TRUE(context.callback_state.request_count == 1U);
  ASSERT_TRUE(context.transport_state.message_count == 1U);

  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  ASSERT_TRUE(json_value_get_type(response) == JSONArray);

  auto response_array = json_value_get_array(response);
  ASSERT_TRUE(json_array_get_count(response_array) == 1U);
  auto response_item = json_array_get_object(response_array, 0U);
  ASSERT_TRUE(response_item != nullptr);
  ASSERT_TRUE(json_object_get_number(response_item, "id") == 9.0);
  ASSERT_TRUE(strcmp(json_object_get_string(response_item, "result"), "pong") ==
              0);

  json_value_free(response);
  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_send_result_and_send_error_api() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  auto id = json_value_init_string("r1");
  ASSERT_TRUE(id != nullptr);
  auto result = json_value_init_number(42.0);
  ASSERT_TRUE(result != nullptr);
  ASSERT_TRUE(jsonrpc_conn_send_result(conn, id, result));
  json_value_free(id);

  ASSERT_TRUE(context.transport_state.message_count == 1U);
  auto result_response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(result_response != nullptr);
  auto result_obj = json_value_get_object(result_response);
  ASSERT_TRUE(strcmp(json_object_get_string(result_obj, "id"), "r1") == 0);
  ASSERT_TRUE(json_object_get_number(result_obj, "result") == 42.0);
  json_value_free(result_response);

  ASSERT_TRUE(
      jsonrpc_conn_send_error(conn, nullptr, JSONRPC_ERR_INVALID_REQUEST, nullptr));
  ASSERT_TRUE(context.transport_state.message_count == 2U);
  auto error_response = test_parse_sent_json(&context.transport_state, 1U);
  ASSERT_TRUE(error_response != nullptr);
  auto error_obj = json_object_get_object(json_value_get_object(error_response),
                                          "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_INVALID_REQUEST);
  ASSERT_TRUE(strcmp(json_object_get_string(error_obj, "message"),
                     "Invalid Request") == 0);
  json_value_free(error_response);

  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_send_failure_triggers_transport_close() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);
  context.transport_state.fail_send = true;

  auto result = json_value_init_string("value");
  ASSERT_TRUE(result != nullptr);
  ASSERT_TRUE(!jsonrpc_conn_send_result(conn, nullptr, result));
  ASSERT_TRUE(context.transport_state.close_calls >= 1U);
  ASSERT_TRUE(context.transport_state.message_count == 0U);

  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_invalid_request_shapes() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *wrong_version =
      "{\"jsonrpc\":\"1.0\",\"id\":1,\"method\":\"ping\"}\n";
  const char *missing_method = "{\"jsonrpc\":\"2.0\",\"id\":2}\n";
  const char *bad_id_type =
      "{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"ping\"}\n";
  const char *top_level_scalar = "42\n";

  jsonrpc_conn_feed(conn, (const uint8_t *)wrong_version, strlen(wrong_version));
  jsonrpc_conn_feed(conn, (const uint8_t *)missing_method, strlen(missing_method));
  jsonrpc_conn_feed(conn, (const uint8_t *)bad_id_type, strlen(bad_id_type));
  jsonrpc_conn_feed(conn, (const uint8_t *)top_level_scalar, strlen(top_level_scalar));

  ASSERT_TRUE(context.transport_state.message_count == 4U);
  ASSERT_TRUE(context.callback_state.request_count == 0U);

  auto first = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(first != nullptr);
  auto first_error = json_object_get_object(json_value_get_object(first), "error");
  ASSERT_TRUE((int32_t)json_object_get_number(first_error, "code") ==
              JSONRPC_ERR_INVALID_REQUEST);
  ASSERT_TRUE(json_object_get_number(json_value_get_object(first), "id") == 1.0);
  json_value_free(first);

  auto second = test_parse_sent_json(&context.transport_state, 1U);
  ASSERT_TRUE(second != nullptr);
  auto second_error =
      json_object_get_object(json_value_get_object(second), "error");
  ASSERT_TRUE((int32_t)json_object_get_number(second_error, "code") ==
              JSONRPC_ERR_INVALID_REQUEST);
  ASSERT_TRUE(json_object_get_number(json_value_get_object(second), "id") == 2.0);
  json_value_free(second);

  auto third = test_parse_sent_json(&context.transport_state, 2U);
  ASSERT_TRUE(third != nullptr);
  auto third_obj = json_value_get_object(third);
  auto third_id = json_object_get_value(third_obj, "id");
  ASSERT_TRUE(third_id != nullptr);
  ASSERT_TRUE(json_value_get_type(third_id) == JSONNull);
  json_value_free(third);

  auto fourth = test_parse_sent_json(&context.transport_state, 3U);
  ASSERT_TRUE(fourth != nullptr);
  auto fourth_error =
      json_object_get_object(json_value_get_object(fourth), "error");
  ASSERT_TRUE((int32_t)json_object_get_number(fourth_error, "code") ==
              JSONRPC_ERR_INVALID_REQUEST);
  json_value_free(fourth);

  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_batch_edge_cases() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *empty_batch = "[]\n";
  const char *notifications_only =
      "[{\"jsonrpc\":\"2.0\",\"method\":\"n1\"},"
      "{\"jsonrpc\":\"2.0\",\"method\":\"n2\"}]\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)empty_batch, strlen(empty_batch));
  jsonrpc_conn_feed(conn, (const uint8_t *)notifications_only,
                    strlen(notifications_only));

  ASSERT_TRUE(context.transport_state.message_count == 1U);
  ASSERT_TRUE(context.callback_state.notification_count == 2U);

  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  auto error_obj = json_object_get_object(json_value_get_object(response), "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_INVALID_REQUEST);
  json_value_free(response);

  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_framing_and_embedded_nul_parse_error() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *blank_line = "\r\n";
  const char *ping_crlf = "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"ping\"}\r\n";
  const uint8_t embedded_nul[] = {'{', '"', 'j', 's', 'o', 'n', 'r', 'p',
                                  'c', '"', ':', '"', '2', '.', '0', '"',
                                  ',', '"', 'i', 'd', '"', ':', '1', '2',
                                  ',', '"', 'm', 'e', 't', 'h', 'o', 'd',
                                  '"', ':', '"', 'p', 'i', 'n', 'g', '"',
                                  '}', '\0', '\n'};

  jsonrpc_conn_feed(conn, (const uint8_t *)blank_line, strlen(blank_line));
  jsonrpc_conn_feed(conn, (const uint8_t *)ping_crlf, strlen(ping_crlf));
  jsonrpc_conn_feed(conn, embedded_nul, sizeof(embedded_nul));

  ASSERT_TRUE(context.transport_state.message_count == 2U);

  auto ok = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(ok != nullptr);
  ASSERT_TRUE(json_object_get_number(json_value_get_object(ok), "id") == 11.0);
  json_value_free(ok);

  auto parse_err = test_parse_sent_json(&context.transport_state, 1U);
  ASSERT_TRUE(parse_err != nullptr);
  auto error_obj = json_object_get_object(json_value_get_object(parse_err), "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_PARSE);
  json_value_free(parse_err);

  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_request_too_large_closes_connection() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const size_t line_len = JSONRPC_MAX_MESSAGE_BYTES + 1U;
  auto payload = (uint8_t *)calloc(line_len + 1U, sizeof(uint8_t));
  ASSERT_TRUE(payload != nullptr);
  memset(payload, 'a', line_len);
  payload[line_len] = '\n';

  jsonrpc_conn_feed(conn, payload, line_len + 1U);
  free(payload);

  ASSERT_TRUE(context.transport_state.close_calls >= 1U);
  ASSERT_TRUE(context.transport_state.message_count == 1U);

  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  auto error_obj = json_object_get_object(json_value_get_object(response), "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_INVALID_REQUEST);
  ASSERT_TRUE(strcmp(json_object_get_string(error_obj, "message"),
                     "Request too large") == 0);
  json_value_free(response);

  ASSERT_TRUE(jsonrpc_conn_get_context(conn) == &context);
  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_inbound_buffer_overflow_closes_connection() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const size_t too_big = JSONRPC_MAX_BUFFER_BYTES + 1U;
  auto payload = (uint8_t *)calloc(too_big, sizeof(uint8_t));
  ASSERT_TRUE(payload != nullptr);
  memset(payload, 'x', too_big);

  jsonrpc_conn_feed(conn, payload, too_big);
  free(payload);

  ASSERT_TRUE(context.transport_state.close_calls >= 1U);
  ASSERT_TRUE(context.transport_state.message_count == 1U);

  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  auto error_obj = json_object_get_object(json_value_get_object(response), "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_INVALID_REQUEST);
  ASSERT_TRUE(strcmp(json_object_get_string(error_obj, "message"),
                     "Request too large") == 0);
  json_value_free(response);

  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_request_handler_error_paths() {
  test_context_t context = {0};
  g_active_test_context = &context;
  auto conn = test_conn_new(&context);
  ASSERT_TRUE(conn != nullptr);

  const char *raise_custom = "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"raise\"}\n";
  const char *raise_default =
      "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"raise_default\"}\n";
  const char *no_result = "{\"jsonrpc\":\"2.0\",\"id\":23,\"method\":\"no_result\"}\n";
  const char *close_with_result =
      "{\"jsonrpc\":\"2.0\",\"id\":24,\"method\":\"close_with_result\"}\n";

  jsonrpc_conn_feed(conn, (const uint8_t *)raise_custom, strlen(raise_custom));
  jsonrpc_conn_feed(conn, (const uint8_t *)raise_default, strlen(raise_default));
  jsonrpc_conn_feed(conn, (const uint8_t *)no_result, strlen(no_result));
  jsonrpc_conn_feed(conn, (const uint8_t *)close_with_result,
                    strlen(close_with_result));

  ASSERT_TRUE(context.transport_state.message_count == 3U);
  ASSERT_TRUE(context.callback_state.close_count == 1U);

  auto first = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(first != nullptr);
  auto first_error = json_object_get_object(json_value_get_object(first), "error");
  ASSERT_TRUE((int32_t)json_object_get_number(first_error, "code") ==
              JSONRPC_ERR_INTERNAL);
  ASSERT_TRUE(strcmp(json_object_get_string(first_error, "message"), "boom") == 0);
  json_value_free(first);

  auto second = test_parse_sent_json(&context.transport_state, 1U);
  ASSERT_TRUE(second != nullptr);
  auto second_error =
      json_object_get_object(json_value_get_object(second), "error");
  ASSERT_TRUE((int32_t)json_object_get_number(second_error, "code") ==
              JSONRPC_ERR_INTERNAL);
  ASSERT_TRUE(strcmp(json_object_get_string(second_error, "message"),
                     "Internal error") == 0);
  json_value_free(second);

  auto third = test_parse_sent_json(&context.transport_state, 2U);
  ASSERT_TRUE(third != nullptr);
  auto third_error = json_object_get_object(json_value_get_object(third), "error");
  ASSERT_TRUE((int32_t)json_object_get_number(third_error, "code") ==
              JSONRPC_ERR_INTERNAL);
  ASSERT_TRUE(strcmp(json_object_get_string(third_error, "message"),
                     "Handler returned no result") == 0);
  json_value_free(third);

  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_no_request_callback_returns_method_not_found() {
  test_context_t context = {0};
  g_active_test_context = &context;
  jsonrpc_callbacks_t callbacks = {.on_open = on_open,
                                   .on_close = on_close,
                                   .on_request = nullptr,
                                   .on_notification = on_notification};
  auto conn = test_conn_new_with_callbacks(&context, callbacks);
  ASSERT_TRUE(conn != nullptr);

  const char *request = "{\"jsonrpc\":\"2.0\",\"id\":25,\"method\":\"ping\"}\n";
  jsonrpc_conn_feed(conn, (const uint8_t *)request, strlen(request));

  ASSERT_TRUE(context.callback_state.request_count == 0U);
  ASSERT_TRUE(context.transport_state.message_count == 1U);
  auto response = test_parse_sent_json(&context.transport_state, 0U);
  ASSERT_TRUE(response != nullptr);
  auto error_obj = json_object_get_object(json_value_get_object(response), "error");
  ASSERT_TRUE((int32_t)json_object_get_number(error_obj, "code") ==
              JSONRPC_ERR_METHOD_NOT_FOUND);
  json_value_free(response);

  jsonrpc_conn_free(conn);
  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_connection_closed_during_on_open_returns_null() {
  test_context_t context = {0};
  g_active_test_context = &context;
  jsonrpc_callbacks_t callbacks = {.on_open = on_open_close_immediately,
                                   .on_close = on_close,
                                   .on_request = on_request,
                                   .on_notification = on_notification};
  auto conn = test_conn_new_with_callbacks(&context, callbacks);
  ASSERT_TRUE(conn == nullptr);
  ASSERT_TRUE(context.callback_state.close_count == 1U);
  ASSERT_TRUE(context.callback_state.open_count == 0U);

  test_transport_state_reset(&context.transport_state);
  g_active_test_context = nullptr;
  return true;
}

static bool test_arena_api_paths() {
  Arena stack_arena = {0};
  char region[32] = {0};

  arena_init(nullptr, region, sizeof(region));
  arena_init(&stack_arena, region, 0U);
  ASSERT_TRUE(stack_arena.region == nullptr);
  ASSERT_TRUE(stack_arena.size == 0U);

  arena_init(&stack_arena, region, sizeof(region));
  ASSERT_TRUE(arena_alloc(nullptr, 8U) == nullptr);
  ASSERT_TRUE(arena_alloc(&stack_arena, 0U) == nullptr);
  ASSERT_TRUE(arena_alloc_aligned(&stack_arena, 8U, 8U) != nullptr);
  ASSERT_TRUE(arena_alloc_aligned(&stack_arena, 8U, 16U) != nullptr);
  ASSERT_TRUE(arena_alloc_aligned(&stack_arena, 32U, 8U) == nullptr);
  arena_clear(&stack_arena);
  ASSERT_TRUE(stack_arena.index == 0U);
  arena_clear(nullptr);

  auto src = arena_create(32U);
  auto dest = arena_create(16U);
  ASSERT_TRUE(src != nullptr);
  ASSERT_TRUE(dest != nullptr);

  auto src_bytes = (uint8_t *)arena_alloc(src, 12U);
  ASSERT_TRUE(src_bytes != nullptr);
  for (size_t i = 0U; i < 12U; ++i) {
    src_bytes[i] = (uint8_t)(i + 1U);
  }

  const size_t copied = arena_copy(dest, src);
  ASSERT_TRUE(copied == 12U);
  ASSERT_TRUE(dest->index == 12U);
  ASSERT_TRUE(memcmp(dest->region, src->region, 12U) == 0);

  ASSERT_TRUE(arena_copy(nullptr, src) == 0U);
  ASSERT_TRUE(arena_copy(dest, nullptr) == 0U);

  Arena invalid = {0};
  ASSERT_TRUE(arena_copy(&invalid, src) == 0U);
  ASSERT_TRUE(arena_copy(dest, &invalid) == 0U);

  arena_destroy(src);
  arena_destroy(dest);
  ASSERT_TRUE(arena_create(0U) == nullptr);
  arena_destroy(nullptr);
  return true;
}

int main() {
  typedef struct {
    const char *name;
    bool (*run)();
  } test_case_t;

  const test_case_t tests[] = {
      {.name = "basic_request_response", .run = test_basic_request_response},
      {.name = "notification_has_no_response",
       .run = test_notification_has_no_response},
      {.name = "parse_error_response", .run = test_parse_error_response},
      {.name = "method_not_found_response", .run = test_method_not_found_response},
      {.name = "invalid_params_response", .run = test_invalid_params_response},
      {.name = "batch_mixed_notification_and_request",
       .run = test_batch_mixed_notification_and_request},
      {.name = "send_result_and_send_error_api",
       .run = test_send_result_and_send_error_api},
      {.name = "send_failure_triggers_transport_close",
       .run = test_send_failure_triggers_transport_close},
      {.name = "invalid_request_shapes", .run = test_invalid_request_shapes},
      {.name = "batch_edge_cases", .run = test_batch_edge_cases},
      {.name = "framing_and_embedded_nul_parse_error",
       .run = test_framing_and_embedded_nul_parse_error},
      {.name = "request_too_large_closes_connection",
       .run = test_request_too_large_closes_connection},
      {.name = "inbound_buffer_overflow_closes_connection",
       .run = test_inbound_buffer_overflow_closes_connection},
      {.name = "request_handler_error_paths",
       .run = test_request_handler_error_paths},
      {.name = "no_request_callback_returns_method_not_found",
       .run = test_no_request_callback_returns_method_not_found},
      {.name = "connection_closed_during_on_open_returns_null",
       .run = test_connection_closed_during_on_open_returns_null},
      {.name = "arena_api_paths", .run = test_arena_api_paths},
  };

  size_t failures = 0U;
  const size_t total = sizeof(tests) / sizeof(tests[0]);
  for (size_t i = 0U; i < total; ++i) {
    if (tests[i].run()) {
      printf("[PASS] %s\n", tests[i].name);
    } else {
      printf("[FAIL] %s\n", tests[i].name);
      failures += 1U;
    }
  }

  if (failures != 0U) {
    fprintf(stderr, "%zu/%zu tests failed\n", failures, total);
    return EXIT_FAILURE;
  }

  printf("All %zu tests passed\n", total);
  return EXIT_SUCCESS;
}
