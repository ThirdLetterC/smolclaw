#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>

#include <uv.h>

#include "jsonrpc/parson.h"

constexpr int32_t DEFAULT_PORT = 8080;
constexpr int32_t DEFAULT_CONNECTIONS = 50;
constexpr double DEFAULT_DURATION_SEC = 5.0;
constexpr double DEFAULT_TIMEOUT_SEC = 5.0;
constexpr size_t READ_CHUNK_BYTES = 4'096;
constexpr size_t MAX_LINE_BYTES = 131'072U;
constexpr double MS_PER_SEC = 1'000.0;
constexpr double NS_PER_SEC = 1'000'000'000.0;

typedef struct {
  const char *host;
  int32_t port;
  int32_t connections;
  double duration_sec;
  double timeout_sec;
  const char *method;
  const char *params_json;
} bench_options_t;

typedef struct {
  char *data;
  size_t len;
} owned_buffer_t;

typedef struct bench_ctx_t bench_ctx_t;
typedef struct bench_conn_t bench_conn_t;

typedef struct {
  uv_write_t req;
  bench_conn_t *conn;
  char *payload;
  size_t len;
} write_ctx_t;

struct bench_conn_t {
  int32_t index;
  bench_ctx_t *ctx;
  uv_tcp_t tcp;
  uv_connect_t connect_req;
  uv_timer_t timeout_timer;
  bool active;
  bool connected;
  bool awaiting_response;
  bool write_inflight;
  bool closing;
  bool timed_out;
  char *read_chunk;
  size_t read_chunk_cap;
  uint8_t *recv_buf;
  size_t recv_len;
  size_t recv_cap;
  uint64_t request_id;
  uint64_t responses;
};

struct bench_ctx_t {
  uv_loop_t loop;
  bench_options_t options;
  JSON_Value *params_value;
  bench_conn_t *connections;
  size_t conn_count;
  size_t active_connections;
  bool send_enabled;
  uv_timer_t duration_timer;
  struct sockaddr_storage addr;
  int addr_len;
  uint64_t start_ns;
  uint64_t end_ns;
  uint64_t timed_out_conns;
};

static void print_usage(FILE *out, const char *program) {
  fprintf(out,
          "Usage: %s [options]\n\n"
          "Options:\n"
          "  --host <host>         Server host (default: 127.0.0.1)\n"
          "  --port <port>         Server port (default: 8080)\n"
          "  --connections <n>     Parallel TCP connections (default: 50)\n"
          "  --duration <sec>      Benchmark duration in seconds (default: 5)\n"
          "  --timeout <sec>       Per-request read timeout in seconds "
          "(default: 5)\n"
          "  --method <name>       JSON-RPC method (default: ping)\n"
          "  --params <json>       Optional JSON params (array or object)\n"
          "  --help                Show this help\n",
          program);
}

static void bench_options_init(bench_options_t *options) {
  options->host = "127.0.0.1";
  options->port = DEFAULT_PORT;
  options->connections = DEFAULT_CONNECTIONS;
  options->duration_sec = DEFAULT_DURATION_SEC;
  options->timeout_sec = DEFAULT_TIMEOUT_SEC;
  options->method = "ping";
  options->params_json = nullptr;
}

[[nodiscard]] static bool parse_int32(const char *text, int32_t *out) {
  if (text == nullptr || out == nullptr) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const long value = strtol(text, &end, 10);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  if (value < INT32_MIN || value > INT32_MAX) {
    return false;
  }
  *out = (int32_t)value;
  return true;
}

[[nodiscard]] static bool parse_double(const char *text, double *out) {
  if (text == nullptr || out == nullptr) {
    return false;
  }
  errno = 0;
  char *end = nullptr;
  const double value = strtod(text, &end);
  if (errno != 0 || end == text || *end != '\0') {
    return false;
  }
  *out = value;
  return true;
}

[[nodiscard]] static int parse_args(bench_options_t *options, int argc,
                                    char **argv) {
  for (int i = 1; i < argc; ++i) {
    const char *arg = argv[i];
    if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
      print_usage(stdout, argv[0]);
      return 1;
    }
    if (strcmp(arg, "--host") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--host requires a value\n");
        return 2;
      }
      options->host = argv[++i];
      continue;
    }
    if (strcmp(arg, "--port") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--port requires a value\n");
        return 2;
      }
      if (!parse_int32(argv[++i], &options->port)) {
        fprintf(stderr, "--port must be an integer\n");
        return 2;
      }
      continue;
    }
    if (strcmp(arg, "--connections") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--connections requires a value\n");
        return 2;
      }
      if (!parse_int32(argv[++i], &options->connections)) {
        fprintf(stderr, "--connections must be an integer\n");
        return 2;
      }
      continue;
    }
    if (strcmp(arg, "--duration") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--duration requires a value\n");
        return 2;
      }
      if (!parse_double(argv[++i], &options->duration_sec)) {
        fprintf(stderr, "--duration must be a number\n");
        return 2;
      }
      continue;
    }
    if (strcmp(arg, "--timeout") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--timeout requires a value\n");
        return 2;
      }
      if (!parse_double(argv[++i], &options->timeout_sec)) {
        fprintf(stderr, "--timeout must be a number\n");
        return 2;
      }
      continue;
    }
    if (strcmp(arg, "--method") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--method requires a value\n");
        return 2;
      }
      options->method = argv[++i];
      continue;
    }
    if (strcmp(arg, "--params") == 0) {
      if (i + 1 >= argc) {
        fprintf(stderr, "--params requires a value\n");
        return 2;
      }
      options->params_json = argv[++i];
      continue;
    }

    fprintf(stderr, "Unknown argument: %s\n", arg);
    return 2;
  }

  return 0;
}

[[nodiscard]] static bool resolve_host(const char *host, int32_t port,
                                       struct sockaddr_storage *out,
                                       int *out_len) {
  if (host == nullptr || out == nullptr || out_len == nullptr) {
    return false;
  }

  char port_buf[16];
  const int port_len = snprintf(port_buf, sizeof(port_buf), "%" PRId32, port);
  if (port_len <= 0 || (size_t)port_len >= sizeof(port_buf)) {
    return false;
  }

  struct addrinfo hints;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_protocol = IPPROTO_TCP;

  struct addrinfo *result = nullptr;
  const int rc = getaddrinfo(host, port_buf, &hints, &result);
  if (rc != 0 || result == nullptr) {
    fprintf(stderr, "resolve failed: %s\n", gai_strerror(rc));
    if (result != nullptr) {
      freeaddrinfo(result);
    }
    return false;
  }

  if ((size_t)result->ai_addrlen > sizeof(*out) ||
      result->ai_addrlen > (socklen_t)INT_MAX) {
    fprintf(stderr, "resolve failed: resolved address too large\n");
    freeaddrinfo(result);
    return false;
  }

  memcpy(out, result->ai_addr, result->ai_addrlen);
  *out_len = (int)result->ai_addrlen;
  freeaddrinfo(result);
  return true;
}

[[nodiscard]] static uint64_t clamp_timeout_ms(double seconds) {
  if (seconds <= 0.0) {
    return 0U;
  }
  double ms = seconds * MS_PER_SEC;
  if (ms < 1.0) {
    ms = 1.0;
  }
  if (ms > (double)UINT64_MAX) {
    return UINT64_MAX;
  }
  return (uint64_t)ms;
}

[[nodiscard]] static bool build_request(owned_buffer_t *out, const char *method,
                                        const JSON_Value *params_value,
                                        uint64_t request_id) {
  if (out == nullptr || method == nullptr) {
    return false;
  }

  JSON_Value *root = json_value_init_object();
  if (root == nullptr) {
    return false;
  }

  JSON_Object *object = json_value_get_object(root);
  if (object == nullptr) {
    json_value_free(root);
    return false;
  }

  if (json_object_set_string(object, "jsonrpc", "2.0") != JSONSuccess ||
      json_object_set_number(object, "id", (double)request_id) != JSONSuccess ||
      json_object_set_string(object, "method", method) != JSONSuccess) {
    json_value_free(root);
    return false;
  }

  if (params_value != nullptr) {
    JSON_Value *params_copy = json_value_deep_copy(params_value);
    if (params_copy == nullptr) {
      json_value_free(root);
      return false;
    }
    if (json_object_set_value(object, "params", params_copy) != JSONSuccess) {
      json_value_free(params_copy);
      json_value_free(root);
      return false;
    }
  }

  const size_t json_size = json_serialization_size(root);
  if (json_size == 0U) {
    json_value_free(root);
    return false;
  }
  if (json_size > SIZE_MAX - 2U) {
    json_value_free(root);
    return false;
  }

  const size_t alloc_size = json_size + 1U;
  // Ownership: caller frees out->data.
  char *buffer = (char *)calloc(alloc_size, 1U);
  if (buffer == nullptr) {
    json_value_free(root);
    return false;
  }

  if (json_serialize_to_buffer(root, buffer, json_size) != JSONSuccess) {
    free(buffer);
    json_value_free(root);
    return false;
  }

  buffer[json_size - 1U] = '\n';
  buffer[json_size] = '\0';
  out->data = buffer;
  out->len = json_size;

  json_value_free(root);
  return true;
}

static void conn_close(bench_conn_t *conn);

static void conn_fail(bench_conn_t *conn, const char *reason) {
  if (conn == nullptr || reason == nullptr) {
    return;
  }
  fprintf(stderr, "connection %" PRId32 ": %s\n", conn->index, reason);
  conn_close(conn);
}

[[nodiscard]] static bool conn_grow_recv(bench_conn_t *conn, size_t needed) {
  if (conn == nullptr) {
    return false;
  }
  if (needed > MAX_LINE_BYTES) {
    return false;
  }
  size_t new_cap = conn->recv_cap == 0U ? READ_CHUNK_BYTES : conn->recv_cap;
  while (new_cap < needed) {
    if (new_cap > MAX_LINE_BYTES / 2U) {
      new_cap = MAX_LINE_BYTES;
      break;
    }
    new_cap *= 2U;
  }
  if (new_cap > MAX_LINE_BYTES) {
    return false;
  }
  uint8_t *new_buf = (uint8_t *)calloc(new_cap, 1U);
  if (new_buf == nullptr) {
    return false;
  }
  if (conn->recv_len > 0U) {
    memcpy(new_buf, conn->recv_buf, conn->recv_len);
  }
  free(conn->recv_buf);
  conn->recv_buf = new_buf;
  conn->recv_cap = new_cap;
  return true;
}

[[nodiscard]] static bool conn_append_recv(bench_conn_t *conn,
                                           const uint8_t *data, size_t len) {
  if (conn == nullptr || data == nullptr) {
    return false;
  }
  if (len == 0U) {
    return true;
  }
  if (conn->recv_len > MAX_LINE_BYTES - len) {
    return false;
  }
  const size_t needed = conn->recv_len + len;
  if (needed > conn->recv_cap) {
    if (!conn_grow_recv(conn, needed)) {
      return false;
    }
  }
  memcpy(conn->recv_buf + conn->recv_len, data, len);
  conn->recv_len = needed;
  return true;
}

static bool conn_consume_lines(bench_conn_t *conn) {
  if (conn == nullptr) {
    return false;
  }
  bool got_response = false;
  while (conn->recv_len > 0U) {
    uint8_t *newline = (uint8_t *)memchr(conn->recv_buf, '\n', conn->recv_len);
    if (newline == nullptr) {
      break;
    }
    const size_t line_len = (size_t)(newline - (uint8_t *)conn->recv_buf);
    const size_t remaining = conn->recv_len - line_len - 1U;
    if (remaining > 0U) {
      memmove(conn->recv_buf, conn->recv_buf + line_len + 1U, remaining);
    }
    conn->recv_len = remaining;
    conn->responses += 1U;
    got_response = true;
  }
  return got_response;
}

static void on_conn_closed(uv_handle_t *handle) {
  bench_conn_t *conn = (bench_conn_t *)handle->data;
  if (conn == nullptr) {
    return;
  }
  bench_ctx_t *ctx = conn->ctx;

  free(conn->read_chunk);
  conn->read_chunk = nullptr;
  conn->read_chunk_cap = 0U;

  free(conn->recv_buf);
  conn->recv_buf = nullptr;
  conn->recv_len = 0U;
  conn->recv_cap = 0U;

  conn->connected = false;
  conn->awaiting_response = false;
  conn->write_inflight = false;
  conn->closing = false;

  if (conn->active && ctx != nullptr) {
    conn->active = false;
    if (ctx->active_connections > 0U) {
      ctx->active_connections -= 1U;
    }
    if (ctx->active_connections == 0U) {
      ctx->end_ns = uv_hrtime();
      uv_stop(&ctx->loop);
    }
  }
}

static void conn_close(bench_conn_t *conn) {
  if (conn == nullptr || conn->closing) {
    return;
  }
  conn->closing = true;

  if (!uv_is_closing((uv_handle_t *)&conn->timeout_timer)) {
    uv_timer_stop(&conn->timeout_timer);
    uv_close((uv_handle_t *)&conn->timeout_timer, nullptr);
  }

  if (!uv_is_closing((uv_handle_t *)&conn->tcp)) {
    uv_read_stop((uv_stream_t *)&conn->tcp);
    uv_close((uv_handle_t *)&conn->tcp, on_conn_closed);
  }
}

static void conn_stop_timeout(bench_conn_t *conn) {
  if (conn == nullptr) {
    return;
  }
  uv_timer_stop(&conn->timeout_timer);
}

static void on_request_timeout(uv_timer_t *timer) {
  bench_conn_t *conn = (bench_conn_t *)timer->data;
  if (conn == nullptr || conn->closing) {
    return;
  }
  if (!conn->timed_out) {
    conn->timed_out = true;
    if (conn->ctx != nullptr) {
      conn->ctx->timed_out_conns += 1U;
    }
  }
  conn_fail(conn, "timeout waiting for response");
}

static void on_write(uv_write_t *req, int status) {
  write_ctx_t *write_ctx = (write_ctx_t *)req;
  if (write_ctx == nullptr) {
    return;
  }
  bench_conn_t *conn = write_ctx->conn;
  if (conn != nullptr) {
    conn->write_inflight = false;
  }
  free(write_ctx->payload);
  write_ctx->payload = nullptr;

  if (conn == nullptr) {
    free(write_ctx);
    return;
  }

  if (conn->closing) {
    free(write_ctx);
    return;
  }

  if (status < 0) {
    free(write_ctx);
    conn_fail(conn, uv_strerror(status));
    return;
  }

  conn->awaiting_response = true;
  uv_timer_start(&conn->timeout_timer, on_request_timeout,
                 clamp_timeout_ms(conn->ctx->options.timeout_sec), 0U);

  free(write_ctx);
}

static void conn_send_next(bench_conn_t *conn) {
  if (conn == nullptr || conn->ctx == nullptr) {
    return;
  }
  bench_ctx_t *ctx = conn->ctx;
  if (!ctx->send_enabled || conn->awaiting_response || conn->write_inflight ||
      conn->closing) {
    return;
  }

  owned_buffer_t request = {.data = nullptr, .len = 0U};
  if (!build_request(&request, ctx->options.method, ctx->params_value,
                     conn->request_id + 1U)) {
    conn_fail(conn, "failed to build request");
    return;
  }
  conn->request_id += 1U;

  write_ctx_t *write_ctx = (write_ctx_t *)calloc(1U, sizeof(write_ctx_t));
  if (write_ctx == nullptr) {
    free(request.data);
    conn_fail(conn, "failed to allocate write context");
    return;
  }
  write_ctx->conn = conn;
  write_ctx->payload = request.data;
  write_ctx->len = request.len;
  if (write_ctx->len > (size_t)UINT_MAX) {
    free(write_ctx->payload);
    free(write_ctx);
    conn_fail(conn, "request too large");
    return;
  }

  uv_buf_t buf = uv_buf_init(write_ctx->payload, (unsigned int)write_ctx->len);
  conn->write_inflight = true;
  const int rc =
      uv_write(&write_ctx->req, (uv_stream_t *)&conn->tcp, &buf, 1, on_write);
  if (rc != 0) {
    conn->write_inflight = false;
    free(write_ctx->payload);
    free(write_ctx);
    conn_fail(conn, uv_strerror(rc));
  }
}

static void on_alloc(uv_handle_t *handle, size_t suggested, uv_buf_t *buf) {
  (void)suggested;
  bench_conn_t *conn = (bench_conn_t *)handle->data;
  if (conn == nullptr || conn->read_chunk == nullptr) {
    buf->base = nullptr;
    buf->len = 0U;
    return;
  }
  buf->base = conn->read_chunk;
  buf->len = (unsigned int)conn->read_chunk_cap;
}

static void on_read(uv_stream_t *stream, ssize_t nread, const uv_buf_t *buf) {
  bench_conn_t *conn = (bench_conn_t *)stream->data;
  if (conn == nullptr || conn->closing) {
    return;
  }

  if (nread > 0) {
    if (!conn_append_recv(conn, (const uint8_t *)buf->base, (size_t)nread)) {
      conn_fail(conn, "response too large");
      return;
    }
    if (conn_consume_lines(conn)) {
      conn_stop_timeout(conn);
      conn->awaiting_response = false;
      if (conn->ctx != nullptr && conn->ctx->send_enabled) {
        conn_send_next(conn);
      } else {
        conn_close(conn);
      }
    }
    return;
  }

  if (nread < 0) {
    if (nread == UV_EOF) {
      conn_fail(conn, "server closed connection");
    } else {
      conn_fail(conn, uv_strerror((int)nread));
    }
  }
}

static void on_connect(uv_connect_t *req, int status) {
  bench_conn_t *conn = (bench_conn_t *)req->data;
  if (conn == nullptr) {
    return;
  }
  if (status < 0) {
    conn_fail(conn, uv_strerror(status));
    return;
  }
  conn->connected = true;

  const int read_status =
      uv_read_start((uv_stream_t *)&conn->tcp, on_alloc, on_read);
  if (read_status != 0) {
    conn_fail(conn, uv_strerror(read_status));
    return;
  }

  if (conn->ctx != nullptr && conn->ctx->send_enabled) {
    conn_send_next(conn);
  } else {
    conn_close(conn);
  }
}

static void on_duration(uv_timer_t *timer) {
  bench_ctx_t *ctx = (bench_ctx_t *)timer->data;
  if (ctx == nullptr) {
    return;
  }
  ctx->send_enabled = false;

  for (size_t i = 0; i < ctx->conn_count; ++i) {
    bench_conn_t *conn = &ctx->connections[i];
    if (!conn->active) {
      continue;
    }
    if (!conn->awaiting_response && !conn->write_inflight) {
      conn_close(conn);
    }
  }
}

[[nodiscard]] static bool conn_setup(bench_ctx_t *ctx, bench_conn_t *conn,
                                     int32_t index) {
  memset(conn, 0, sizeof(*conn));
  conn->index = index;
  conn->ctx = ctx;

  const int tcp_status = uv_tcp_init(&ctx->loop, &conn->tcp);
  if (tcp_status != 0) {
    fprintf(stderr, "connection %" PRId32 ": tcp init failed: %s\n", index,
            uv_strerror(tcp_status));
    return false;
  }
  conn->tcp.data = conn;

  const int timer_status = uv_timer_init(&ctx->loop, &conn->timeout_timer);
  if (timer_status != 0) {
    fprintf(stderr, "connection %" PRId32 ": timer init failed: %s\n", index,
            uv_strerror(timer_status));
    uv_close((uv_handle_t *)&conn->tcp, on_conn_closed);
    return false;
  }
  conn->timeout_timer.data = conn;

  conn->read_chunk = (char *)calloc(READ_CHUNK_BYTES, 1U);
  if (conn->read_chunk == nullptr) {
    fprintf(stderr, "connection %" PRId32 ": read buffer alloc failed\n",
            index);
    uv_close((uv_handle_t *)&conn->timeout_timer, nullptr);
    uv_close((uv_handle_t *)&conn->tcp, on_conn_closed);
    return false;
  }
  conn->read_chunk_cap = READ_CHUNK_BYTES;
  return true;
}

int main(int argc, char **argv) {
  bench_options_t options;
  bench_options_init(&options);

  const int parse_status = parse_args(&options, argc, argv);
  if (parse_status != 0) {
    return parse_status == 1 ? 0 : 2;
  }

  if (options.connections <= 0) {
    fprintf(stderr, "--connections must be > 0\n");
    return 2;
  }
  if (options.port <= 0 || options.port > UINT16_MAX) {
    fprintf(stderr, "--port must be in range 1..65535\n");
    return 2;
  }
  if (options.duration_sec <= 0.0) {
    fprintf(stderr, "--duration must be > 0\n");
    return 2;
  }
  if (options.timeout_sec <= 0.0) {
    fprintf(stderr, "--timeout must be > 0\n");
    return 2;
  }

  JSON_Value *params_value = nullptr;
  if (options.params_json != nullptr) {
    params_value = json_parse_string(options.params_json);
    if (params_value == nullptr) {
      fprintf(stderr, "--params must be valid JSON\n");
      return 2;
    }
  }

  bench_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  ctx.options = options;
  ctx.params_value = params_value;
  ctx.send_enabled = true;

  if (!resolve_host(options.host, options.port, &ctx.addr, &ctx.addr_len)) {
    json_value_free(params_value);
    return 2;
  }

  if (uv_loop_init(&ctx.loop) != 0) {
    fprintf(stderr, "uv_loop_init failed\n");
    json_value_free(params_value);
    return 2;
  }

  const size_t conn_count = (size_t)options.connections;
  ctx.connections = (bench_conn_t *)calloc(conn_count, sizeof(bench_conn_t));
  if (ctx.connections == nullptr) {
    fprintf(stderr, "failed to allocate connections\n");
    uv_loop_close(&ctx.loop);
    json_value_free(params_value);
    return 2;
  }
  ctx.conn_count = conn_count;

  const int timer_status = uv_timer_init(&ctx.loop, &ctx.duration_timer);
  if (timer_status != 0) {
    fprintf(stderr, "duration timer init failed: %s\n",
            uv_strerror(timer_status));
    free(ctx.connections);
    uv_loop_close(&ctx.loop);
    json_value_free(params_value);
    return 2;
  }
  ctx.duration_timer.data = &ctx;

  for (size_t i = 0; i < conn_count; ++i) {
    bench_conn_t *conn = &ctx.connections[i];
    if (!conn_setup(&ctx, conn, (int32_t)i)) {
      continue;
    }

    conn->connect_req.data = conn;
    const int rc =
        uv_tcp_connect(&conn->connect_req, &conn->tcp,
                       (const struct sockaddr *)&ctx.addr, on_connect);
    if (rc != 0) {
      fprintf(stderr, "connection %" PRId32 ": connect failed: %s\n",
              conn->index, uv_strerror(rc));
      conn_close(conn);
      continue;
    }

    conn->active = true;
    ctx.active_connections += 1U;
  }

  if (ctx.active_connections == 0U) {
    fprintf(stderr, "no active connections\n");
    uv_close((uv_handle_t *)&ctx.duration_timer, nullptr);
    uv_run(&ctx.loop, UV_RUN_DEFAULT);
    uv_loop_close(&ctx.loop);
    free(ctx.connections);
    json_value_free(params_value);
    return 2;
  }

  const uint64_t duration_ms = clamp_timeout_ms(options.duration_sec);
  uv_timer_start(&ctx.duration_timer, on_duration, duration_ms, 0U);

  ctx.start_ns = uv_hrtime();
  uv_run(&ctx.loop, UV_RUN_DEFAULT);
  if (ctx.end_ns == 0U) {
    ctx.end_ns = uv_hrtime();
  }

  uint64_t total = 0U;
  for (size_t i = 0; i < conn_count; ++i) {
    total += ctx.connections[i].responses;
  }

  const double elapsed_sec = (double)(ctx.end_ns - ctx.start_ns) / NS_PER_SEC;
  const double rps = elapsed_sec > 0.0 ? (double)total / elapsed_sec : 0.0;

  printf("connections=%" PRId32 "\n", options.connections);
  printf("responses=%" PRIu64 "\n", total);
  printf("timeouts=%" PRIu64 "\n", ctx.timed_out_conns);
  printf("elapsed_sec=%.3f\n", elapsed_sec);
  printf("rps=%.1f\n", rps);

  uv_timer_stop(&ctx.duration_timer);
  uv_close((uv_handle_t *)&ctx.duration_timer, nullptr);
  uv_run(&ctx.loop, UV_RUN_DEFAULT);

  uv_loop_close(&ctx.loop);
  free(ctx.connections);
  json_value_free(params_value);
  return 0;
}
