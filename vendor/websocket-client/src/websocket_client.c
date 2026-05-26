#include "websocket_client/websocket_client.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <wolfssl/options.h>
#include <wolfssl/ssl.h>
#include <wolfssl/wolfcrypt/ecc.h>

constexpr size_t WS_ERROR_MESSAGE_CAPACITY = 256;
constexpr size_t WS_HTTP_RESPONSE_CAPACITY = 8 * 1024;
constexpr size_t WS_MAX_HEADER_VALUE = 256;
constexpr size_t WS_MAX_HOST_TEXT_LENGTH = 255;
constexpr uint64_t WS_MAX_INBOUND_MESSAGE_BYTES = 16'777'216ULL;
constexpr uint16_t WS_HANDSHAKE_VERSION = 13;
constexpr uint32_t WS_READ_TIMEOUT_SECONDS = 5;
constexpr uint32_t WS_WRITE_TIMEOUT_SECONDS = 5;
constexpr uint32_t WS_HTTP_HEADER_TERMINATOR = 0x0D0A0D0AU;

constexpr char WS_ACCEPT_GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr char WS_BASE64_TABLE[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

struct ws_client {
  int socket_fd;
  bool connected;
  bool use_tls;
  WOLFSSL_CTX *ssl_ctx;
  WOLFSSL *ssl;
  char last_error[WS_ERROR_MESSAGE_CAPACITY];
};

typedef struct {
  uint32_t state[5];
  uint64_t total_bits;
  uint8_t block[64];
  size_t block_length;
} sha1_ctx_t;

static void ws_drop_connection(ws_client_t *client);

[[nodiscard]] static uint32_t ws_rotl32(uint32_t value, unsigned shift) {
  return (value << shift) | (value >> (32U - shift));
}

[[nodiscard]] static bool ws_checked_add_size(size_t *out, size_t left,
                                              size_t right) {
  return !ckd_add(out, left, right);
}

[[nodiscard]] static bool ws_checked_sub_size(size_t *out, size_t left,
                                              size_t right) {
  return !ckd_sub(out, left, right);
}

[[nodiscard]] static bool ws_checked_mul_size(size_t *out, size_t left,
                                              size_t right) {
  return !ckd_mul(out, left, right);
}

static void ws_secure_wipe(void *buffer, size_t length) {
  if (buffer == nullptr || length == 0) {
    return;
  }

  volatile uint8_t *volatile bytes = (volatile uint8_t *volatile)buffer;
  for (size_t i = 0; i < length; ++i) {
    bytes[i] = 0;
  }
}

[[nodiscard]] static bool ws_validate_host_text(const char *host) {
  if (host == nullptr || host[0] == '\0') {
    return false;
  }

  auto length = strlen(host);
  if (length > WS_MAX_HOST_TEXT_LENGTH) {
    return false;
  }

  for (size_t i = 0; i < length; ++i) {
    auto octet = (unsigned char)host[i];
    if (octet <= 0x20U || octet == 0x7FU) {
      return false;
    }
    if (octet == '/' || octet == '\\' || octet == '?' || octet == '#') {
      return false;
    }
  }

  return true;
}

[[nodiscard]] static bool ws_validate_request_path(const char *path) {
  if (path == nullptr || path[0] != '/') {
    return false;
  }

  for (size_t i = 0; path[i] != '\0'; ++i) {
    auto octet = (unsigned char)path[i];
    if (octet <= 0x20U || octet == 0x7FU) {
      return false;
    }
  }

  return true;
}

[[nodiscard]] static bool
ws_is_switching_protocols_response(const char *response) {
  if (response == nullptr) {
    return false;
  }

  const char *line_end = strstr(response, "\r\n");
  if (line_end == nullptr) {
    return false;
  }

  auto line_length = (size_t)(line_end - response);
  if (line_length < 12U) {
    return false;
  }

  if (strncmp(response, "HTTP/1.", 7U) != 0) {
    return false;
  }
  if (response[7] != '0' && response[7] != '1') {
    return false;
  }
  if (response[8] != ' ') {
    return false;
  }
  if (response[9] != '1' || response[10] != '0' || response[11] != '1') {
    return false;
  }
  if (line_length > 12U && response[12] != ' ') {
    return false;
  }

  return true;
}

static void ws_sha1_init(sha1_ctx_t *ctx) {
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

static void ws_sha1_transform(sha1_ctx_t *ctx, const uint8_t *block) {
  if (ctx == nullptr || block == nullptr) {
    return;
  }

  uint32_t w[80] = {0};
  for (size_t i = 0; i < 16; ++i) {
    auto offset = i * 4;
    w[i] = ((uint32_t)block[offset] << 24U) |
           ((uint32_t)block[offset + 1] << 16U) |
           ((uint32_t)block[offset + 2] << 8U) | (uint32_t)block[offset + 3];
  }

  for (size_t i = 16; i < 80; ++i) {
    w[i] = ws_rotl32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1U);
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

    auto temp = ws_rotl32(a, 5U) + f + e + k + w[i];
    e = d;
    d = c;
    c = ws_rotl32(b, 30U);
    b = a;
    a = temp;
  }

  ctx->state[0] += a;
  ctx->state[1] += b;
  ctx->state[2] += c;
  ctx->state[3] += d;
  ctx->state[4] += e;
}

static void ws_sha1_update(sha1_ctx_t *ctx, const uint8_t *data,
                           size_t length) {
  if (ctx == nullptr) {
    return;
  }
  if (data == nullptr && length != 0U) {
    return;
  }
  if (length == 0) {
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
      ws_sha1_transform(ctx, ctx->block);
      ctx->block_length = 0;
    }
  }
}

static void ws_sha1_final(sha1_ctx_t *ctx, uint8_t digest[20]) {
  if (ctx == nullptr || digest == nullptr) {
    return;
  }

  ctx->block[ctx->block_length++] = 0x80U;

  if (ctx->block_length > 56) {
    while (ctx->block_length < 64) {
      ctx->block[ctx->block_length++] = 0;
    }
    ws_sha1_transform(ctx, ctx->block);
    ctx->block_length = 0;
  }

  while (ctx->block_length < 56) {
    ctx->block[ctx->block_length++] = 0;
  }

  for (size_t i = 0; i < 8; ++i) {
    auto shift = (7U - i) * 8U;
    ctx->block[56 + i] = (uint8_t)((ctx->total_bits >> shift) & 0xFFU);
  }

  ws_sha1_transform(ctx, ctx->block);

  for (size_t i = 0; i < 5; ++i) {
    digest[i * 4] = (uint8_t)((ctx->state[i] >> 24U) & 0xFFU);
    digest[i * 4 + 1] = (uint8_t)((ctx->state[i] >> 16U) & 0xFFU);
    digest[i * 4 + 2] = (uint8_t)((ctx->state[i] >> 8U) & 0xFFU);
    digest[i * 4 + 3] = (uint8_t)(ctx->state[i] & 0xFFU);
  }
}

[[nodiscard]] static size_t ws_base64_encode(const uint8_t *input,
                                             size_t input_length, char *output,
                                             size_t output_capacity) {
  if (output == nullptr || output_capacity == 0U) {
    return 0;
  }
  if (input == nullptr && input_length != 0U) {
    return 0;
  }

  size_t padded_input_length = 0;
  if (!ws_checked_add_size(&padded_input_length, input_length, 2U)) {
    return 0;
  }

  size_t encoded_length = 0;
  if (!ws_checked_mul_size(&encoded_length, padded_input_length / 3U, 4U)) {
    return 0;
  }

  size_t required = 0;
  if (!ws_checked_add_size(&required, encoded_length, 1U)) {
    return 0;
  }

  if (output_capacity < required) {
    return 0;
  }

  size_t src = 0;
  size_t dst = 0;

  while (src + 2U < input_length) {
    auto chunk = ((uint32_t)input[src] << 16U) |
                 ((uint32_t)input[src + 1] << 8U) | (uint32_t)input[src + 2];

    output[dst++] = WS_BASE64_TABLE[(chunk >> 18U) & 0x3FU];
    output[dst++] = WS_BASE64_TABLE[(chunk >> 12U) & 0x3FU];
    output[dst++] = WS_BASE64_TABLE[(chunk >> 6U) & 0x3FU];
    output[dst++] = WS_BASE64_TABLE[chunk & 0x3FU];
    src += 3;
  }

  if (src < input_length) {
    auto chunk = (uint32_t)input[src] << 16U;
    if (src + 1U < input_length) {
      chunk |= (uint32_t)input[src + 1] << 8U;
    }

    output[dst++] = WS_BASE64_TABLE[(chunk >> 18U) & 0x3FU];
    output[dst++] = WS_BASE64_TABLE[(chunk >> 12U) & 0x3FU];

    if (src + 1U < input_length) {
      output[dst++] = WS_BASE64_TABLE[(chunk >> 6U) & 0x3FU];
    } else {
      output[dst++] = '=';
    }

    output[dst++] = '=';
  }

  output[dst] = '\0';
  return dst;
}

static void ws_set_error(ws_client_t *client, const char *format, ...) {
  if (client == nullptr) {
    return;
  }

  va_list args;
  va_start(args, format);
  (void)vsnprintf(client->last_error, sizeof(client->last_error), format, args);
  va_end(args);
}

[[nodiscard]] static ssize_t ws_transport_recv(int fd, WOLFSSL *ssl,
                                               bool use_tls, uint8_t *buffer,
                                               size_t length) {
  if (buffer == nullptr || length == 0U) {
    errno = EINVAL;
    return -1;
  }

  if (use_tls) {
    if (ssl == nullptr) {
      errno = EINVAL;
      return -1;
    }
    if (length > (size_t)INT_MAX) {
      errno = EINVAL;
      return -1;
    }

    while (true) {
      auto received = wolfSSL_read(ssl, buffer, (int)length);
      if (received > 0) {
        return received;
      }

      auto error = wolfSSL_get_error(ssl, received);
      if (error == WOLFSSL_ERROR_WANT_READ ||
          error == WOLFSSL_ERROR_WANT_WRITE) {
        continue;
      }

      if (error == WOLFSSL_ERROR_SYSCALL && errno == EINTR) {
        continue;
      }

      errno = EIO;
      return -1;
    }
  }

  while (true) {
    auto received = recv(fd, buffer, length, 0);
    if (received < 0 && errno == EINTR) {
      continue;
    }
    return received;
  }
}

[[nodiscard]] static ssize_t ws_transport_send(int fd, WOLFSSL *ssl,
                                               bool use_tls,
                                               const uint8_t *buffer,
                                               size_t length) {
  if (buffer == nullptr || length == 0U) {
    errno = EINVAL;
    return -1;
  }

  if (use_tls) {
    if (ssl == nullptr) {
      errno = EINVAL;
      return -1;
    }
    if (length > (size_t)INT_MAX) {
      errno = EINVAL;
      return -1;
    }

    while (true) {
      auto sent = wolfSSL_write(ssl, buffer, (int)length);
      if (sent > 0) {
        return sent;
      }

      auto error = wolfSSL_get_error(ssl, sent);
      if (error == WOLFSSL_ERROR_WANT_READ ||
          error == WOLFSSL_ERROR_WANT_WRITE) {
        continue;
      }
      if (error == WOLFSSL_ERROR_SYSCALL && errno == EINTR) {
        continue;
      }

      errno = EIO;
      return -1;
    }
  }

  int flags = 0;
#ifdef MSG_NOSIGNAL
  flags = MSG_NOSIGNAL;
#endif

  while (true) {
    auto sent = send(fd, buffer, length, flags);
    if (sent < 0 && errno == EINTR) {
      continue;
    }
    return sent;
  }
}

[[nodiscard]] static bool ws_read_exact(int fd, WOLFSSL *ssl, bool use_tls,
                                        uint8_t *buffer, size_t length) {
  size_t total = 0;
  while (total < length) {
    auto received =
        ws_transport_recv(fd, ssl, use_tls, buffer + total, length - total);
    if (received <= 0) {
      return false;
    }
    total += (size_t)received;
  }
  return true;
}

[[nodiscard]] static bool ws_send_all(int fd, WOLFSSL *ssl, bool use_tls,
                                      const uint8_t *buffer, size_t length) {
  size_t total = 0;
  while (total < length) {
    auto sent =
        ws_transport_send(fd, ssl, use_tls, buffer + total, length - total);
    if (sent <= 0) {
      return false;
    }
    total += (size_t)sent;
  }
  return true;
}

[[nodiscard]] static bool ws_read_http_headers(int fd, WOLFSSL *ssl,
                                               bool use_tls, char *response,
                                               size_t response_capacity) {
  if (response == nullptr || response_capacity < 2) {
    return false;
  }

  response[0] = '\0';
  size_t response_length = 0;
  uint32_t marker = 0;

  while (response_length + 1 < response_capacity) {
    uint8_t byte = 0;
    auto received = ws_transport_recv(fd, ssl, use_tls, &byte, 1);
    if (received <= 0) {
      return false;
    }
    if (byte == '\0') {
      return false;
    }

    response[response_length++] = (char)byte;
    response[response_length] = '\0';
    marker = (marker << 8U) | (uint32_t)byte;

    if (marker == WS_HTTP_HEADER_TERMINATOR) {
      return true;
    }
  }

  return false;
}

[[nodiscard]] static bool ws_discard_bytes(int fd, WOLFSSL *ssl, bool use_tls,
                                           uint64_t length) {
  uint8_t scratch[512] = {0};
  auto remaining = length;

  while (remaining > 0) {
    auto chunk =
        (remaining > sizeof(scratch)) ? sizeof(scratch) : (size_t)remaining;
    if (!ws_read_exact(fd, ssl, use_tls, scratch, chunk)) {
      return false;
    }
    remaining -= chunk;
  }

  return true;
}

[[nodiscard]] static bool ws_get_random_bytes(uint8_t *buffer, size_t length) {
  if (length == 0) {
    return true;
  }
  if (buffer == nullptr) {
    return false;
  }

  int flags = O_RDONLY;
#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
  auto fd = open("/dev/urandom", flags);
  if (fd < 0) {
    return false;
  }

  size_t read_total = 0;
  while (read_total < length) {
    auto bytes = read(fd, buffer + read_total, length - read_total);
    if (bytes < 0 && errno == EINTR) {
      continue;
    }
    if (bytes <= 0) {
      (void)close(fd);
      return false;
    }
    read_total += (size_t)bytes;
  }

  if (close(fd) != 0 && errno != EINTR) {
    return false;
  }

  return read_total == length;
}

[[nodiscard]] static bool ws_connect_tcp(const char *host, uint16_t port,
                                         int *out_fd, char *error_message,
                                         size_t error_capacity) {
  if (out_fd != nullptr) {
    *out_fd = -1;
  }

  if (error_message != nullptr && error_capacity > 0) {
    error_message[0] = '\0';
  }
  if (host == nullptr || host[0] == '\0' || out_fd == nullptr) {
    if (error_message != nullptr && error_capacity > 0) {
      (void)snprintf(error_message, error_capacity,
                     "invalid connection parameters");
    }
    return false;
  }

  char port_text[6] = {0};
  (void)snprintf(port_text, sizeof(port_text), "%u", (unsigned)port);

  struct addrinfo hints = {0};
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo *result = nullptr;
  auto status = getaddrinfo(host, port_text, &hints, &result);
  if (status != 0) {
    if (error_message != nullptr && error_capacity > 0) {
      (void)snprintf(error_message, error_capacity, "getaddrinfo: %s",
                     gai_strerror(status));
    }
    return false;
  }

  auto connected_fd = -1;
  auto last_errno = 0;
  auto attempted_connect = false;
  for (auto current = result; current != nullptr; current = current->ai_next) {
    auto fd =
        socket(current->ai_family, current->ai_socktype, current->ai_protocol);
    if (fd < 0) {
      last_errno = errno;
      continue;
    }

    struct timeval timeout = {
        .tv_sec = WS_READ_TIMEOUT_SECONDS,
        .tv_usec = 0,
    };

    if (setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout)) !=
        0) {
      last_errno = errno;
      (void)close(fd);
      continue;
    }
    timeout.tv_sec = WS_WRITE_TIMEOUT_SECONDS;
    if (setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout)) !=
        0) {
      last_errno = errno;
      (void)close(fd);
      continue;
    }

    attempted_connect = true;
    if (connect(fd, current->ai_addr, current->ai_addrlen) == 0) {
      connected_fd = fd;
      break;
    }

    last_errno = errno;
    (void)close(fd);
  }

  freeaddrinfo(result);

  if (connected_fd < 0) {
    if (error_message != nullptr && error_capacity > 0) {
      if (attempted_connect && last_errno != 0) {
        (void)snprintf(error_message, error_capacity, "connect: %s",
                       strerror(last_errno));
      } else if (last_errno != 0) {
        (void)snprintf(error_message, error_capacity, "socket: %s",
                       strerror(last_errno));
      } else {
        (void)snprintf(error_message, error_capacity,
                       "no address candidates returned");
      }
    }
    return false;
  }

  *out_fd = connected_fd;
  return true;
}

[[nodiscard]] static bool ws_init_tls_ctx(ws_client_t *client) {
  if (client->ssl_ctx != nullptr) {
    return true;
  }

  if (wolfSSL_Init() != WOLFSSL_SUCCESS) {
    return false;
  }

  auto ctx = wolfSSL_CTX_new(wolfTLS_client_method());
  if (ctx == nullptr) {
    return false;
  }

  wolfSSL_CTX_set_verify(ctx, WOLFSSL_VERIFY_PEER, nullptr);
  if (wolfSSL_CTX_set_default_verify_paths(ctx) != WOLFSSL_SUCCESS) {
    wolfSSL_CTX_free(ctx);
    return false;
  }

  client->ssl_ctx = ctx;
  return true;
}

[[nodiscard]] static bool ws_connect_tls(const char *host, int socket_fd,
                                         WOLFSSL_CTX *ctx, WOLFSSL **out_ssl) {
  if (host == nullptr || host[0] == '\0' || ctx == nullptr ||
      out_ssl == nullptr) {
    return false;
  }
  *out_ssl = nullptr;

  auto ssl = wolfSSL_new(ctx);
  if (ssl == nullptr) {
    return false;
  }

  auto host_length = strlen(host);
  if (host_length > UINT16_MAX) {
    wolfSSL_free(ssl);
    return false;
  }

  if (wolfSSL_UseSNI(ssl, WOLFSSL_SNI_HOST_NAME, host,
                     (unsigned short)host_length) != WOLFSSL_SUCCESS) {
    wolfSSL_free(ssl);
    return false;
  }

  if (wolfSSL_check_domain_name(ssl, host) != WOLFSSL_SUCCESS) {
    wolfSSL_free(ssl);
    return false;
  }

  if (wolfSSL_set_fd(ssl, socket_fd) != WOLFSSL_SUCCESS ||
      wolfSSL_connect(ssl) != WOLFSSL_SUCCESS) {
    /* Free retained ECC fixed-point tables after certificate verification. */
    wc_ecc_fp_free();
    wolfSSL_free(ssl);
    return false;
  }

  /* Free retained ECC fixed-point tables after certificate verification. */
  wc_ecc_fp_free();
  *out_ssl = ssl;
  return true;
}

[[nodiscard]] static bool ws_find_header_value(const char *response,
                                               const char *header_name,
                                               char *value,
                                               size_t value_capacity) {
  if (response == nullptr || header_name == nullptr || header_name[0] == '\0' ||
      value == nullptr || value_capacity == 0U) {
    return false;
  }
  value[0] = '\0';

  auto header_name_length = strlen(header_name);
  auto line = strstr(response, "\r\n");
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
      auto name_length = (size_t)(colon - line);
      if (name_length == header_name_length &&
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
        size_t required_capacity = 0;
        if (!ws_checked_add_size(&required_capacity, value_length, 1U) ||
            required_capacity > value_capacity) {
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

[[nodiscard]] static bool ws_header_has_token(const char *value,
                                              const char *token) {
  if (value == nullptr || token == nullptr || token[0] == '\0') {
    return false;
  }

  auto token_length = strlen(token);
  auto cursor = value;

  while (*cursor != '\0') {
    while (*cursor == ' ' || *cursor == '\t' || *cursor == ',') {
      ++cursor;
    }

    auto start = cursor;
    while (*cursor != '\0' && *cursor != ',') {
      ++cursor;
    }

    auto end = cursor;
    while (end > start && (end[-1] == ' ' || end[-1] == '\t')) {
      --end;
    }

    if ((size_t)(end - start) == token_length &&
        strncasecmp(start, token, token_length) == 0) {
      return true;
    }

    if (*cursor == ',') {
      ++cursor;
    }
  }

  return false;
}

[[nodiscard]] static bool ws_compute_accept_value(const char *key, char *output,
                                                  size_t output_capacity) {
  if (key == nullptr || output == nullptr || output_capacity == 0U) {
    return false;
  }

  char challenge[128] = {0};
  auto printed =
      snprintf(challenge, sizeof(challenge), "%s%s", key, WS_ACCEPT_GUID);
  if (printed < 0 || (size_t)printed >= sizeof(challenge)) {
    return false;
  }

  sha1_ctx_t sha1 = {0};
  uint8_t digest[20] = {0};

  ws_sha1_init(&sha1);
  ws_sha1_update(&sha1, (const uint8_t *)challenge, (size_t)printed);
  ws_sha1_final(&sha1, digest);
  auto encoded =
      ws_base64_encode(digest, sizeof(digest), output, output_capacity) != 0;
  ws_secure_wipe(digest, sizeof(digest));
  ws_secure_wipe(challenge, sizeof(challenge));
  return encoded;
}

[[nodiscard]] static bool ws_send_frame(ws_client_t *client, uint8_t opcode,
                                        const uint8_t *payload,
                                        size_t payload_length) {
  if (client == nullptr || !client->connected) {
    return false;
  }
  if (payload == nullptr && payload_length != 0U) {
    ws_set_error(client, "Frame payload pointer is invalid");
    return false;
  }

  size_t length_field_size = 0;
  if (payload_length <= 125) {
    length_field_size = 0;
  } else if (payload_length <= UINT16_MAX) {
    length_field_size = 2;
  } else {
    length_field_size = 8;
  }

  size_t frame_overhead = 0;
  if (!ws_checked_add_size(&frame_overhead, 2U, length_field_size) ||
      !ws_checked_add_size(&frame_overhead, frame_overhead, 4U)) {
    ws_set_error(client, "Frame payload is too large");
    return false;
  }

  size_t frame_length = 0;
  if (!ws_checked_add_size(&frame_length, frame_overhead, payload_length)) {
    ws_set_error(client, "Frame payload is too large");
    return false;
  }

  auto frame = (uint8_t *)calloc(frame_length, sizeof(uint8_t));
  if (frame == nullptr) {
    ws_set_error(client, "Failed to allocate frame buffer");
    return false;
  }

  size_t offset = 0;
  frame[offset++] = (uint8_t)(0x80U | (opcode & 0x0FU));

  if (payload_length <= 125) {
    frame[offset++] = (uint8_t)(0x80U | (uint8_t)payload_length);
  } else if (payload_length <= UINT16_MAX) {
    frame[offset++] = (uint8_t)(0x80U | 126U);
    frame[offset++] = (uint8_t)((payload_length >> 8U) & 0xFFU);
    frame[offset++] = (uint8_t)(payload_length & 0xFFU);
  } else {
    frame[offset++] = (uint8_t)(0x80U | 127U);
    for (size_t shift = 0; shift < 8; ++shift) {
      auto bits = (7U - shift) * 8U;
      frame[offset++] = (uint8_t)(((uint64_t)payload_length >> bits) & 0xFFU);
    }
  }

  uint8_t mask[4] = {0};
  if (!ws_get_random_bytes(mask, sizeof(mask))) {
    free(frame);
    ws_set_error(client, "Failed to generate websocket mask");
    return false;
  }

  memcpy(frame + offset, mask, sizeof(mask));
  offset += sizeof(mask);

  for (size_t i = 0; i < payload_length; ++i) {
    frame[offset + i] = payload[i] ^ mask[i % 4];
  }

  auto ok = ws_send_all(client->socket_fd, client->ssl, client->use_tls, frame,
                        frame_length);
  free(frame);

  if (!ok) {
    ws_set_error(client, "Failed to send frame: %s", strerror(errno));
    return false;
  }

  return true;
}

[[nodiscard]] ws_client_t *ws_client_create() {
  auto client = (ws_client_t *)calloc(1, sizeof(ws_client_t));
  if (client == nullptr) {
    return nullptr;
  }

  client->socket_fd = -1;
  client->connected = false;
  client->use_tls = false;
  client->ssl_ctx = nullptr;
  client->ssl = nullptr;
  client->last_error[0] = '\0';
  return client;
}

void ws_client_destroy(ws_client_t *client) {
  if (client == nullptr) {
    return;
  }

  ws_client_close(client);
  if (client->ssl_ctx != nullptr) {
    wolfSSL_CTX_free(client->ssl_ctx);
    client->ssl_ctx = nullptr;
  }
  free(client);
}

[[nodiscard]] static bool
ws_client_connect_impl(ws_client_t *client, const char *host, uint16_t port,
                       const char *path, bool use_tls) {
  if (client == nullptr) {
    return false;
  }
  if (host == nullptr || path == nullptr) {
    ws_set_error(client, "Host and path must be non-null");
    return false;
  }
  if (!ws_validate_host_text(host)) {
    ws_set_error(client, "Host contains unsupported characters");
    return false;
  }
  if (!ws_validate_request_path(path)) {
    ws_set_error(client, "Path must start with '/' and contain no controls");
    return false;
  }

  if (client->connected) {
    ws_client_close(client);
  }

  int socket_fd = -1;
  WOLFSSL *ssl = nullptr;
  bool success = false;
  uint8_t key_raw[16] = {0};
  char key_encoded[32] = {0};

  char connect_error[128] = {0};
  if (!ws_connect_tcp(host, port, &socket_fd, connect_error,
                      sizeof(connect_error))) {
    if (connect_error[0] != '\0') {
      ws_set_error(client, "Failed to connect to %s:%u (%s)", host,
                   (unsigned)port, connect_error);
    } else {
      ws_set_error(client, "Failed to connect to %s:%u", host, (unsigned)port);
    }
    goto cleanup;
  }

  if (use_tls) {
    if (!ws_init_tls_ctx(client)) {
      ws_set_error(client, "Failed to initialize TLS context");
      goto cleanup;
    }

    if (!ws_connect_tls(host, socket_fd, client->ssl_ctx, &ssl)) {
      ws_set_error(client, "Failed to establish TLS with %s:%u", host,
                   (unsigned)port);
      goto cleanup;
    }
  }

  if (!ws_get_random_bytes(key_raw, sizeof(key_raw))) {
    ws_set_error(client, "Failed to generate handshake key");
    goto cleanup;
  }

  if (ws_base64_encode(key_raw, sizeof(key_raw), key_encoded,
                       sizeof(key_encoded)) == 0) {
    ws_set_error(client, "Failed to encode handshake key");
    goto cleanup;
  }

  char host_header[512] = {0};
  auto is_default_port = use_tls ? (port == 443U) : (port == 80U);
  auto host_header_length =
      snprintf(host_header, sizeof(host_header),
               is_default_port ? "%s" : "%s:%u", host, (unsigned)port);
  if (host_header_length < 0 ||
      (size_t)host_header_length >= sizeof(host_header)) {
    ws_set_error(client, "Handshake Host header is too large");
    goto cleanup;
  }

  char request[1024] = {0};
  auto request_length =
      snprintf(request, sizeof(request),
               "GET %s HTTP/1.1\r\n"
               "Host: %s\r\n"
               "Upgrade: websocket\r\n"
               "Connection: Upgrade\r\n"
               "Sec-WebSocket-Key: %s\r\n"
               "Sec-WebSocket-Version: %u\r\n"
               "\r\n",
               path, host_header, key_encoded, (unsigned)WS_HANDSHAKE_VERSION);

  if (request_length < 0 || (size_t)request_length >= sizeof(request)) {
    ws_set_error(client, "Handshake request is too large");
    goto cleanup;
  }

  if (!ws_send_all(socket_fd, ssl, use_tls, (const uint8_t *)request,
                   (size_t)request_length)) {
    ws_set_error(client, "Failed to send handshake: %s", strerror(errno));
    goto cleanup;
  }

  char response[WS_HTTP_RESPONSE_CAPACITY] = {0};
  if (!ws_read_http_headers(socket_fd, ssl, use_tls, response,
                            sizeof(response))) {
    ws_set_error(client, "Incomplete handshake response from %s:%u", host,
                 (unsigned)port);
    goto cleanup;
  }

  if (!ws_is_switching_protocols_response(response)) {
    ws_set_error(client, "Server rejected websocket upgrade");
    goto cleanup;
  }

  char upgrade_header[WS_MAX_HEADER_VALUE] = {0};
  if (!ws_find_header_value(response, "Upgrade", upgrade_header,
                            sizeof(upgrade_header)) ||
      strcasecmp(upgrade_header, "websocket") != 0) {
    ws_set_error(client, "Handshake missing valid Upgrade header");
    goto cleanup;
  }

  char connection_header[WS_MAX_HEADER_VALUE] = {0};
  if (!ws_find_header_value(response, "Connection", connection_header,
                            sizeof(connection_header)) ||
      !ws_header_has_token(connection_header, "Upgrade")) {
    ws_set_error(client, "Handshake missing valid Connection header");
    goto cleanup;
  }

  char accept_received[WS_MAX_HEADER_VALUE] = {0};
  if (!ws_find_header_value(response, "Sec-WebSocket-Accept", accept_received,
                            sizeof(accept_received))) {
    ws_set_error(client, "Handshake missing Sec-WebSocket-Accept");
    goto cleanup;
  }

  char accept_expected[WS_MAX_HEADER_VALUE] = {0};
  if (!ws_compute_accept_value(key_encoded, accept_expected,
                               sizeof(accept_expected))) {
    ws_set_error(client, "Failed to compute expected accept key");
    goto cleanup;
  }

  if (strcmp(accept_received, accept_expected) != 0) {
    ws_set_error(client, "Invalid Sec-WebSocket-Accept header");
    goto cleanup;
  }

  client->socket_fd = socket_fd;
  client->connected = true;
  client->use_tls = use_tls;
  client->ssl = ssl;
  client->last_error[0] = '\0';
  socket_fd = -1;
  ssl = nullptr;
  success = true;

cleanup:
  ws_secure_wipe(key_raw, sizeof(key_raw));
  ws_secure_wipe(key_encoded, sizeof(key_encoded));

  if (!success) {
    if (ssl != nullptr) {
      wolfSSL_free(ssl);
    }
    if (socket_fd >= 0) {
      (void)close(socket_fd);
    }
  }

  return success;
}

[[nodiscard]] bool ws_client_connect(ws_client_t *client, const char *host,
                                     uint16_t port, const char *path) {
  return ws_client_connect_impl(client, host, port, path, false);
}

[[nodiscard]] bool ws_client_connect_secure(ws_client_t *client,
                                            const char *host, uint16_t port,
                                            const char *path) {
  return ws_client_connect_impl(client, host, port, path, true);
}

[[nodiscard]] bool ws_client_send_text(ws_client_t *client, const char *text,
                                       size_t length) {
  if (client == nullptr) {
    return false;
  }
  if (text == nullptr) {
    ws_set_error(client, "Text payload pointer is invalid");
    return false;
  }

  return ws_send_frame(client, 0x1U, (const uint8_t *)text, length);
}

[[nodiscard]] bool ws_client_send_binary(ws_client_t *client,
                                         const uint8_t *data, size_t length) {
  if (client == nullptr) {
    return false;
  }
  if (data == nullptr) {
    ws_set_error(client, "Binary payload pointer is invalid");
    return false;
  }

  return ws_send_frame(client, 0x2U, data, length);
}

[[nodiscard]] static bool ws_discard_frame_payload(ws_client_t *client,
                                                   uint64_t payload_length,
                                                   const char *payload_name) {
  if (payload_length == 0U) {
    return true;
  }
  if (payload_name == nullptr) {
    payload_name = "frame";
  }

  if (!ws_discard_bytes(client->socket_fd, client->ssl, client->use_tls,
                        payload_length)) {
    ws_set_error(client, "Failed to discard %s payload", payload_name);
    ws_drop_connection(client);
    return false;
  }

  return true;
}

[[nodiscard]] static bool
ws_client_receive_data(ws_client_t *client, uint8_t expected_opcode,
                       const char *expected_name, uint8_t *buffer,
                       size_t capacity, size_t *out_length,
                       bool add_nul_terminator) {
  if (client == nullptr || buffer == nullptr || capacity == 0) {
    return false;
  }
  if (!client->connected) {
    ws_set_error(client, "Client is not connected");
    return false;
  }
  if (out_length != nullptr) {
    *out_length = 0;
  }

  auto terminator_size = add_nul_terminator ? 1U : 0U;
  size_t max_payload_capacity = 0;
  if (!ws_checked_sub_size(&max_payload_capacity, capacity, terminator_size)) {
    ws_set_error(client, "Receive buffer capacity is invalid");
    return false;
  }

  size_t total_payload_length = 0;
  bool message_in_progress = false;

  while (true) {
    uint8_t header[2] = {0};
    if (!ws_read_exact(client->socket_fd, client->ssl, client->use_tls, header,
                       sizeof(header))) {
      ws_set_error(client, "Failed to read websocket frame header");
      ws_drop_connection(client);
      return false;
    }

    auto fin = (header[0] & 0x80U) != 0;
    auto rsv = header[0] & 0x70U;
    auto opcode = header[0] & 0x0FU;
    auto masked = (header[1] & 0x80U) != 0;

    uint64_t payload_length = header[1] & 0x7FU;
    if (payload_length == 126U) {
      uint8_t extended[2] = {0};
      if (!ws_read_exact(client->socket_fd, client->ssl, client->use_tls,
                         extended, sizeof(extended))) {
        ws_set_error(client, "Failed to read frame length");
        ws_drop_connection(client);
        return false;
      }
      payload_length = ((uint64_t)extended[0] << 8U) | (uint64_t)extended[1];
    } else if (payload_length == 127U) {
      uint8_t extended[8] = {0};
      if (!ws_read_exact(client->socket_fd, client->ssl, client->use_tls,
                         extended, sizeof(extended))) {
        ws_set_error(client, "Failed to read frame length");
        ws_drop_connection(client);
        return false;
      }
      payload_length = 0;
      for (size_t i = 0; i < 8; ++i) {
        payload_length = (payload_length << 8U) | (uint64_t)extended[i];
      }
    }

    if (payload_length > WS_MAX_INBOUND_MESSAGE_BYTES) {
      ws_set_error(client, "Inbound frame exceeds security limit");
      ws_drop_connection(client);
      return false;
    }

    if (rsv != 0U) {
      ws_set_error(client, "Server frame has unsupported RSV bits");
      ws_drop_connection(client);
      return false;
    }

    if (masked) {
      ws_set_error(client, "Server frame must not be masked");
      ws_drop_connection(client);
      return false;
    }

    auto is_control_frame = (opcode & 0x08U) != 0U;
    if (is_control_frame && (!fin || payload_length > 125U)) {
      ws_set_error(client, "Invalid control frame format");
      ws_drop_connection(client);
      return false;
    }

    if (opcode == 0x8U && payload_length == 1U) {
      ws_set_error(client, "Invalid close frame payload length");
      ws_drop_connection(client);
      return false;
    }

    if (opcode == 0x8U) {
      if (!ws_discard_frame_payload(client, payload_length, "close")) {
        return false;
      }

      ws_set_error(client, "Connection closed by server");
      ws_drop_connection(client);
      return false;
    }

    if (opcode == 0x9U) {
      if (!fin || payload_length > 125U) {
        ws_set_error(client, "Invalid ping frame length");
        ws_drop_connection(client);
        return false;
      }

      uint8_t ping_payload[125] = {0};
      if (!ws_read_exact(client->socket_fd, client->ssl, client->use_tls,
                         ping_payload, (size_t)payload_length)) {
        ws_set_error(client, "Failed to read ping payload");
        ws_drop_connection(client);
        return false;
      }

      if (!ws_send_frame(client, 0xAU, ping_payload, (size_t)payload_length)) {
        ws_drop_connection(client);
        return false;
      }
      continue;
    }

    if (opcode == 0xAU) {
      if (!fin || payload_length > 125U) {
        ws_set_error(client, "Invalid pong frame length");
        ws_drop_connection(client);
        return false;
      }

      if (!ws_discard_frame_payload(client, payload_length, "pong")) {
        return false;
      }
      continue;
    }

    if (opcode == 0x0U) {
      if (!message_in_progress) {
        if (!ws_discard_frame_payload(client, payload_length, "continuation")) {
          return false;
        }
        ws_set_error(client, "Unexpected continuation frame");
        ws_drop_connection(client);
        return false;
      }
    } else if (opcode == expected_opcode) {
      if (message_in_progress) {
        if (!ws_discard_frame_payload(client, payload_length, expected_name)) {
          return false;
        }
        ws_set_error(client,
                     "Received %s frame while continuation was expected",
                     expected_name);
        ws_drop_connection(client);
        return false;
      }
      message_in_progress = true;
    } else if (opcode == 0x1U || opcode == 0x2U) {
      if (!ws_discard_frame_payload(client, payload_length, "unexpected")) {
        return false;
      }
      ws_set_error(client, "Received unexpected %s frame",
                   (opcode == 0x1U) ? "text" : "binary");
      ws_drop_connection(client);
      return false;
    } else {
      if (!ws_discard_frame_payload(client, payload_length, "unsupported")) {
        return false;
      }
      ws_set_error(client, "Received unsupported opcode 0x%02X", opcode);
      ws_drop_connection(client);
      return false;
    }

    if (payload_length > (uint64_t)SIZE_MAX) {
      ws_set_error(client, "Message is too large for this platform");
      ws_drop_connection(client);
      return false;
    }

    auto fragment_length = (size_t)payload_length;
    size_t next_total_payload_length = 0;
    if (!ws_checked_add_size(&next_total_payload_length, total_payload_length,
                             fragment_length)) {
      ws_set_error(client, "Message is too large for this platform");
      ws_drop_connection(client);
      return false;
    }

    if (next_total_payload_length > max_payload_capacity) {
      size_t required_capacity = 0;
      if (!ws_checked_add_size(&required_capacity, next_total_payload_length,
                               terminator_size)) {
        ws_set_error(client, "Receive buffer requirement overflow");
        ws_drop_connection(client);
        return false;
      }

      ws_set_error(client, "Receive buffer is too small (%zu bytes required)",
                   required_capacity);
      ws_drop_connection(client);
      return false;
    }

    if (!ws_read_exact(client->socket_fd, client->ssl, client->use_tls,
                       buffer + total_payload_length, fragment_length)) {
      ws_set_error(client, "Failed to read frame payload");
      ws_drop_connection(client);
      return false;
    }

    total_payload_length = next_total_payload_length;
    if (!fin) {
      continue;
    }

    if (add_nul_terminator) {
      buffer[total_payload_length] = '\0';
    }

    if (out_length != nullptr) {
      *out_length = total_payload_length;
    }

    return true;
  }
}

[[nodiscard]] bool ws_client_receive_text(ws_client_t *client, char *buffer,
                                          size_t capacity, size_t *out_length) {
  if (client == nullptr) {
    return false;
  }
  if (buffer == nullptr || capacity == 0) {
    ws_set_error(client, "Text receive buffer is invalid");
    return false;
  }

  return ws_client_receive_data(client, 0x1U, "text", (uint8_t *)buffer,
                                capacity, out_length, true);
}

[[nodiscard]] bool ws_client_receive_binary(ws_client_t *client,
                                            uint8_t *buffer, size_t capacity,
                                            size_t *out_length) {
  if (client == nullptr) {
    return false;
  }
  if (buffer == nullptr || capacity == 0) {
    ws_set_error(client, "Binary receive buffer is invalid");
    return false;
  }

  return ws_client_receive_data(client, 0x2U, "binary", buffer, capacity,
                                out_length, false);
}

static void ws_drop_connection(ws_client_t *client) {
  if (client == nullptr) {
    return;
  }

  if (client->ssl != nullptr) {
    (void)wolfSSL_shutdown(client->ssl);
    wolfSSL_free(client->ssl);
    client->ssl = nullptr;
  }
  if (client->socket_fd >= 0) {
    (void)shutdown(client->socket_fd, SHUT_RDWR);
    (void)close(client->socket_fd);
  }

  client->socket_fd = -1;
  client->connected = false;
  client->use_tls = false;
}

void ws_client_close(ws_client_t *client) {
  if (client == nullptr) {
    return;
  }

  if (client->connected) {
    (void)ws_send_frame(client, 0x8U, nullptr, 0);
  }
  ws_drop_connection(client);
}

[[nodiscard]] const char *ws_client_last_error(const ws_client_t *client) {
  if (client == nullptr) {
    return "ws_client_t pointer is nullptr";
  }

  if (client->last_error[0] == '\0') {
    return "no error";
  }

  return client->last_error;
}
