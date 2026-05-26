#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "websocket-server/websocket.h"

constexpr size_t LARGE_MESSAGE_LEN = 65'536U;
constexpr size_t MAX_MESSAGE_PAYLOAD = 1'048'576U;

typedef struct {
  uint8_t *last_data;
  size_t last_len;
  size_t send_count;
  size_t close_count;
  bool closed;
} transport_probe_t;

typedef struct {
  size_t on_open_calls;
  size_t on_message_calls;
  size_t on_close_calls;
  ws_opcode_t last_opcode;
  size_t last_message_len;
  uint8_t last_payload[256];
  size_t last_payload_len;
} callback_probe_t;

typedef struct {
  ws_conn_t *conn;
  transport_probe_t transport;
  callback_probe_t callbacks;
} test_fixture_t;

static bool g_failed = false;

static void fail_check(const char *expr, const char *file, int line) {
  fprintf(stderr, "[FAIL] %s at %s:%d\n", expr, file, line);
  g_failed = true;
}

#define CHECK(expr)                                                            \
  do {                                                                         \
    if (!(expr)) {                                                             \
      fail_check(#expr, __FILE__, __LINE__);                                   \
      return;                                                                  \
    }                                                                          \
  } while (false)

static void probe_clear_last_send(transport_probe_t *probe) {
  free(probe->last_data);
  probe->last_data = nullptr;
  probe->last_len = 0U;
}

static void probe_send_raw(ws_transport_t *self, const uint8_t *data,
                           size_t len) {
  auto probe = (transport_probe_t *)self->user_data;
  probe->send_count += 1U;

  probe_clear_last_send(probe);

  auto copy = (uint8_t *)calloc(len + 1U, sizeof(uint8_t));
  if (copy == nullptr) {
    return;
  }
  if (len > 0U) {
    memcpy(copy, data, len);
  }
  probe->last_data = copy;
  probe->last_len = len;
}

static void probe_close(ws_transport_t *self) {
  auto probe = (transport_probe_t *)self->user_data;
  probe->close_count += 1U;
  probe->closed = true;
}

static void cb_on_open(ws_conn_t *conn) {
  auto probe = (callback_probe_t *)ws_conn_get_context(conn);
  probe->on_open_calls += 1U;
}

static void cb_on_message(ws_conn_t *conn, const uint8_t *data, size_t len,
                          ws_opcode_t opcode) {
  auto probe = (callback_probe_t *)ws_conn_get_context(conn);
  probe->on_message_calls += 1U;
  probe->last_opcode = opcode;
  probe->last_message_len = len;
  probe->last_payload_len =
      len > sizeof(probe->last_payload) ? sizeof(probe->last_payload) : len;
  if (probe->last_payload_len > 0U) {
    memcpy(probe->last_payload, data, probe->last_payload_len);
  }
}

static void cb_on_close(ws_conn_t *conn) {
  auto probe = (callback_probe_t *)ws_conn_get_context(conn);
  probe->on_close_calls += 1U;
}

[[nodiscard]]
static bool fixture_init(test_fixture_t *fixture) {
  memset(fixture, 0, sizeof(*fixture));

  ws_transport_t transport = {.user_data = &fixture->transport,
                              .send_raw = probe_send_raw,
                              .close = probe_close};

  ws_callbacks_t callbacks = {.on_open = cb_on_open,
                              .on_message = cb_on_message,
                              .on_close = cb_on_close};

  fixture->conn = ws_conn_new(transport, callbacks, &fixture->callbacks);
  return fixture->conn != nullptr;
}

static void fixture_deinit(test_fixture_t *fixture) {
  ws_conn_free(fixture->conn);
  fixture->conn = nullptr;
  probe_clear_last_send(&fixture->transport);
}

static void do_handshake(test_fixture_t *fixture) {
  static constexpr char request[] =
      "GET /chat HTTP/1.1\r\n"
      "Host: server.example.com\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "\r\n";

  ws_conn_feed(fixture->conn, (const uint8_t *)request, sizeof(request) - 1U);
}

static void feed_request(test_fixture_t *fixture, const char *request) {
  ws_conn_feed(fixture->conn, (const uint8_t *)request, strlen(request));
}

static void reset_transport_observation(test_fixture_t *fixture) {
  probe_clear_last_send(&fixture->transport);
  fixture->transport.closed = false;
  fixture->transport.close_count = 0U;
}

[[nodiscard]]
static uint8_t *build_masked_client_frame(uint8_t first_byte,
                                          const uint8_t *data, size_t len,
                                          size_t *frame_len_out) {
  if (frame_len_out == nullptr || (len > 0U && data == nullptr)) {
    return nullptr;
  }

  size_t header_len = 2U;
  if (len <= 125U) {
    // No extended payload bytes.
  } else if (len <= 65'535U) {
    header_len += 2U;
  } else {
    header_len += 8U;
  }
  if (header_len > SIZE_MAX - 4U) {
    return nullptr;
  }
  const size_t with_mask = header_len + 4U;
  if (len > SIZE_MAX - with_mask) {
    return nullptr;
  }
  const size_t frame_len = with_mask + len;

  auto frame = (uint8_t *)calloc(frame_len, sizeof(uint8_t));
  if (frame == nullptr) {
    return nullptr;
  }

  static constexpr uint8_t mask[4] = {0x37U, 0xFAU, 0x21U, 0x3DU};

  size_t offset = 0U;
  frame[offset++] = first_byte;
  if (len <= 125U) {
    frame[offset++] = (uint8_t)(0x80U | (uint8_t)len);
  } else if (len <= 65'535U) {
    frame[offset++] = 0x80U | 126U;
    frame[offset++] = (uint8_t)((len >> 8U) & 0xFFU);
    frame[offset++] = (uint8_t)(len & 0xFFU);
  } else {
    const uint64_t len64 = (uint64_t)len;
    frame[offset++] = 0x80U | 127U;
    for (size_t i = 0U; i < 8U; ++i) {
      const uint32_t shift = (uint32_t)((7U - i) * 8U);
      frame[offset++] = (uint8_t)((len64 >> shift) & 0xFFU);
    }
  }

  memcpy(frame + offset, mask, sizeof(mask));
  offset += sizeof(mask);

  for (size_t i = 0U; i < len; ++i) {
    frame[offset + i] = (uint8_t)(data[i] ^ mask[i % 4U]);
  }

  *frame_len_out = frame_len;
  return frame;
}

[[nodiscard]]
static bool feed_masked_frame(test_fixture_t *fixture, uint8_t first_byte,
                              const uint8_t *data, size_t len) {
  size_t frame_len = 0U;
  auto frame = build_masked_client_frame(first_byte, data, len, &frame_len);
  if (frame == nullptr) {
    return false;
  }
  ws_conn_feed(fixture->conn, frame, frame_len);
  free(frame);
  return true;
}

[[nodiscard]]
static bool feed_unmasked_frame(test_fixture_t *fixture, uint8_t first_byte,
                                const uint8_t *data, size_t len) {
  if (len > 125U || (len > 0U && data == nullptr)) {
    return false;
  }
  if (len > SIZE_MAX - 2U) {
    return false;
  }
  const size_t frame_len = 2U + len;
  auto frame = (uint8_t *)calloc(frame_len, sizeof(uint8_t));
  if (frame == nullptr) {
    return false;
  }
  frame[0] = first_byte;
  frame[1] = (uint8_t)len;
  if (len > 0U) {
    memcpy(frame + 2U, data, len);
  }
  ws_conn_feed(fixture->conn, frame, frame_len);
  free(frame);
  return true;
}

[[nodiscard]]
static bool last_send_is_close_with_code(const test_fixture_t *fixture,
                                         uint16_t code) {
  if (fixture->transport.last_data == nullptr ||
      fixture->transport.last_len != 4U) {
    return false;
  }
  if (fixture->transport.last_data[0] != 0x88U ||
      fixture->transport.last_data[1] != 0x02U) {
    return false;
  }

  const uint16_t observed_code =
      ((uint16_t)fixture->transport.last_data[2] << 8U) |
      fixture->transport.last_data[3];
  return observed_code == code;
}

static void test_handshake_success_sends_accept() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);

  CHECK(fixture.callbacks.on_open_calls == 1U);
  CHECK(fixture.transport.send_count == 1U);
  CHECK(fixture.transport.last_data != nullptr);
  CHECK(strstr((const char *)fixture.transport.last_data,
               "HTTP/1.1 101 Switching Protocols") != nullptr);
  CHECK(strstr((const char *)fixture.transport.last_data,
               "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=") !=
        nullptr);

  fixture_deinit(&fixture);
}

static void test_handshake_with_token_lists_succeeds() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));

  static constexpr char request[] =
      "GET /chat HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Upgrade: h2c, websocket\r\n"
      "Connection: keep-alive, Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "\r\n";

  feed_request(&fixture, request);

  CHECK(fixture.callbacks.on_open_calls == 1U);
  CHECK(fixture.transport.send_count == 1U);
  CHECK(fixture.transport.last_data != nullptr);
  CHECK(strstr((const char *)fixture.transport.last_data,
               "HTTP/1.1 101 Switching Protocols") != nullptr);

  fixture_deinit(&fixture);
}

static void test_handshake_failure_returns_http_400_and_closes() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));

  static constexpr char bad_request[] = "GET / HTTP/1.1\r\n"
                                        "Host: localhost\r\n"
                                        "Upgrade: websocket\r\n"
                                        "Connection: Upgrade\r\n"
                                        "Sec-WebSocket-Version: 13\r\n"
                                        "\r\n";

  ws_conn_feed(fixture.conn, (const uint8_t *)bad_request,
               sizeof(bad_request) - 1U);

  CHECK(fixture.callbacks.on_open_calls == 0U);
  CHECK(fixture.transport.send_count >= 1U);
  CHECK(fixture.transport.last_data != nullptr);
  CHECK(strstr((const char *)fixture.transport.last_data, "400 Bad Request") !=
        nullptr);
  CHECK(fixture.transport.closed);
  CHECK(fixture.transport.close_count >= 1U);

  fixture_deinit(&fixture);
}

static void test_handshake_invalid_request_line_returns_http_400() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));

  static constexpr char request[] =
      "POST /chat HTTP/1.1\r\n"
      "Host: localhost\r\n"
      "Upgrade: websocket\r\n"
      "Connection: Upgrade\r\n"
      "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
      "Sec-WebSocket-Version: 13\r\n"
      "\r\n";

  feed_request(&fixture, request);

  CHECK(fixture.callbacks.on_open_calls == 0U);
  CHECK(fixture.transport.closed);
  CHECK(fixture.transport.last_data != nullptr);
  CHECK(strstr((const char *)fixture.transport.last_data, "400 Bad Request") !=
        nullptr);

  fixture_deinit(&fixture);
}

static void test_handshake_oversized_header_returns_http_400() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));

  constexpr size_t oversized_len = 8'300U;
  auto oversized = (uint8_t *)calloc(oversized_len, sizeof(uint8_t));
  CHECK(oversized != nullptr);
  memset(oversized, 'A', oversized_len);
  ws_conn_feed(fixture.conn, oversized, oversized_len);
  free(oversized);

  CHECK(fixture.callbacks.on_open_calls == 0U);
  CHECK(fixture.transport.closed);
  CHECK(fixture.transport.last_data != nullptr);
  CHECK(strstr((const char *)fixture.transport.last_data, "400 Bad Request") !=
        nullptr);

  fixture_deinit(&fixture);
}

static void test_text_frame_dispatches_message_callback() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);

  static constexpr uint8_t payload[] = {'h', 'e', 'l', 'l', 'o'};
  CHECK(feed_masked_frame(&fixture, 0x80U | WS_OP_TEXT, payload,
                          sizeof(payload)));

  CHECK(fixture.callbacks.on_message_calls == 1U);
  CHECK(fixture.callbacks.last_opcode == WS_OP_TEXT);
  CHECK(fixture.callbacks.last_message_len == sizeof(payload));
  CHECK(fixture.callbacks.last_payload_len == sizeof(payload));
  CHECK(memcmp(fixture.callbacks.last_payload, payload, sizeof(payload)) == 0);

  fixture_deinit(&fixture);
}

static void test_text_frame_invalid_utf8_results_1007() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t payload[] = {0xC0U, 0xAFU};
  CHECK(feed_masked_frame(&fixture, 0x80U | WS_OP_TEXT, payload,
                          sizeof(payload)));

  CHECK(fixture.callbacks.on_message_calls == 0U);
  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1007U));

  fixture_deinit(&fixture);
}

static void test_extended_126_frame_dispatches_message_callback() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);

  uint8_t payload[126];
  for (size_t i = 0U; i < sizeof(payload); ++i) {
    payload[i] = (uint8_t)(i & 0xFFU);
  }
  CHECK(feed_masked_frame(&fixture, 0x80U | WS_OP_TEXT, payload,
                          sizeof(payload)));

  CHECK(fixture.callbacks.on_message_calls == 1U);
  CHECK(fixture.callbacks.last_opcode == WS_OP_TEXT);
  CHECK(fixture.callbacks.last_message_len == sizeof(payload));
  CHECK(fixture.callbacks.last_payload_len == sizeof(payload));
  CHECK(memcmp(fixture.callbacks.last_payload, payload, sizeof(payload)) == 0);

  fixture_deinit(&fixture);
}

static void test_extended_127_frame_dispatches_binary_callback() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);

  auto payload = (uint8_t *)calloc(LARGE_MESSAGE_LEN, sizeof(uint8_t));
  CHECK(payload != nullptr);
  for (size_t i = 0U; i < LARGE_MESSAGE_LEN; ++i) {
    payload[i] = (uint8_t)(i % 251U);
  }
  CHECK(feed_masked_frame(&fixture, 0x80U | WS_OP_BINARY, payload,
                          LARGE_MESSAGE_LEN));

  CHECK(fixture.callbacks.on_message_calls == 1U);
  CHECK(fixture.callbacks.last_opcode == WS_OP_BINARY);
  CHECK(fixture.callbacks.last_message_len == LARGE_MESSAGE_LEN);
  CHECK(fixture.callbacks.last_payload_len ==
        sizeof(fixture.callbacks.last_payload));
  CHECK(memcmp(fixture.callbacks.last_payload, payload,
               sizeof(fixture.callbacks.last_payload)) == 0);

  free(payload);
  fixture_deinit(&fixture);
}

static void test_ping_generates_pong_frame() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t payload[] = {'O', 'K'};
  CHECK(feed_masked_frame(&fixture, 0x80U | WS_OP_PING, payload,
                          sizeof(payload)));

  CHECK(fixture.transport.last_data != nullptr);
  CHECK(fixture.transport.last_len == sizeof(payload) + 2U);
  CHECK(fixture.transport.last_data[0] == 0x8AU);
  CHECK(fixture.transport.last_data[1] == sizeof(payload));
  CHECK(memcmp(fixture.transport.last_data + 2U, payload, sizeof(payload)) ==
        0);

  fixture_deinit(&fixture);
}

static void test_pong_frame_is_ignored() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t payload[] = {'A', 'B'};
  CHECK(feed_masked_frame(&fixture, 0x80U | WS_OP_PONG, payload,
                          sizeof(payload)));

  CHECK(fixture.callbacks.on_message_calls == 0U);
  CHECK(fixture.transport.last_data == nullptr);
  CHECK(!fixture.transport.closed);

  fixture_deinit(&fixture);
}

static void test_rsv_frame_is_protocol_error() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t payload[] = {'X'};
  CHECK(feed_masked_frame(&fixture, 0xC1U, payload, sizeof(payload)));

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_unmasked_frame_is_protocol_error() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t payload[] = {'n', 'o'};
  CHECK(feed_unmasked_frame(&fixture, 0x80U | WS_OP_TEXT, payload,
                            sizeof(payload)));

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_non_final_control_frame_is_protocol_error() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  CHECK(feed_masked_frame(&fixture, WS_OP_PING, nullptr, 0U));

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_invalid_extended_126_length_is_protocol_error() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t mask[4] = {0x37U, 0xFAU, 0x21U, 0x3DU};
  uint8_t frame[8U + 125U];
  memset(frame, 0, sizeof(frame));
  frame[0] = 0x80U | WS_OP_TEXT;
  frame[1] = 0x80U | 126U;
  frame[2] = 0x00U;
  frame[3] = 125U;
  memcpy(frame + 4U, mask, sizeof(mask));
  for (size_t i = 0U; i < 125U; ++i) {
    frame[8U + i] = mask[i % 4U];
  }
  ws_conn_feed(fixture.conn, frame, sizeof(frame));

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_invalid_extended_127_msb_is_protocol_error() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  uint8_t frame[10];
  memset(frame, 0, sizeof(frame));
  frame[0] = 0x80U | WS_OP_BINARY;
  frame[1] = 0x80U | 127U;
  frame[2] = 0x80U;
  ws_conn_feed(fixture.conn, frame, sizeof(frame));

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_invalid_extended_127_small_value_is_protocol_error() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  uint8_t frame[10];
  memset(frame, 0, sizeof(frame));
  frame[0] = 0x80U | WS_OP_BINARY;
  frame[1] = 0x80U | 127U;
  frame[8] = 0xFFU;
  frame[9] = 0xFFU;
  ws_conn_feed(fixture.conn, frame, sizeof(frame));

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_inbound_payload_too_large_is_1009() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  uint8_t frame[14];
  memset(frame, 0, sizeof(frame));
  frame[0] = 0x80U | WS_OP_BINARY;
  frame[1] = 0x80U | 127U;
  const uint64_t oversized = (uint64_t)MAX_MESSAGE_PAYLOAD + 1U;
  for (size_t i = 0U; i < 8U; ++i) {
    const uint32_t shift = (uint32_t)((7U - i) * 8U);
    frame[2U + i] = (uint8_t)((oversized >> shift) & 0xFFU);
  }
  ws_conn_feed(fixture.conn, frame, sizeof(frame));

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1009U));

  fixture_deinit(&fixture);
}

static void test_control_frame_payload_too_large_is_protocol_error() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  uint8_t payload[126];
  memset(payload, 0xA5, sizeof(payload));
  CHECK(feed_masked_frame(&fixture, 0x80U | WS_OP_PING, payload,
                          sizeof(payload)));

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_close_frame_invalid_code_results_1002() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t payload[] = {0x03U, 0xEDU};
  CHECK(feed_masked_frame(&fixture, 0x80U | WS_OP_CLOSE, payload,
                          sizeof(payload)));

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_close_frame_invalid_utf8_results_1007() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t payload[] = {0x03U, 0xE8U, 0xC0U};
  CHECK(feed_masked_frame(&fixture, 0x80U | WS_OP_CLOSE, payload,
                          sizeof(payload)));

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1007U));

  fixture_deinit(&fixture);
}

static void test_close_frame_echoes_valid_code() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t payload[] = {0x03U, 0xE9U, 'b', 'y', 'e'};
  CHECK(feed_masked_frame(&fixture, 0x80U | WS_OP_CLOSE, payload,
                          sizeof(payload)));

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1001U));

  fixture_deinit(&fixture);
}

static void test_server_send_text_frame() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t payload[] = {'y', 'o'};
  ws_conn_send(fixture.conn, payload, sizeof(payload), WS_OP_TEXT);

  CHECK(fixture.transport.last_data != nullptr);
  CHECK(fixture.transport.last_len == sizeof(payload) + 2U);
  CHECK(fixture.transport.last_data[0] == 0x81U);
  CHECK(fixture.transport.last_data[1] == sizeof(payload));
  CHECK(memcmp(fixture.transport.last_data + 2U, payload, sizeof(payload)) ==
        0);

  fixture_deinit(&fixture);
}

static void test_server_send_binary_126_frame() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  uint8_t payload[126];
  for (size_t i = 0U; i < sizeof(payload); ++i) {
    payload[i] = (uint8_t)(255U - i);
  }
  ws_conn_send(fixture.conn, payload, sizeof(payload), WS_OP_BINARY);

  CHECK(fixture.transport.last_data != nullptr);
  CHECK(fixture.transport.last_len == sizeof(payload) + 4U);
  CHECK(fixture.transport.last_data[0] == 0x82U);
  CHECK(fixture.transport.last_data[1] == 126U);
  CHECK(fixture.transport.last_data[2] == 0x00U);
  CHECK(fixture.transport.last_data[3] == 126U);
  CHECK(memcmp(fixture.transport.last_data + 4U, payload, sizeof(payload)) ==
        0);

  fixture_deinit(&fixture);
}

static void test_server_send_binary_127_frame() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  auto payload = (uint8_t *)calloc(LARGE_MESSAGE_LEN, sizeof(uint8_t));
  CHECK(payload != nullptr);
  for (size_t i = 0U; i < LARGE_MESSAGE_LEN; ++i) {
    payload[i] = (uint8_t)(i % 127U);
  }
  ws_conn_send(fixture.conn, payload, LARGE_MESSAGE_LEN, WS_OP_BINARY);

  CHECK(fixture.transport.last_data != nullptr);
  CHECK(fixture.transport.last_len == LARGE_MESSAGE_LEN + 10U);
  CHECK(fixture.transport.last_data[0] == 0x82U);
  CHECK(fixture.transport.last_data[1] == 127U);

  uint64_t observed_len = 0U;
  for (size_t i = 0U; i < 8U; ++i) {
    observed_len = (observed_len << 8U) | fixture.transport.last_data[2U + i];
  }
  CHECK(observed_len == LARGE_MESSAGE_LEN);
  CHECK(memcmp(fixture.transport.last_data + 10U, payload, LARGE_MESSAGE_LEN) ==
        0);

  free(payload);
  fixture_deinit(&fixture);
}

static void test_server_send_ignored_before_handshake() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));

  static constexpr uint8_t payload[] = {'z'};
  ws_conn_send(fixture.conn, payload, sizeof(payload), WS_OP_TEXT);

  CHECK(fixture.transport.send_count == 0U);
  CHECK(!fixture.transport.closed);

  fixture_deinit(&fixture);
}

static void test_server_send_rejects_null_payload_with_nonzero_len() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  ws_conn_send(fixture.conn, nullptr, 1U, WS_OP_TEXT);

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_server_send_rejects_continuation_opcode() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t payload[] = {'a'};
  ws_conn_send(fixture.conn, payload, sizeof(payload), WS_OP_CONTINUATION);

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_server_send_rejects_invalid_close_payload() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static constexpr uint8_t payload[] = {0x00U};
  ws_conn_send(fixture.conn, payload, sizeof(payload), WS_OP_CLOSE);

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_server_send_rejects_too_large_control_payload() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  uint8_t payload[126];
  memset(payload, 0x11, sizeof(payload));
  ws_conn_send(fixture.conn, payload, sizeof(payload), WS_OP_PING);

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1002U));

  fixture_deinit(&fixture);
}

static void test_server_send_rejects_too_large_message_payload() {
  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));
  do_handshake(&fixture);
  reset_transport_observation(&fixture);

  static uint8_t marker = 0x42U;
  ws_conn_send(fixture.conn, &marker, MAX_MESSAGE_PAYLOAD + 1U, WS_OP_BINARY);

  CHECK(fixture.transport.closed);
  CHECK(last_send_is_close_with_code(&fixture, 1009U));

  fixture_deinit(&fixture);
}

static void test_ws_conn_guards_and_null_context() {
  CHECK(ws_conn_get_context(nullptr) == nullptr);

  test_fixture_t fixture;
  CHECK(fixture_init(&fixture));

  static constexpr uint8_t byte = 'q';
  ws_conn_feed(nullptr, &byte, 1U);
  ws_conn_feed(fixture.conn, nullptr, 1U);
  ws_conn_feed(fixture.conn, &byte, 0U);

  CHECK(fixture.transport.send_count == 0U);
  CHECK(!fixture.transport.closed);

  fixture_deinit(&fixture);
}

int main() {
  test_handshake_success_sends_accept();
  test_handshake_with_token_lists_succeeds();
  test_handshake_failure_returns_http_400_and_closes();
  test_handshake_invalid_request_line_returns_http_400();
  test_handshake_oversized_header_returns_http_400();
  test_text_frame_dispatches_message_callback();
  test_text_frame_invalid_utf8_results_1007();
  test_extended_126_frame_dispatches_message_callback();
  test_extended_127_frame_dispatches_binary_callback();
  test_ping_generates_pong_frame();
  test_pong_frame_is_ignored();
  test_rsv_frame_is_protocol_error();
  test_unmasked_frame_is_protocol_error();
  test_non_final_control_frame_is_protocol_error();
  test_invalid_extended_126_length_is_protocol_error();
  test_invalid_extended_127_msb_is_protocol_error();
  test_invalid_extended_127_small_value_is_protocol_error();
  test_inbound_payload_too_large_is_1009();
  test_control_frame_payload_too_large_is_protocol_error();
  test_close_frame_invalid_code_results_1002();
  test_close_frame_invalid_utf8_results_1007();
  test_close_frame_echoes_valid_code();
  test_server_send_text_frame();
  test_server_send_binary_126_frame();
  test_server_send_binary_127_frame();
  test_server_send_ignored_before_handshake();
  test_server_send_rejects_null_payload_with_nonzero_len();
  test_server_send_rejects_continuation_opcode();
  test_server_send_rejects_invalid_close_payload();
  test_server_send_rejects_too_large_control_payload();
  test_server_send_rejects_too_large_message_payload();
  test_ws_conn_guards_and_null_context();

  if (g_failed) {
    return EXIT_FAILURE;
  }
  printf("All unit tests passed.\n");
  return EXIT_SUCCESS;
}
