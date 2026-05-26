#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "websocket-server/websocket.h"

constexpr size_t INITIAL_BUFFER_CAP = 4'096;
constexpr size_t FEED_CHUNK_MAX = 4'096U;
constexpr size_t MAX_CONTROL_PAYLOAD = 125U;
constexpr size_t MAX_MESSAGE_PAYLOAD = 1'048'576U; // 1 MiB upper bound
constexpr size_t SHA1_BLOCK_SIZE = 64U;
constexpr size_t SHA1_DIGEST_SIZE = 20U;
constexpr size_t CLOSE_CODE_LEN = 2U;
constexpr size_t CLOSE_FRAME_LEN = 4U;
constexpr size_t FRAME_HEADER_MAX = 10U;
constexpr size_t HANDSHAKE_RESPONSE_MAX = 256U;
constexpr size_t ACCEPT_KEY_MAX = 64U;
constexpr size_t SEC_KEY_MAX = 128U;
constexpr size_t CONCAT_KEY_MAX = 256U;
constexpr size_t MAX_HANDSHAKE_HEADER = 8'192U;
constexpr char WS_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

typedef struct {
  uint8_t *data;
  size_t len;
  size_t cap;
} ws_buffer_t;

typedef struct {
  uint32_t h0;
  uint32_t h1;
  uint32_t h2;
  uint32_t h3;
  uint32_t h4;
  uint64_t length_bits;
  uint8_t block[SHA1_BLOCK_SIZE];
  size_t block_len;
} sha1_ctx_t;

struct ws_conn_s {
  ws_transport_t transport;
  ws_callbacks_t callbacks;
  void *user_context;
  ws_state_t state;
  bool close_sent;
  ws_buffer_t inbound;
};

static void ws_buffer_free(ws_buffer_t *buffer) {
  if (buffer->data != nullptr) {
    free(buffer->data);
    buffer->data = nullptr;
  }
  buffer->len = 0U;
  buffer->cap = 0U;
}

[[nodiscard]]
static bool ws_buffer_reserve(ws_buffer_t *buffer, size_t desired) {
  if (desired <= buffer->cap) {
    return true;
  }

  auto new_cap = buffer->cap == 0U ? INITIAL_BUFFER_CAP : buffer->cap;
  while (new_cap < desired) {
    if (new_cap > SIZE_MAX / 2U) {
      new_cap = desired;
      break;
    }
    new_cap *= 2U;
  }
  if (new_cap < desired) {
    return false;
  }

  auto new_data = (uint8_t *)calloc(new_cap, sizeof(uint8_t));
  if (new_data == nullptr) {
    return false;
  }

  if (buffer->data != nullptr) {
    if (buffer->len > 0U) {
      memcpy(new_data, buffer->data, buffer->len);
    }
    free(buffer->data);
  }

  buffer->data = new_data;
  buffer->cap = new_cap;
  return true;
}

[[nodiscard]]
static bool ws_buffer_append(ws_buffer_t *buffer, const uint8_t *data,
                             size_t len) {
  if (len == 0U) {
    return true;
  }
  if (len > SIZE_MAX - buffer->len) {
    return false;
  }
  const size_t required = buffer->len + len;
  if (!ws_buffer_reserve(buffer, required)) {
    return false;
  }

  memcpy(buffer->data + buffer->len, data, len);
  buffer->len += len;
  return true;
}

static void ws_buffer_consume(ws_buffer_t *buffer, size_t count) {
  if (count == 0U || buffer->len < count) {
    return;
  }
  const size_t remaining = buffer->len - count;
  if (remaining > 0U) {
    memmove(buffer->data, buffer->data + count, remaining);
  }
  buffer->len = remaining;
}

[[nodiscard]]
static uint32_t sha1_rotl(uint32_t value, uint32_t shift) {
  const uint32_t normalized_shift = shift & 31U;
  if (normalized_shift == 0U) {
    return value;
  }
  return (value << normalized_shift) | (value >> (32U - normalized_shift));
}

static void sha1_init(sha1_ctx_t *ctx) {
  ctx->h0 = 0x67452301U;
  ctx->h1 = 0xEFCDAB89U;
  ctx->h2 = 0x98BADCFEU;
  ctx->h3 = 0x10325476U;
  ctx->h4 = 0xC3D2E1F0U;
  ctx->length_bits = 0U;
  ctx->block_len = 0U;
}

static void sha1_process_block(sha1_ctx_t *ctx) {
  uint32_t w[80];
  for (size_t i = 0U; i < 16U; ++i) {
    const size_t idx = i * 4U;
    w[i] = ((uint32_t)ctx->block[idx] << 24U) |
           ((uint32_t)ctx->block[idx + 1U] << 16U) |
           ((uint32_t)ctx->block[idx + 2U] << 8U) |
           (uint32_t)ctx->block[idx + 3U];
  }

  for (size_t i = 16U; i < 80U; ++i) {
    w[i] = sha1_rotl(w[i - 3U] ^ w[i - 8U] ^ w[i - 14U] ^ w[i - 16U], 1U);
  }

  auto a = ctx->h0;
  auto b = ctx->h1;
  auto c = ctx->h2;
  auto d = ctx->h3;
  auto e = ctx->h4;

  for (size_t i = 0U; i < 80U; ++i) {
    uint32_t f;
    uint32_t k;
    if (i < 20U) {
      f = (b & c) | ((~b) & d);
      k = 0x5A827999U;
    } else if (i < 40U) {
      f = b ^ c ^ d;
      k = 0x6ED9EBA1U;
    } else if (i < 60U) {
      f = (b & c) | (b & d) | (c & d);
      k = 0x8F1BBCDCU;
    } else {
      f = b ^ c ^ d;
      k = 0xCA62C1D6U;
    }

    const uint32_t temp = sha1_rotl(a, 5U) + f + e + k + w[i];
    e = d;
    d = c;
    c = sha1_rotl(b, 30U);
    b = a;
    a = temp;
  }

  ctx->h0 += a;
  ctx->h1 += b;
  ctx->h2 += c;
  ctx->h3 += d;
  ctx->h4 += e;
  ctx->block_len = 0U;
}

static void sha1_update(sha1_ctx_t *ctx, const uint8_t *data, size_t len) {
  for (size_t i = 0U; i < len; ++i) {
    ctx->block[ctx->block_len++] = data[i];
    if (ctx->block_len == SHA1_BLOCK_SIZE) {
      sha1_process_block(ctx);
    }
  }
  ctx->length_bits += (uint64_t)len * 8U;
}

static void sha1_final(sha1_ctx_t *ctx, uint8_t out[SHA1_DIGEST_SIZE]) {
  ctx->block[ctx->block_len++] = 0x80U;

  if (ctx->block_len > 56U) {
    while (ctx->block_len < SHA1_BLOCK_SIZE) {
      ctx->block[ctx->block_len++] = 0U;
    }
    sha1_process_block(ctx);
  }

  while (ctx->block_len < 56U) {
    ctx->block[ctx->block_len++] = 0U;
  }

  for (size_t i = 0U; i < 8U; ++i) {
    ctx->block[56U + i] =
        (uint8_t)((ctx->length_bits >> ((7U - i) * 8U)) & 0xFFU);
  }

  sha1_process_block(ctx);

  const uint32_t digest_parts[5] = {ctx->h0, ctx->h1, ctx->h2, ctx->h3,
                                    ctx->h4};
  for (size_t i = 0U; i < 5U; ++i) {
    out[i * 4U] = (uint8_t)((digest_parts[i] >> 24U) & 0xFFU);
    out[i * 4U + 1U] = (uint8_t)((digest_parts[i] >> 16U) & 0xFFU);
    out[i * 4U + 2U] = (uint8_t)((digest_parts[i] >> 8U) & 0xFFU);
    out[i * 4U + 3U] = (uint8_t)(digest_parts[i] & 0xFFU);
  }
}

[[nodiscard]]
static bool base64_encode(const uint8_t *input, size_t len, char *out,
                          size_t out_size) {
  constexpr char table[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  if (len > (SIZE_MAX - 2U) / 3U) {
    if (out_size > 0U) {
      out[0] = '\0';
    }
    return false;
  }
  const size_t encoded_len = ((len + 2U) / 3U) * 4U;
  if (out_size < encoded_len + 1U) {
    if (out_size > 0U) {
      out[0] = '\0';
    }
    return false;
  }

  size_t o = 0U;
  for (size_t i = 0U; i < len; i += 3U) {
    const uint32_t octet_a = input[i];
    const uint32_t octet_b = (i + 1U < len) ? input[i + 1U] : 0U;
    const uint32_t octet_c = (i + 2U < len) ? input[i + 2U] : 0U;

    const uint32_t triple = (octet_a << 16U) | (octet_b << 8U) | octet_c;

    out[o++] = table[(triple >> 18U) & 0x3FU];
    out[o++] = table[(triple >> 12U) & 0x3FU];
    out[o++] = (i + 1U < len) ? table[(triple >> 6U) & 0x3FU] : '=';
    out[o++] = (i + 2U < len) ? table[triple & 0x3FU] : '=';
  }
  out[o] = '\0';
  return true;
}

[[nodiscard]]
static bool contains_token(const char *haystack, const char *needle) {
  const size_t needle_len = strlen(needle);
  const char *p = haystack;
  while (*p != '\0') {
    while (*p == ' ' || *p == '\t' || *p == ',') {
      ++p;
    }
    const char *start = p;
    while (*p != '\0' && *p != ',') {
      ++p;
    }
    const char *end = p;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
      --end;
    }
    const size_t len = (size_t)(end - start);
    if (len == needle_len && strncasecmp(start, needle, needle_len) == 0) {
      return true;
    }
    if (*p == ',') {
      ++p;
    }
  }
  return false;
}

[[nodiscard]]
static bool close_code_is_valid(uint16_t code) {
  if (code < 1000U || code >= 5000U) {
    return false;
  }
  switch (code) {
  case 1004U:
  case 1005U:
  case 1006U:
  case 1015U:
    return false;
  default:
    return true;
  }
}

[[nodiscard]]
static bool is_utf8_continuation(uint8_t byte) {
  return (byte & 0xC0U) == 0x80U;
}

[[nodiscard]]
static bool utf8_is_valid(const uint8_t *data, size_t len) {
  size_t i = 0U;
  while (i < len) {
    const uint8_t lead = data[i++];
    if (lead <= 0x7FU) {
      continue;
    }
    if (lead >= 0xC2U && lead <= 0xDFU) {
      if (i >= len || !is_utf8_continuation(data[i])) {
        return false;
      }
      ++i;
      continue;
    }
    if (lead == 0xE0U) {
      if (i + 1U >= len || data[i] < 0xA0U || data[i] > 0xBFU ||
          !is_utf8_continuation(data[i + 1U])) {
        return false;
      }
      i += 2U;
      continue;
    }
    if ((lead >= 0xE1U && lead <= 0xECU) || (lead >= 0xEEU && lead <= 0xEFU)) {
      if (i + 1U >= len || !is_utf8_continuation(data[i]) ||
          !is_utf8_continuation(data[i + 1U])) {
        return false;
      }
      i += 2U;
      continue;
    }
    if (lead == 0xEDU) {
      if (i + 1U >= len || data[i] < 0x80U || data[i] > 0x9FU ||
          !is_utf8_continuation(data[i + 1U])) {
        return false;
      }
      i += 2U;
      continue;
    }
    if (lead == 0xF0U) {
      if (i + 2U >= len || data[i] < 0x90U || data[i] > 0xBFU ||
          !is_utf8_continuation(data[i + 1U]) ||
          !is_utf8_continuation(data[i + 2U])) {
        return false;
      }
      i += 3U;
      continue;
    }
    if (lead >= 0xF1U && lead <= 0xF3U) {
      if (i + 2U >= len || !is_utf8_continuation(data[i]) ||
          !is_utf8_continuation(data[i + 1U]) ||
          !is_utf8_continuation(data[i + 2U])) {
        return false;
      }
      i += 3U;
      continue;
    }
    if (lead == 0xF4U) {
      if (i + 2U >= len || data[i] < 0x80U || data[i] > 0x8FU ||
          !is_utf8_continuation(data[i + 1U]) ||
          !is_utf8_continuation(data[i + 2U])) {
        return false;
      }
      i += 3U;
      continue;
    }
    return false;
  }
  return true;
}

[[nodiscard]]
static uint16_t close_payload_validation_error(const uint8_t *payload,
                                               size_t payload_len,
                                               uint16_t *code_out) {
  if (payload_len == 0U) {
    if (code_out != nullptr) {
      *code_out = 1000U;
    }
    return 0U;
  }
  if (payload == nullptr || payload_len == 1U) {
    return 1002U;
  }

  const uint16_t code = ((uint16_t)payload[0] << 8U) | payload[1];
  if (!close_code_is_valid(code)) {
    return 1002U;
  }
  if (payload_len > 2U && !utf8_is_valid(payload + 2U, payload_len - 2U)) {
    return 1007U;
  }

  if (code_out != nullptr) {
    *code_out = code;
  }
  return 0U;
}

[[nodiscard]]
static bool request_line_is_valid(const char *line) {
  const char *method_end = strchr(line, ' ');
  if (method_end == nullptr || method_end != line + 3U ||
      strncmp(line, "GET", 3U) != 0) {
    return false;
  }

  const char *target = method_end + 1U;
  if (*target == '\0') {
    return false;
  }

  const char *version_sep = strchr(target, ' ');
  if (version_sep == nullptr || version_sep == target) {
    return false;
  }

  return strcmp(version_sep + 1U, "HTTP/1.1") == 0;
}

[[nodiscard]]
static bool send_handshake_response(ws_conn_t *conn, const char *accept_key) {
  if (conn->transport.send_raw == nullptr) {
    return false;
  }
  constexpr char RESPONSE_FMT[] = "HTTP/1.1 101 Switching Protocols\r\n"
                                  "Upgrade: websocket\r\n"
                                  "Connection: Upgrade\r\n"
                                  "Sec-WebSocket-Accept: %s\r\n"
                                  "\r\n";

  char response[HANDSHAKE_RESPONSE_MAX];
  const int written =
      snprintf(response, sizeof(response), RESPONSE_FMT, accept_key);
  if (written <= 0 || (size_t)written >= sizeof(response)) {
    return false;
  }

  conn->transport.send_raw(&conn->transport, (const uint8_t *)response,
                           (size_t)written);
  return true;
}

static void send_http_error_and_close(ws_conn_t *conn) {
  constexpr char RESPONSE[] =
      "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n";
  if (conn->transport.send_raw != nullptr) {
    conn->transport.send_raw(&conn->transport, (const uint8_t *)RESPONSE,
                             strlen(RESPONSE));
  }
  if (conn->transport.close != nullptr) {
    conn->transport.close(&conn->transport);
  }
}

static void send_close_frame(ws_conn_t *conn, uint16_t code) {
  if (conn->close_sent || conn->transport.send_raw == nullptr) {
    return;
  }

  uint8_t payload[CLOSE_CODE_LEN];
  payload[0] = (uint8_t)((code >> 8U) & 0xFFU);
  payload[1] = (uint8_t)(code & 0xFFU);

  uint8_t frame[CLOSE_FRAME_LEN];
  frame[0] = 0x88U; // FIN + CLOSE
  frame[1] = 0x02U;
  frame[2] = payload[0];
  frame[3] = payload[1];

  conn->transport.send_raw(&conn->transport, frame, sizeof(frame));
  conn->close_sent = true;
}

static void handle_close(ws_conn_t *conn, uint16_t code) {
  send_close_frame(conn, code);
  conn->state = WS_STATE_CLOSING;
  if (conn->transport.close != nullptr) {
    conn->transport.close(&conn->transport);
  }
}

[[nodiscard]]
static bool handle_handshake(ws_conn_t *conn) {
  if (conn->inbound.len < 4U) {
    return false;
  }

  size_t header_end = 0U;
  for (size_t i = 3U; i < conn->inbound.len; ++i) {
    if (conn->inbound.data[i - 3U] == '\r' &&
        conn->inbound.data[i - 2U] == '\n' &&
        conn->inbound.data[i - 1U] == '\r' && conn->inbound.data[i] == '\n') {
      header_end = i + 1U;
      break;
    }
  }

  if (header_end == 0U) {
    if (conn->inbound.len > MAX_HANDSHAKE_HEADER) {
      conn->state = WS_STATE_CLOSING;
      send_http_error_and_close(conn);
    }
    return false;
  }
  if (header_end > MAX_HANDSHAKE_HEADER) {
    conn->state = WS_STATE_CLOSING;
    send_http_error_and_close(conn);
    return false;
  }

  auto request = (char *)calloc(header_end + 1U, sizeof(char));
  if (request == nullptr) {
    conn->state = WS_STATE_CLOSING;
    send_http_error_and_close(conn);
    return false;
  }
  memcpy(request, conn->inbound.data, header_end);

  char *line_end = strstr(request, "\r\n");
  if (line_end == nullptr) {
    free(request);
    conn->state = WS_STATE_CLOSING;
    send_http_error_and_close(conn);
    return false;
  }
  *line_end = '\0';

  if (!request_line_is_valid(request)) {
    free(request);
    conn->state = WS_STATE_CLOSING;
    send_http_error_and_close(conn);
    return false;
  }

  bool has_upgrade = false;
  bool has_connection = false;
  bool version_ok = false;
  char sec_key[SEC_KEY_MAX] = {0};

  char *line = line_end + 2U;
  while (*line != '\0') {
    char *next_line_end = strstr(line, "\r\n");
    if (next_line_end == nullptr) {
      free(request);
      conn->state = WS_STATE_CLOSING;
      send_http_error_and_close(conn);
      return false;
    }
    *next_line_end = '\0';
    if (*line == '\0') {
      break;
    }

    char *colon = strchr(line, ':');
    if (colon == nullptr || colon == line) {
      free(request);
      conn->state = WS_STATE_CLOSING;
      send_http_error_and_close(conn);
      return false;
    }
    *colon = '\0';
    char *value = colon + 1;
    while (*value == ' ' || *value == '\t') {
      ++value;
    }
    size_t value_len = strlen(value);
    while (value_len > 0U &&
           (value[value_len - 1U] == ' ' || value[value_len - 1U] == '\t')) {
      value[--value_len] = '\0';
    }

    if (strcasecmp(line, "Upgrade") == 0) {
      has_upgrade = has_upgrade || contains_token(value, "websocket");
    } else if (strcasecmp(line, "Connection") == 0) {
      has_connection = has_connection || contains_token(value, "Upgrade");
    } else if (strcasecmp(line, "Sec-WebSocket-Version") == 0) {
      version_ok = version_ok || strcmp(value, "13") == 0;
    } else if (strcasecmp(line, "Sec-WebSocket-Key") == 0) {
      if (value_len < sizeof(sec_key)) {
        memcpy(sec_key, value, value_len + 1U);
      }
    }
    line = next_line_end + 2U;
  }

  free(request);

  if (!has_upgrade || !has_connection || !version_ok || sec_key[0] == '\0') {
    conn->state = WS_STATE_CLOSING;
    send_http_error_and_close(conn);
    return false;
  }

  char concatenated[CONCAT_KEY_MAX];
  const int concat_written =
      snprintf(concatenated, sizeof(concatenated), "%s%s", sec_key, WS_GUID);
  if (concat_written <= 0 || (size_t)concat_written >= sizeof(concatenated)) {
    conn->state = WS_STATE_CLOSING;
    send_http_error_and_close(conn);
    return false;
  }

  uint8_t digest[SHA1_DIGEST_SIZE];
  sha1_ctx_t sha_ctx;
  sha1_init(&sha_ctx);
  sha1_update(&sha_ctx, (const uint8_t *)concatenated, (size_t)concat_written);
  sha1_final(&sha_ctx, digest);

  char accept_key[ACCEPT_KEY_MAX];
  const bool encoded =
      base64_encode(digest, SHA1_DIGEST_SIZE, accept_key, sizeof(accept_key));
  if (!encoded || accept_key[0] == '\0' ||
      !send_handshake_response(conn, accept_key)) {
    conn->state = WS_STATE_CLOSING;
    send_http_error_and_close(conn);
    return false;
  }

  ws_buffer_consume(&conn->inbound, header_end);
  conn->state = WS_STATE_OPEN;
  if (conn->callbacks.on_open != nullptr) {
    conn->callbacks.on_open(conn);
  }
  return true;
}

static void handle_ping(ws_conn_t *conn, const uint8_t *payload,
                        size_t payload_len) {
  if (conn->transport.send_raw == nullptr || conn->state != WS_STATE_OPEN) {
    return;
  }

  const size_t header_len = 2U;
  uint8_t header[2];
  header[0] = 0x80U | WS_OP_PONG;
  header[1] = (uint8_t)payload_len;

  if (payload_len > SIZE_MAX - header_len) {
    handle_close(conn, 1011U);
    return;
  }
  const size_t frame_len = header_len + payload_len;
  auto frame = (uint8_t *)calloc(frame_len, sizeof(uint8_t));
  if (frame == nullptr) {
    handle_close(conn, 1011U);
    return;
  }
  memcpy(frame, header, header_len);
  if (payload_len > 0U) {
    memcpy(frame + header_len, payload, payload_len);
  }

  conn->transport.send_raw(&conn->transport, frame, frame_len);
  free(frame);
}

static void process_frames(ws_conn_t *conn) {
  while (conn->state == WS_STATE_OPEN) {
    if (conn->inbound.len < 2U) {
      return;
    }

    const uint8_t b0 = conn->inbound.data[0];
    const uint8_t b1 = conn->inbound.data[1];

    const bool fin = (b0 & 0x80U) != 0U;
    const uint8_t opcode = (uint8_t)(b0 & 0x0FU);
    const bool masked = (b1 & 0x80U) != 0U;
    uint64_t payload_len = (uint64_t)(b1 & 0x7FU);

    if ((b0 & 0x70U) != 0U) { // RSV bits set
      handle_close(conn, 1002U);
      return;
    }
    if (!masked) {
      handle_close(conn, 1002U);
      return;
    }
    if (opcode > WS_OP_PONG ||
        (opcode > WS_OP_BINARY && opcode < WS_OP_CLOSE)) {
      handle_close(conn, 1002U);
      return;
    }

    const bool is_control = opcode >= WS_OP_CLOSE;
    if (opcode == WS_OP_CONTINUATION ||
        (!fin && (opcode == WS_OP_TEXT || opcode == WS_OP_BINARY)) ||
        (is_control && !fin)) {
      handle_close(conn, 1002U);
      return;
    }

    size_t header_len = 2U;
    if (payload_len == 126U) {
      if (conn->inbound.len < 4U) {
        return;
      }
      payload_len =
          ((uint64_t)conn->inbound.data[2] << 8U) | conn->inbound.data[3];
      if (payload_len < 126U) {
        handle_close(conn, 1002U);
        return;
      }
      header_len += 2U;
    } else if (payload_len == 127U) {
      if (conn->inbound.len < 10U) {
        return;
      }
      if ((conn->inbound.data[2] & 0x80U) != 0U) {
        handle_close(conn, 1002U);
        return;
      }
      payload_len = 0U;
      for (size_t i = 0U; i < 8U; ++i) {
        payload_len = (payload_len << 8U) | conn->inbound.data[2U + i];
      }
      if (payload_len <= 65'535U) {
        handle_close(conn, 1002U);
        return;
      }
      header_len += 8U;
    }

    header_len += 4U; // mask key

    if (conn->inbound.len < header_len) {
      return;
    }

    if (payload_len > MAX_MESSAGE_PAYLOAD || payload_len > SIZE_MAX) {
      handle_close(conn, 1009U);
      return;
    }
    if (is_control && payload_len > MAX_CONTROL_PAYLOAD) {
      handle_close(conn, 1002U);
      return;
    }
    if (opcode == WS_OP_CLOSE && payload_len == 1U) {
      handle_close(conn, 1002U);
      return;
    }

    const size_t total_frame = header_len + (size_t)payload_len;
    if (total_frame < header_len) {
      handle_close(conn, 1009U);
      return;
    }
    if (conn->inbound.len < total_frame) {
      return;
    }

    const uint8_t *mask_key = conn->inbound.data + (header_len - 4U);
    const uint8_t *payload = conn->inbound.data + header_len;

    const size_t alloc_size = payload_len == 0U ? 1U : (size_t)payload_len;
    auto decoded = (uint8_t *)calloc(alloc_size, sizeof(uint8_t));
    if (decoded == nullptr) {
      handle_close(conn, 1011U);
      return;
    }

    for (size_t i = 0U; i < payload_len; ++i) {
      decoded[i] = (uint8_t)(payload[i] ^ mask_key[i % 4U]);
    }

    switch (opcode) {
    case WS_OP_TEXT:
      if (!utf8_is_valid(decoded, (size_t)payload_len)) {
        free(decoded);
        handle_close(conn, 1007U);
        return;
      }
      [[fallthrough]];
    case WS_OP_BINARY:
      if (conn->callbacks.on_message != nullptr) {
        conn->callbacks.on_message(conn, decoded, (size_t)payload_len,
                                   (ws_opcode_t)opcode);
      }
      break;
    case WS_OP_CLOSE: {
      uint16_t code = 1000U;
      const uint16_t validation_error =
          close_payload_validation_error(decoded, (size_t)payload_len, &code);
      if (validation_error != 0U) {
        free(decoded);
        handle_close(conn, validation_error);
        return;
      }
      free(decoded);
      handle_close(conn, code);
      return;
    }
    case WS_OP_PING:
      handle_ping(conn, decoded, (size_t)payload_len);
      break;
    case WS_OP_PONG:
      break;
    default:
      free(decoded);
      handle_close(conn, 1003U);
      return;
    }

    free(decoded);
    ws_buffer_consume(&conn->inbound, total_frame);
  }
}

[[nodiscard]]
ws_conn_t *ws_conn_new(ws_transport_t transport, ws_callbacks_t callbacks,
                       void *external_context) {
  auto conn = (ws_conn_t *)calloc(1, sizeof(ws_conn_t));
  if (conn == nullptr) {
    return nullptr;
  }

  conn->transport = transport;
  conn->callbacks = callbacks;
  conn->user_context = external_context;
  conn->state = WS_STATE_HANDSHAKE;
  conn->close_sent = false;
  conn->inbound.data = nullptr;
  conn->inbound.cap = 0U;
  conn->inbound.len = 0U;

  return conn;
}

void ws_conn_free(ws_conn_t *conn) {
  if (conn == nullptr) {
    return;
  }

  if (conn->callbacks.on_close != nullptr && conn->state != WS_STATE_CLOSED) {
    conn->callbacks.on_close(conn);
  }

  ws_buffer_free(&conn->inbound);
  conn->state = WS_STATE_CLOSED;
  free(conn);
}

void ws_conn_feed(ws_conn_t *conn, const uint8_t *data, size_t len) {
  if (conn == nullptr || data == nullptr || len == 0U ||
      conn->state == WS_STATE_CLOSED || conn->state == WS_STATE_CLOSING) {
    return;
  }

  size_t offset = 0U;
  while (offset < len) {
    if (conn->state == WS_STATE_CLOSING || conn->state == WS_STATE_CLOSED) {
      return;
    }

    size_t chunk_len = len - offset;
    if (chunk_len > FEED_CHUNK_MAX) {
      chunk_len = FEED_CHUNK_MAX;
    }

    if (conn->state == WS_STATE_HANDSHAKE) {
      if (conn->inbound.len >= MAX_HANDSHAKE_HEADER) {
        conn->state = WS_STATE_CLOSING;
        send_http_error_and_close(conn);
        return;
      }

      const size_t handshake_room = MAX_HANDSHAKE_HEADER - conn->inbound.len;
      if (chunk_len > handshake_room) {
        chunk_len = handshake_room;
      }
    }

    if (!ws_buffer_append(&conn->inbound, data + offset, chunk_len)) {
      if (conn->state == WS_STATE_HANDSHAKE) {
        conn->state = WS_STATE_CLOSING;
        if (conn->transport.close != nullptr) {
          conn->transport.close(&conn->transport);
        }
      } else {
        handle_close(conn, 1011U);
      }
      return;
    }
    offset += chunk_len;

    if (conn->state == WS_STATE_HANDSHAKE && !handle_handshake(conn)) {
      continue;
    }
    if (conn->state == WS_STATE_OPEN) {
      process_frames(conn);
    }
  }
}

void ws_conn_send(ws_conn_t *conn, const uint8_t *data, size_t len,
                  ws_opcode_t opcode) {
  if (conn == nullptr || conn->transport.send_raw == nullptr ||
      conn->state != WS_STATE_OPEN) {
    return;
  }
  if (len > 0U && data == nullptr) {
    handle_close(conn, 1002U);
    return;
  }
  if ((uint8_t)opcode > WS_OP_PONG ||
      ((uint8_t)opcode > WS_OP_BINARY && (uint8_t)opcode < WS_OP_CLOSE) ||
      opcode == WS_OP_CONTINUATION) {
    handle_close(conn, 1002U);
    return;
  }
  const bool is_control = (uint8_t)opcode >= WS_OP_CLOSE;
  if (is_control &&
      (len > MAX_CONTROL_PAYLOAD || (opcode == WS_OP_CLOSE && len == 1U))) {
    handle_close(conn, 1002U);
    return;
  }
  if (!is_control && len > MAX_MESSAGE_PAYLOAD) {
    handle_close(conn, 1009U);
    return;
  }
  if (opcode == WS_OP_CLOSE) {
    uint16_t code = 1000U;
    const uint16_t validation_error =
        close_payload_validation_error(data, len, &code);
    if (validation_error != 0U) {
      handle_close(conn, validation_error);
      return;
    }
  }
  if (len > SIZE_MAX - FRAME_HEADER_MAX) {
    handle_close(conn, 1009U);
    return;
  }
  const size_t frame_size = FRAME_HEADER_MAX + len;
  auto frame = (uint8_t *)calloc(frame_size, sizeof(uint8_t));
  if (frame == nullptr) {
    handle_close(conn, 1011U);
    return;
  }

  size_t offset = 0U;
  frame[offset++] = (uint8_t)(0x80U | (uint8_t)opcode);

  if (len <= 125U) {
    frame[offset++] = (uint8_t)len;
  } else if (len <= 65'535U) {
    frame[offset++] = 126U;
    frame[offset++] = (uint8_t)((len >> 8U) & 0xFFU);
    frame[offset++] = (uint8_t)(len & 0xFFU);
  } else {
    frame[offset++] = 127U;
    const uint64_t len64 = (uint64_t)len;
    for (size_t i = 0U; i < 8U; ++i) {
      const uint32_t shift = (uint32_t)((7U - i) * 8U);
      frame[offset++] = (uint8_t)((len64 >> shift) & 0xFFU);
    }
  }

  if (len > 0U) {
    memcpy(frame + offset, data, len);
  }

  conn->transport.send_raw(&conn->transport, frame, offset + len);
  free(frame);
}

[[nodiscard]]
void *ws_conn_get_context(ws_conn_t *conn) {
  if (conn == nullptr) {
    return nullptr;
  }
  return conn->user_context;
}
