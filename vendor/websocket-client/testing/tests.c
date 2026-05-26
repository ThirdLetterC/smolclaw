#include "websocket_client/websocket_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#define TEST(name)                                                             \
  do {                                                                         \
    printf("Running: %s\n", #name);                                            \
    if (run_test_##name()) {                                                   \
      printf("  ✓ PASSED\n");                                                  \
      tests_passed++;                                                          \
    } else {                                                                   \
      printf("  ✗ FAILED\n");                                                  \
      tests_failed++;                                                          \
    }                                                                          \
  } while (0)

constexpr size_t TEST_HTTP_BUFFER_CAPACITY = 2048;
constexpr size_t TEST_MAX_FRAME_PAYLOAD = 256;
constexpr char TEST_ACCEPT_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr char TEST_BASE64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

typedef struct {
  uint32_t state[5];
  uint64_t total_bits;
  uint8_t block[64];
  size_t block_length;
} test_sha1_ctx_t;

typedef struct {
  uint8_t opcode;
  bool fin;
  size_t payload_length;
  uint8_t payload[TEST_MAX_FRAME_PAYLOAD];
} test_frame_t;

typedef enum {
  TEST_SERVER_HANDSHAKE_WAIT_CLOSE = 1,
  TEST_SERVER_RAW_ACCEPT_ONLY,
  TEST_SERVER_BAD_ACCEPT,
  TEST_SERVER_BAD_STATUS,
  TEST_SERVER_MISSING_ACCEPT,
  TEST_SERVER_EXPECT_TEXT,
  TEST_SERVER_EXPECT_BINARY,
  TEST_SERVER_SEND_TEXT,
  TEST_SERVER_SEND_BINARY,
  TEST_SERVER_PING_THEN_TEXT,
  TEST_SERVER_SEND_FRAGMENTED_TEXT,
  TEST_SERVER_SEND_MASKED_TEXT,
  TEST_SERVER_SEND_UNEXPECTED_BINARY,
} test_server_mode_t;

typedef struct {
  pid_t pid;
  uint16_t port;
} test_server_t;

[[nodiscard]] static bool run_test_last_error_null_client();
[[nodiscard]] static bool run_test_create_sets_no_error();
[[nodiscard]] static bool run_test_connect_rejects_null_client();
[[nodiscard]] static bool run_test_connect_rejects_null_host();
[[nodiscard]] static bool run_test_connect_rejects_null_path();
[[nodiscard]] static bool run_test_connect_rejects_invalid_host();
[[nodiscard]] static bool run_test_connect_rejects_invalid_path();
[[nodiscard]] static bool run_test_connect_secure_rejects_invalid_host();
[[nodiscard]] static bool run_test_send_text_rejects_null_payload();
[[nodiscard]] static bool run_test_send_binary_rejects_null_payload();
[[nodiscard]] static bool run_test_send_text_fails_when_disconnected();
[[nodiscard]] static bool run_test_send_binary_fails_when_disconnected();
[[nodiscard]] static bool run_test_receive_text_rejects_invalid_buffer();
[[nodiscard]] static bool run_test_receive_binary_rejects_invalid_buffer();
[[nodiscard]] static bool run_test_receive_text_fails_when_disconnected();
[[nodiscard]] static bool run_test_receive_binary_fails_when_disconnected();
[[nodiscard]] static bool run_test_close_and_destroy_allow_null();
[[nodiscard]] static bool run_test_connect_success_with_mock_server();
[[nodiscard]] static bool run_test_connect_fails_on_bad_accept();
[[nodiscard]] static bool run_test_connect_fails_on_bad_status();
[[nodiscard]] static bool run_test_connect_fails_on_missing_accept();
[[nodiscard]] static bool run_test_connect_secure_fails_against_plain_server();
[[nodiscard]] static bool run_test_send_text_to_mock_server();
[[nodiscard]] static bool run_test_send_binary_to_mock_server();
[[nodiscard]] static bool run_test_receive_text_from_mock_server();
[[nodiscard]] static bool run_test_receive_binary_from_mock_server();
[[nodiscard]] static bool run_test_receive_text_fails_on_small_buffer();
[[nodiscard]] static bool run_test_receive_binary_fails_on_small_buffer();
[[nodiscard]] static bool run_test_receive_text_handles_ping_pong();
[[nodiscard]] static bool
run_test_receive_text_reassembles_fragmented_message();
[[nodiscard]] static bool run_test_receive_text_rejects_masked_server_frame();
[[nodiscard]] static bool run_test_receive_text_rejects_unexpected_binary();

[[nodiscard]] static bool expect_error_equals(const ws_client_t *client,
                                              const char *expected);
[[nodiscard]] static bool expect_error_contains(const ws_client_t *client,
                                                const char *expected_fragment);

[[nodiscard]] static bool test_read_exact(int fd, void *buffer, size_t length);
[[nodiscard]] static bool test_send_all(int fd, const void *buffer,
                                        size_t length);
[[nodiscard]] static bool test_read_http_request(int fd, char *request,
                                                 size_t capacity);
[[nodiscard]] static bool test_find_header_value(const char *http,
                                                 const char *header_name,
                                                 char *value,
                                                 size_t value_capacity);
[[nodiscard]] static bool test_compute_accept_value(const char *key,
                                                    char *output,
                                                    size_t output_capacity);
[[nodiscard]] static bool
test_send_handshake_response(int fd, const char *accept_value);

[[nodiscard]] static bool test_send_frame(int fd, uint8_t opcode, bool fin,
                                          bool masked, const uint8_t *payload,
                                          size_t payload_length);
[[nodiscard]] static bool test_read_frame(int fd, test_frame_t *frame,
                                          bool require_masked);
[[nodiscard]] static bool test_expect_close_frame(int fd);

[[nodiscard]] static bool test_server_start(test_server_t *server,
                                            test_server_mode_t mode);
[[nodiscard]] static bool test_server_wait_success(test_server_t *server);
static void test_server_abort(test_server_t *server);
[[nodiscard]] static bool test_server_child(test_server_mode_t mode,
                                            int listen_fd);
[[nodiscard]] static bool test_server_handle_mode(test_server_mode_t mode,
                                                  int conn_fd);

[[nodiscard]] static bool test_send_response_missing_accept(int fd);

[[nodiscard]] static uint32_t test_rotl32(uint32_t value, unsigned shift);
static void test_sha1_init(test_sha1_ctx_t *ctx);
static void test_sha1_transform(test_sha1_ctx_t *ctx, const uint8_t *block);
static void test_sha1_update(test_sha1_ctx_t *ctx, const uint8_t *data,
                             size_t length);
static void test_sha1_final(test_sha1_ctx_t *ctx, uint8_t digest[20]);
[[nodiscard]] static size_t test_base64_encode(const uint8_t *input,
                                               size_t input_length,
                                               char *output,
                                               size_t output_capacity);

[[nodiscard]] static bool expect_error_equals(const ws_client_t *client,
                                              const char *expected) {
  auto actual = ws_client_last_error(client);
  if (strcmp(actual, expected) == 0) {
    return true;
  }

  printf("  expected error: \"%s\"\n", expected);
  printf("  actual error:   \"%s\"\n", actual);
  return false;
}

[[nodiscard]] static bool expect_error_contains(const ws_client_t *client,
                                                const char *expected_fragment) {
  auto actual = ws_client_last_error(client);
  if (strstr(actual, expected_fragment) != nullptr) {
    return true;
  }

  printf("  expected error to contain: \"%s\"\n", expected_fragment);
  printf("  actual error:              \"%s\"\n", actual);
  return false;
}

[[nodiscard]] static bool test_read_exact(int fd, void *buffer, size_t length) {
  if (buffer == nullptr && length != 0U) {
    return false;
  }

  auto bytes = (uint8_t *)buffer;
  size_t total = 0;
  while (total < length) {
    auto received = recv(fd, bytes + total, length - total, 0);
    if (received < 0 && errno == EINTR) {
      continue;
    }
    if (received <= 0) {
      return false;
    }
    total += (size_t)received;
  }
  return true;
}

[[nodiscard]] static bool test_send_all(int fd, const void *buffer,
                                        size_t length) {
  if (buffer == nullptr && length != 0U) {
    return false;
  }

  auto bytes = (const uint8_t *)buffer;
  size_t total = 0;
  while (total < length) {
    auto sent = send(fd, bytes + total, length - total, 0);
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    if (sent <= 0) {
      return false;
    }
    total += (size_t)sent;
  }
  return true;
}

[[nodiscard]] static bool test_read_http_request(int fd, char *request,
                                                 size_t capacity) {
  if (request == nullptr || capacity < 2U) {
    return false;
  }

  request[0] = '\0';
  size_t length = 0;
  uint32_t marker = 0;

  while (length + 1U < capacity) {
    uint8_t byte = 0;
    if (!test_read_exact(fd, &byte, sizeof(byte))) {
      return false;
    }
    if (byte == '\0') {
      return false;
    }

    request[length++] = (char)byte;
    request[length] = '\0';
    marker = (marker << 8U) | (uint32_t)byte;
    if (marker == 0x0D0A0D0AU) {
      return true;
    }
  }

  return false;
}

[[nodiscard]] static bool test_find_header_value(const char *http,
                                                 const char *header_name,
                                                 char *value,
                                                 size_t value_capacity) {
  if (http == nullptr || header_name == nullptr || header_name[0] == '\0' ||
      value == nullptr || value_capacity == 0U) {
    return false;
  }

  value[0] = '\0';
  auto header_name_length = strlen(header_name);
  auto line = strstr(http, "\r\n");
  if (line == nullptr) {
    return false;
  }
  line += 2;

  while (*line != '\0') {
    auto line_end = strstr(line, "\r\n");
    if (line_end == nullptr) {
      return false;
    }
    if (line_end == line) {
      return false;
    }

    auto line_length = (size_t)(line_end - line);
    const char *colon = (const char *)memchr(line, ':', line_length);
    if (colon != nullptr) {
      auto current_name_length = (size_t)(colon - line);
      if (current_name_length == header_name_length &&
          strncasecmp(line, header_name, header_name_length) == 0) {
        const char *raw = colon + 1;
        while (*raw == ' ' || *raw == '\t') {
          ++raw;
        }

        const char *raw_end = line_end;
        while (raw_end > raw && (raw_end[-1] == ' ' || raw_end[-1] == '\t')) {
          --raw_end;
        }

        auto value_length = (size_t)(raw_end - raw);
        if (value_length + 1U > value_capacity) {
          return false;
        }

        memcpy(value, raw, value_length);
        value[value_length] = '\0';
        return true;
      }
    }

    line = line_end + 2;
  }

  return false;
}

[[nodiscard]] static uint32_t test_rotl32(uint32_t value, unsigned shift) {
  return (value << shift) | (value >> (32U - shift));
}

static void test_sha1_init(test_sha1_ctx_t *ctx) {
  if (ctx == nullptr) {
    return;
  }

  ctx->state[0] = 0x67452301U;
  ctx->state[1] = 0xEFCDAB89U;
  ctx->state[2] = 0x98BADCFEU;
  ctx->state[3] = 0x10325476U;
  ctx->state[4] = 0xC3D2E1F0U;
  ctx->total_bits = 0;
  ctx->block_length = 0;
}

static void test_sha1_transform(test_sha1_ctx_t *ctx, const uint8_t *block) {
  if (ctx == nullptr || block == nullptr) {
    return;
  }

  uint32_t words[80] = {0};
  for (size_t i = 0; i < 16; ++i) {
    auto offset = i * 4U;
    words[i] = ((uint32_t)block[offset] << 24U) |
               ((uint32_t)block[offset + 1U] << 16U) |
               ((uint32_t)block[offset + 2U] << 8U) |
               (uint32_t)block[offset + 3U];
  }

  for (size_t i = 16; i < 80; ++i) {
    words[i] = test_rotl32(
        words[i - 3U] ^ words[i - 8U] ^ words[i - 14U] ^ words[i - 16U], 1U);
  }

  auto a = ctx->state[0];
  auto b = ctx->state[1];
  auto c = ctx->state[2];
  auto d = ctx->state[3];
  auto e = ctx->state[4];

  for (size_t i = 0; i < 80; ++i) {
    uint32_t f = 0;
    uint32_t k = 0;

    if (i < 20) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999U;
    } else if (i < 40) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1U;
    } else if (i < 60) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCU;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6U;
    }

    auto temp = test_rotl32(a, 5U) + f + e + k + words[i];
    e = d;
    d = c;
    c = test_rotl32(b, 30U);
    b = a;
    a = temp;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
}

static void test_sha1_update(test_sha1_ctx_t *ctx, const uint8_t *data,
                             size_t length) {
  if (ctx == nullptr) {
    return;
  }
  if (data == nullptr && length != 0U) {
    return;
  }
  if (length == 0U) {
    return;
  }

  ctx->total_bits += (uint64_t)length * 8U;
  size_t index = 0;
  while (index < length) {
    auto space = sizeof(ctx->block) - ctx->block_length;
    auto remaining = length - index;
    auto to_copy = (remaining < space) ? remaining : space;
    memcpy(ctx->block + ctx->block_length, data + index, to_copy);
    ctx->block_length += to_copy;
    index += to_copy;

    if (ctx->block_length == sizeof(ctx->block)) {
      test_sha1_transform(ctx, ctx->block);
      ctx->block_length = 0;
    }
  }
}

static void test_sha1_final(test_sha1_ctx_t *ctx, uint8_t digest[20]) {
  if (ctx == nullptr || digest == nullptr) {
    return;
  }

  ctx->block[ctx->block_length++] = 0x80U;
  if (ctx->block_length > 56U) {
    while (ctx->block_length < 64U) {
      ctx->block[ctx->block_length++] = 0U;
    }
    test_sha1_transform(ctx, ctx->block);
    ctx->block_length = 0U;
  }

  while (ctx->block_length < 56U) {
    ctx->block[ctx->block_length++] = 0U;
  }

  for (size_t i = 0; i < 8U; ++i) {
    auto shift = (7U - i) * 8U;
    ctx->block[56U + i] = (uint8_t)((ctx->total_bits >> shift) & 0xFFU);
  }
  test_sha1_transform(ctx, ctx->block);

  for (size_t i = 0; i < 5U; ++i) {
    digest[i * 4U] = (uint8_t)((ctx->state[i] >> 24U) & 0xFFU);
    digest[i * 4U + 1U] = (uint8_t)((ctx->state[i] >> 16U) & 0xFFU);
    digest[i * 4U + 2U] = (uint8_t)((ctx->state[i] >> 8U) & 0xFFU);
    digest[i * 4U + 3U] = (uint8_t)(ctx->state[i] & 0xFFU);
  }
}

[[nodiscard]] static size_t test_base64_encode(const uint8_t *input,
                                               size_t input_length,
                                               char *output,
                                               size_t output_capacity) {
  if (output == nullptr || output_capacity == 0U) {
    return 0;
  }
  if (input == nullptr && input_length != 0U) {
    return 0;
  }

  auto encoded_length = ((input_length + 2U) / 3U) * 4U;
  if (encoded_length + 1U > output_capacity) {
    return 0;
  }

  size_t src = 0;
  size_t dst = 0;
  while (src + 2U < input_length) {
    auto chunk = ((uint32_t)input[src] << 16U) |
                 ((uint32_t)input[src + 1U] << 8U) | (uint32_t)input[src + 2U];
    output[dst++] = TEST_BASE64_TABLE[(chunk >> 18U) & 0x3FU];
    output[dst++] = TEST_BASE64_TABLE[(chunk >> 12U) & 0x3FU];
    output[dst++] = TEST_BASE64_TABLE[(chunk >> 6U) & 0x3FU];
    output[dst++] = TEST_BASE64_TABLE[chunk & 0x3FU];
    src += 3U;
  }

  if (src < input_length) {
    auto chunk = (uint32_t)input[src] << 16U;
    if (src + 1U < input_length) {
      chunk |= (uint32_t)input[src + 1U] << 8U;
    }

    output[dst++] = TEST_BASE64_TABLE[(chunk >> 18U) & 0x3FU];
    output[dst++] = TEST_BASE64_TABLE[(chunk >> 12U) & 0x3FU];
    if (src + 1U < input_length) {
      output[dst++] = TEST_BASE64_TABLE[(chunk >> 6U) & 0x3FU];
    } else {
      output[dst++] = '=';
    }
    output[dst++] = '=';
  }

  output[dst] = '\0';
  return dst;
}

[[nodiscard]] static bool test_compute_accept_value(const char *key,
                                                    char *output,
                                                    size_t output_capacity) {
  if (key == nullptr || output == nullptr || output_capacity == 0U) {
    return false;
  }

  char challenge[128] = {0};
  auto challenge_length =
      snprintf(challenge, sizeof(challenge), "%s%s", key, TEST_ACCEPT_GUID);
  if (challenge_length < 0 || (size_t)challenge_length >= sizeof(challenge)) {
    return false;
  }

  test_sha1_ctx_t sha1 = {0};
  uint8_t digest[20] = {0};
  test_sha1_init(&sha1);
  test_sha1_update(&sha1, (const uint8_t *)challenge, (size_t)challenge_length);
  test_sha1_final(&sha1, digest);
  return test_base64_encode(digest, sizeof(digest), output, output_capacity) !=
         0;
}

[[nodiscard]] static bool
test_send_handshake_response(int fd, const char *accept_value) {
  if (accept_value == nullptr) {
    return false;
  }

  char response[512] = {0};
  auto written = snprintf(response, sizeof(response),
                          "HTTP/1.1 101 Switching Protocols\r\n"
                          "Upgrade: websocket\r\n"
                          "Connection: Upgrade\r\n"
                          "Sec-WebSocket-Accept: %s\r\n"
                          "\r\n",
                          accept_value);
  if (written < 0 || (size_t)written >= sizeof(response)) {
    return false;
  }

  return test_send_all(fd, response, (size_t)written);
}

[[nodiscard]] static bool test_send_response_missing_accept(int fd) {
  constexpr char response[] = "HTTP/1.1 101 Switching Protocols\r\n"
                              "Upgrade: websocket\r\n"
                              "Connection: Upgrade\r\n"
                              "\r\n";
  return test_send_all(fd, response, sizeof(response) - 1U);
}

[[nodiscard]] static bool test_send_frame(int fd, uint8_t opcode, bool fin,
                                          bool masked, const uint8_t *payload,
                                          size_t payload_length) {
  if (payload == nullptr && payload_length != 0U) {
    return false;
  }
  if (payload_length > UINT16_MAX) {
    return false;
  }

  uint8_t header[14] = {0};
  size_t header_length = 0;
  header[header_length++] = (uint8_t)((fin ? 0x80U : 0x00U) | (opcode & 0x0FU));

  if (payload_length <= 125U) {
    header[header_length++] =
        (uint8_t)((masked ? 0x80U : 0x00U) | (uint8_t)payload_length);
  } else {
    header[header_length++] = (uint8_t)((masked ? 0x80U : 0x00U) | 126U);
    header[header_length++] = (uint8_t)((payload_length >> 8U) & 0xFFU);
    header[header_length++] = (uint8_t)(payload_length & 0xFFU);
  }

  uint8_t mask[4] = {0x11U, 0x22U, 0x33U, 0x44U};
  if (masked) {
    memcpy(header + header_length, mask, sizeof(mask));
    header_length += sizeof(mask);
  }

  if (!test_send_all(fd, header, header_length)) {
    return false;
  }

  if (payload_length == 0U) {
    return true;
  }

  if (!masked) {
    return test_send_all(fd, payload, payload_length);
  }

  auto masked_payload = (uint8_t *)calloc(payload_length, sizeof(uint8_t));
  if (masked_payload == nullptr) {
    return false;
  }

  for (size_t i = 0; i < payload_length; ++i) {
    masked_payload[i] = payload[i] ^ mask[i % 4U];
  }
  auto ok = test_send_all(fd, masked_payload, payload_length);
  free(masked_payload);
  return ok;
}

[[nodiscard]] static bool test_read_frame(int fd, test_frame_t *frame,
                                          bool require_masked) {
  if (frame == nullptr) {
    return false;
  }

  uint8_t header[2] = {0};
  if (!test_read_exact(fd, header, sizeof(header))) {
    return false;
  }

  auto fin = (header[0] & 0x80U) != 0U;
  auto opcode = header[0] & 0x0FU;
  auto masked = (header[1] & 0x80U) != 0U;
  uint64_t payload_length = (uint64_t)(header[1] & 0x7FU);

  if (payload_length == 126U) {
    uint8_t extended[2] = {0};
    if (!test_read_exact(fd, extended, sizeof(extended))) {
      return false;
    }
    payload_length = ((uint64_t)extended[0] << 8U) | (uint64_t)extended[1];
  } else if (payload_length == 127U) {
    return false;
  }

  if (require_masked && !masked) {
    return false;
  }
  if (payload_length > sizeof(frame->payload)) {
    return false;
  }

  uint8_t mask[4] = {0};
  if (masked && !test_read_exact(fd, mask, sizeof(mask))) {
    return false;
  }

  if (!test_read_exact(fd, frame->payload, (size_t)payload_length)) {
    return false;
  }
  if (masked) {
    for (size_t i = 0; i < (size_t)payload_length; ++i) {
      frame->payload[i] ^= mask[i % 4U];
    }
  }

  frame->fin = fin;
  frame->opcode = opcode;
  frame->payload_length = (size_t)payload_length;
  return true;
}

[[nodiscard]] static bool test_expect_close_frame(int fd) {
  test_frame_t frame = {0};
  if (!test_read_frame(fd, &frame, true)) {
    return false;
  }
  return frame.opcode == 0x8U;
}

[[nodiscard]] static bool test_server_start(test_server_t *server,
                                            test_server_mode_t mode) {
  if (server == nullptr) {
    return false;
  }
  server->pid = -1;
  server->port = 0U;

  auto listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (listen_fd < 0) {
    return false;
  }

  int reuse = 1;
  if (setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) !=
      0) {
    (void)close(listen_fd);
    return false;
  }

  struct sockaddr_in address = {
      .sin_family = AF_INET,
      .sin_port = 0U,
      .sin_addr =
          {
              .s_addr = htonl(INADDR_LOOPBACK),
          },
  };

  if (bind(listen_fd, (const struct sockaddr *)&address, sizeof(address)) !=
      0) {
    (void)close(listen_fd);
    return false;
  }
  if (listen(listen_fd, 1) != 0) {
    (void)close(listen_fd);
    return false;
  }

  struct sockaddr_in bound = {0};
  socklen_t bound_length = sizeof(bound);
  if (getsockname(listen_fd, (struct sockaddr *)&bound, &bound_length) != 0) {
    (void)close(listen_fd);
    return false;
  }
  server->port = ntohs(bound.sin_port);

  auto pid = fork();
  if (pid < 0) {
    (void)close(listen_fd);
    return false;
  }

  if (pid == 0) {
    auto success = test_server_child(mode, listen_fd);
    (void)close(listen_fd);
    _exit(success ? 0 : 1);
  }

  server->pid = pid;
  (void)close(listen_fd);
  return true;
}

[[nodiscard]] static bool test_server_wait_success(test_server_t *server) {
  if (server == nullptr || server->pid <= 0) {
    return false;
  }

  int status = 0;
  auto waited = waitpid(server->pid, &status, 0);
  server->pid = -1;
  if (waited < 0) {
    return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static void test_server_abort(test_server_t *server) {
  if (server == nullptr || server->pid <= 0) {
    return;
  }

  (void)kill(server->pid, SIGTERM);
  (void)waitpid(server->pid, nullptr, 0);
  server->pid = -1;
}

[[nodiscard]] static bool test_server_child(test_server_mode_t mode,
                                            int listen_fd) {
  struct pollfd poll_fd = {
      .fd = listen_fd,
      .events = POLLIN,
      .revents = 0,
  };
  if (poll(&poll_fd, 1, 5'000) <= 0) {
    return false;
  }

  auto conn_fd = accept(listen_fd, nullptr, nullptr);
  if (conn_fd < 0) {
    return false;
  }

  struct timeval timeout = {
      .tv_sec = 5,
      .tv_usec = 0,
  };
  (void)setsockopt(conn_fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
  (void)setsockopt(conn_fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));

  auto ok = test_server_handle_mode(mode, conn_fd);
  (void)shutdown(conn_fd, SHUT_RDWR);
  (void)close(conn_fd);
  return ok;
}

[[nodiscard]] static bool test_server_handle_mode(test_server_mode_t mode,
                                                  int conn_fd) {
  if (mode == TEST_SERVER_RAW_ACCEPT_ONLY) {
    uint8_t scratch[256] = {0};
    (void)recv(conn_fd, scratch, sizeof(scratch), 0);
    return true;
  }

  char request[TEST_HTTP_BUFFER_CAPACITY] = {0};
  if (!test_read_http_request(conn_fd, request, sizeof(request))) {
    return false;
  }

  if (mode == TEST_SERVER_BAD_STATUS) {
    constexpr char bad_response[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    return test_send_all(conn_fd, bad_response, sizeof(bad_response) - 1U);
  }
  if (mode == TEST_SERVER_MISSING_ACCEPT) {
    return test_send_response_missing_accept(conn_fd);
  }

  char key[128] = {0};
  if (!test_find_header_value(request, "Sec-WebSocket-Key", key, sizeof(key))) {
    return false;
  }

  char accept_value[128] = {0};
  if (!test_compute_accept_value(key, accept_value, sizeof(accept_value))) {
    return false;
  }
  if (mode == TEST_SERVER_BAD_ACCEPT) {
    (void)snprintf(accept_value, sizeof(accept_value), "%s", "bad-accept");
  }

  if (!test_send_handshake_response(conn_fd, accept_value)) {
    return false;
  }

  if (mode == TEST_SERVER_BAD_ACCEPT) {
    return true;
  }
  if (mode == TEST_SERVER_HANDSHAKE_WAIT_CLOSE) {
    return test_expect_close_frame(conn_fd);
  }
  if (mode == TEST_SERVER_EXPECT_TEXT) {
    test_frame_t frame = {0};
    if (!test_read_frame(conn_fd, &frame, true)) {
      return false;
    }
    if (frame.opcode != 0x1U || frame.payload_length != 5U) {
      return false;
    }
    return memcmp(frame.payload, "hello", 5U) == 0;
  }
  if (mode == TEST_SERVER_EXPECT_BINARY) {
    test_frame_t frame = {0};
    constexpr uint8_t expected_payload[] = {0x00U, 0x10U, 0x7FU, 0xFFU};
    if (!test_read_frame(conn_fd, &frame, true)) {
      return false;
    }
    if (frame.opcode != 0x2U ||
        frame.payload_length != sizeof(expected_payload)) {
      return false;
    }
    return memcmp(frame.payload, expected_payload, sizeof(expected_payload)) ==
           0;
  }
  if (mode == TEST_SERVER_SEND_TEXT) {
    return test_send_frame(conn_fd, 0x1U, true, false,
                           (const uint8_t *)"server-text", 11U);
  }
  if (mode == TEST_SERVER_SEND_BINARY) {
    constexpr uint8_t payload[] = {0xA1U, 0xB2U, 0xC3U};
    return test_send_frame(conn_fd, 0x2U, true, false, payload,
                           sizeof(payload));
  }
  if (mode == TEST_SERVER_PING_THEN_TEXT) {
    if (!test_send_frame(conn_fd, 0x9U, true, false, (const uint8_t *)"xy",
                         2U)) {
      return false;
    }

    test_frame_t frame = {0};
    if (!test_read_frame(conn_fd, &frame, true)) {
      return false;
    }
    if (frame.opcode != 0xAU || frame.payload_length != 2U ||
        memcmp(frame.payload, "xy", 2U) != 0) {
      return false;
    }

    return test_send_frame(conn_fd, 0x1U, true, false,
                           (const uint8_t *)"after-ping", 10U);
  }
  if (mode == TEST_SERVER_SEND_FRAGMENTED_TEXT) {
    if (!test_send_frame(conn_fd, 0x1U, false, false, (const uint8_t *)"frag",
                         4U)) {
      return false;
    }
    return test_send_frame(conn_fd, 0x0U, true, false, (const uint8_t *)"ment",
                           4U);
  }
  if (mode == TEST_SERVER_SEND_MASKED_TEXT) {
    return test_send_frame(conn_fd, 0x1U, true, true, (const uint8_t *)"bad",
                           3U);
  }
  if (mode == TEST_SERVER_SEND_UNEXPECTED_BINARY) {
    constexpr uint8_t payload[] = {0x01U, 0x02U};
    return test_send_frame(conn_fd, 0x2U, true, false, payload,
                           sizeof(payload));
  }

  return false;
}

[[nodiscard]] static bool run_test_last_error_null_client() {
  return strcmp(ws_client_last_error(nullptr),
                "ws_client_t pointer is nullptr") == 0;
}

[[nodiscard]] static bool run_test_create_sets_no_error() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  auto ok = strcmp(ws_client_last_error(client), "no error") == 0;
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_connect_rejects_null_client() {
  return !ws_client_connect(nullptr, "example.com", 80, "/");
}

[[nodiscard]] static bool run_test_connect_rejects_null_host() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  auto ok = !ws_client_connect(client, nullptr, 80, "/") &&
            expect_error_equals(client, "Host and path must be non-null");
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_connect_rejects_null_path() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  auto ok = !ws_client_connect(client, "example.com", 80, nullptr) &&
            expect_error_equals(client, "Host and path must be non-null");
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_connect_rejects_invalid_host() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  auto ok = !ws_client_connect(client, "bad host", 80, "/") &&
            expect_error_equals(client, "Host contains unsupported characters");
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_connect_rejects_invalid_path() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  auto ok = !ws_client_connect(client, "example.com", 80, "chat") &&
            expect_error_equals(
                client, "Path must start with '/' and contain no controls");
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_connect_secure_rejects_invalid_host() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  auto ok = !ws_client_connect_secure(client, "bad host", 443, "/") &&
            expect_error_equals(client, "Host contains unsupported characters");
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_send_text_rejects_null_payload() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  auto ok = !ws_client_send_text(client, nullptr, 1U) &&
            expect_error_equals(client, "Text payload pointer is invalid");
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_send_binary_rejects_null_payload() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  auto ok = !ws_client_send_binary(client, nullptr, 1U) &&
            expect_error_equals(client, "Binary payload pointer is invalid");
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_send_text_fails_when_disconnected() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  constexpr char payload[] = "hello";
  auto ok = !ws_client_send_text(client, payload, sizeof(payload) - 1U);
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_send_binary_fails_when_disconnected() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  constexpr uint8_t payload[] = {0x01U, 0x02U};
  auto ok = !ws_client_send_binary(client, payload, sizeof(payload));
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_receive_text_rejects_invalid_buffer() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  size_t out_length = 0;
  auto ok = !ws_client_receive_text(client, nullptr, 32U, &out_length) &&
            expect_error_equals(client, "Text receive buffer is invalid");
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_receive_binary_rejects_invalid_buffer() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  size_t out_length = 0;
  auto ok = !ws_client_receive_binary(client, nullptr, 32U, &out_length) &&
            expect_error_equals(client, "Binary receive buffer is invalid");
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_receive_text_fails_when_disconnected() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  char buffer[16] = {0};
  size_t out_length = 0;
  auto ok =
      !ws_client_receive_text(client, buffer, sizeof(buffer), &out_length) &&
      expect_error_equals(client, "Client is not connected");
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_receive_binary_fails_when_disconnected() {
  auto client = ws_client_create();
  if (client == nullptr) {
    return false;
  }

  uint8_t buffer[16] = {0};
  size_t out_length = 0;
  auto ok =
      !ws_client_receive_binary(client, buffer, sizeof(buffer), &out_length) &&
      expect_error_equals(client, "Client is not connected");
  ws_client_destroy(client);
  return ok;
}

[[nodiscard]] static bool run_test_close_and_destroy_allow_null() {
  ws_client_close(nullptr);
  ws_client_destroy(nullptr);
  return true;
}

[[nodiscard]] static bool run_test_connect_success_with_mock_server() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_HANDSHAKE_WAIT_CLOSE)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  auto connected = ws_client_connect(client, "127.0.0.1", server.port, "/chat");
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return connected && server_ok;
}

[[nodiscard]] static bool run_test_connect_fails_on_bad_accept() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_BAD_ACCEPT)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  auto ok = !ws_client_connect(client, "127.0.0.1", server.port, "/chat") &&
            expect_error_equals(client, "Invalid Sec-WebSocket-Accept header");
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return ok && server_ok;
}

[[nodiscard]] static bool run_test_connect_fails_on_bad_status() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_BAD_STATUS)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  auto ok = !ws_client_connect(client, "127.0.0.1", server.port, "/chat") &&
            expect_error_equals(client, "Server rejected websocket upgrade");
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return ok && server_ok;
}

[[nodiscard]] static bool run_test_connect_fails_on_missing_accept() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_MISSING_ACCEPT)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  auto ok =
      !ws_client_connect(client, "127.0.0.1", server.port, "/chat") &&
      expect_error_equals(client, "Handshake missing Sec-WebSocket-Accept");
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return ok && server_ok;
}

[[nodiscard]] static bool run_test_connect_secure_fails_against_plain_server() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_RAW_ACCEPT_ONLY)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  auto ok =
      !ws_client_connect_secure(client, "127.0.0.1", server.port, "/chat") &&
      expect_error_contains(client, "Failed to establish TLS");
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return ok && server_ok;
}

[[nodiscard]] static bool run_test_send_text_to_mock_server() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_EXPECT_TEXT)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  auto connected = ws_client_connect(client, "127.0.0.1", server.port, "/chat");
  auto sent = connected && ws_client_send_text(client, "hello", 5U);
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return connected && sent && server_ok;
}

[[nodiscard]] static bool run_test_send_binary_to_mock_server() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_EXPECT_BINARY)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  constexpr uint8_t payload[] = {0x00U, 0x10U, 0x7FU, 0xFFU};
  auto connected = ws_client_connect(client, "127.0.0.1", server.port, "/chat");
  auto sent =
      connected && ws_client_send_binary(client, payload, sizeof(payload));
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return connected && sent && server_ok;
}

[[nodiscard]] static bool run_test_receive_text_from_mock_server() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_SEND_TEXT)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  char buffer[64] = {0};
  size_t length = 0;
  auto connected = ws_client_connect(client, "127.0.0.1", server.port, "/chat");
  auto received =
      connected &&
      ws_client_receive_text(client, buffer, sizeof(buffer), &length) &&
      length == 11U && strcmp(buffer, "server-text") == 0;
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return connected && received && server_ok;
}

[[nodiscard]] static bool run_test_receive_binary_from_mock_server() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_SEND_BINARY)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  uint8_t buffer[64] = {0};
  size_t length = 0;
  constexpr uint8_t expected[] = {0xA1U, 0xB2U, 0xC3U};
  auto connected = ws_client_connect(client, "127.0.0.1", server.port, "/chat");
  auto received =
      connected &&
      ws_client_receive_binary(client, buffer, sizeof(buffer), &length) &&
      length == sizeof(expected) &&
      memcmp(buffer, expected, sizeof(expected)) == 0;
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return connected && received && server_ok;
}

[[nodiscard]] static bool run_test_receive_text_fails_on_small_buffer() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_SEND_TEXT)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  char buffer[8] = {0};
  size_t length = 0;
  auto connected = ws_client_connect(client, "127.0.0.1", server.port, "/chat");
  auto received =
      connected &&
      !ws_client_receive_text(client, buffer, sizeof(buffer), &length) &&
      expect_error_contains(client, "Receive buffer is too small");
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return connected && received && server_ok;
}

[[nodiscard]] static bool run_test_receive_binary_fails_on_small_buffer() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_SEND_BINARY)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  uint8_t buffer[2] = {0};
  size_t length = 0;
  auto connected = ws_client_connect(client, "127.0.0.1", server.port, "/chat");
  auto received =
      connected &&
      !ws_client_receive_binary(client, buffer, sizeof(buffer), &length) &&
      expect_error_contains(client, "Receive buffer is too small");
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return connected && received && server_ok;
}

[[nodiscard]] static bool run_test_receive_text_handles_ping_pong() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_PING_THEN_TEXT)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  char buffer[64] = {0};
  size_t length = 0;
  auto connected = ws_client_connect(client, "127.0.0.1", server.port, "/chat");
  auto received =
      connected &&
      ws_client_receive_text(client, buffer, sizeof(buffer), &length) &&
      length == 10U && strcmp(buffer, "after-ping") == 0;
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return connected && received && server_ok;
}

[[nodiscard]] static bool
run_test_receive_text_reassembles_fragmented_message() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_SEND_FRAGMENTED_TEXT)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  char buffer[64] = {0};
  size_t length = 0;
  auto connected = ws_client_connect(client, "127.0.0.1", server.port, "/chat");
  auto received =
      connected &&
      ws_client_receive_text(client, buffer, sizeof(buffer), &length) &&
      length == 8U && strcmp(buffer, "fragment") == 0;
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return connected && received && server_ok;
}

[[nodiscard]] static bool run_test_receive_text_rejects_masked_server_frame() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_SEND_MASKED_TEXT)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  char buffer[16] = {0};
  size_t length = 0;
  auto connected = ws_client_connect(client, "127.0.0.1", server.port, "/chat");
  auto received =
      connected &&
      !ws_client_receive_text(client, buffer, sizeof(buffer), &length) &&
      expect_error_equals(client, "Server frame must not be masked");
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return connected && received && server_ok;
}

[[nodiscard]] static bool run_test_receive_text_rejects_unexpected_binary() {
  test_server_t server = {0};
  if (!test_server_start(&server, TEST_SERVER_SEND_UNEXPECTED_BINARY)) {
    return false;
  }

  auto client = ws_client_create();
  if (client == nullptr) {
    test_server_abort(&server);
    return false;
  }

  char buffer[16] = {0};
  size_t length = 0;
  auto connected = ws_client_connect(client, "127.0.0.1", server.port, "/chat");
  auto received =
      connected &&
      !ws_client_receive_text(client, buffer, sizeof(buffer), &length) &&
      expect_error_contains(client, "Received unexpected binary frame");
  ws_client_destroy(client);
  auto server_ok = test_server_wait_success(&server);
  return connected && received && server_ok;
}

int main() {
  auto tests_passed = 0;
  auto tests_failed = 0;

  TEST(last_error_null_client);
  TEST(create_sets_no_error);
  TEST(connect_rejects_null_client);
  TEST(connect_rejects_null_host);
  TEST(connect_rejects_null_path);
  TEST(connect_rejects_invalid_host);
  TEST(connect_rejects_invalid_path);
  TEST(connect_secure_rejects_invalid_host);
  TEST(send_text_rejects_null_payload);
  TEST(send_binary_rejects_null_payload);
  TEST(send_text_fails_when_disconnected);
  TEST(send_binary_fails_when_disconnected);
  TEST(receive_text_rejects_invalid_buffer);
  TEST(receive_binary_rejects_invalid_buffer);
  TEST(receive_text_fails_when_disconnected);
  TEST(receive_binary_fails_when_disconnected);
  TEST(close_and_destroy_allow_null);
  TEST(connect_success_with_mock_server);
  TEST(connect_fails_on_bad_accept);
  TEST(connect_fails_on_bad_status);
  TEST(connect_fails_on_missing_accept);
  TEST(connect_secure_fails_against_plain_server);
  TEST(send_text_to_mock_server);
  TEST(send_binary_to_mock_server);
  TEST(receive_text_from_mock_server);
  TEST(receive_binary_from_mock_server);
  TEST(receive_text_fails_on_small_buffer);
  TEST(receive_binary_fails_on_small_buffer);
  TEST(receive_text_handles_ping_pong);
  TEST(receive_text_reassembles_fragmented_message);
  TEST(receive_text_rejects_masked_server_frame);
  TEST(receive_text_rejects_unexpected_binary);

  printf("\nSummary: %d passed, %d failed\n", tests_passed, tests_failed);
  return (tests_failed == 0) ? 0 : 1;
}
