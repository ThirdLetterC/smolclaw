#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "websocket-server/server.h"
#include "websocket-server/websocket.h"

constexpr size_t READ_CHUNK = 4'096;
static uv_loop_t *g_loop = nullptr;
static uv_tcp_t g_server;
static bool g_shutdown_requested = false;

typedef struct {
  uv_write_t req;
  uint8_t *buffer;
} write_ctx_t;

/**
 * @brief Internal wrapper linking the protocol and libuv handle.
 */
typedef struct {
  uv_tcp_t tcp;
  ws_conn_t *ws;
  ws_transport_t transport;
} client_ctx_t;

static void on_uv_client_closed(uv_handle_t *handle);
static void transport_close(ws_transport_t *self);

static bool is_client_handle(uv_handle_t *handle) {
  if (uv_handle_get_type(handle) != UV_TCP) {
    return false;
  }
  if (handle == (uv_handle_t *)&g_server) {
    return false;
  }
  auto ctx = (client_ctx_t *)handle->data;
  return ctx != nullptr && &ctx->tcp == (uv_tcp_t *)handle;
}

static void close_handle(uv_handle_t *handle, void *arg [[maybe_unused]]) {
  if (uv_is_closing(handle)) {
    return;
  }
  if (is_client_handle(handle)) {
    uv_close(handle, on_uv_client_closed);
    return;
  }
  uv_close(handle, nullptr);
}

static ws_callbacks_t g_callbacks = {
    .on_open = nullptr, .on_message = nullptr, .on_close = nullptr};

void server_set_callbacks(ws_callbacks_t callbacks) { g_callbacks = callbacks; }

void server_request_shutdown() {
  if (g_loop == nullptr) {
    return;
  }

  g_shutdown_requested = true;
  uv_stop(g_loop);
  uv_walk(g_loop, close_handle, nullptr);
}

[[nodiscard]] ws_callbacks_t server_get_callbacks() { return g_callbacks; }

static void on_uv_write(uv_write_t *req, int status) {
  if (status < 0 && req->handle != nullptr) {
    auto ctx = (client_ctx_t *)req->handle->data;
    if (ctx != nullptr) {
      transport_close(&ctx->transport);
    }
  }
  auto ctx = (write_ctx_t *)req;
  free(ctx->buffer);
  free(ctx);
}

static void transport_send_raw(ws_transport_t *self, const uint8_t *data,
                               size_t len) {
  if (self == nullptr) {
    return;
  }
  if (len == 0U) {
    return;
  }
  if (data == nullptr || len > UINT_MAX) {
    transport_close(self);
    return;
  }

  auto ctx = (client_ctx_t *)self->user_data;
  if (ctx == nullptr) {
    return;
  }

  auto write_ctx = (write_ctx_t *)calloc(1, sizeof(write_ctx_t));
  if (write_ctx == nullptr) {
    transport_close(self);
    return;
  }

  write_ctx->buffer = (uint8_t *)calloc(len, sizeof(uint8_t));
  if (write_ctx->buffer == nullptr) {
    free(write_ctx);
    transport_close(self);
    return;
  }

  memcpy(write_ctx->buffer, data, len);

  uv_buf_t buf = uv_buf_init((char *)write_ctx->buffer, (unsigned int)len);
  const int write_status =
      uv_write(&write_ctx->req, (uv_stream_t *)&ctx->tcp, &buf, 1, on_uv_write);
  if (write_status != 0) {
    fprintf(stderr, "uv_write failed: %s\n", uv_strerror(write_status));
    free(write_ctx->buffer);
    free(write_ctx);
    transport_close(self);
  }
}

static void on_uv_client_closed(uv_handle_t *handle) {
  auto ctx = (client_ctx_t *)handle->data;
  if (ctx != nullptr) {
    if (ctx->ws != nullptr) {
      ws_conn_free(ctx->ws);
      ctx->ws = nullptr;
    }
    free(ctx);
  }
}

static void transport_close(ws_transport_t *self) {
  if (self == nullptr) {
    return;
  }
  auto ctx = (client_ctx_t *)self->user_data;
  if (ctx == nullptr) {
    return;
  }
  auto handle = (uv_handle_t *)&ctx->tcp;
  if (uv_is_closing(handle)) {
    return;
  }
  uv_close(handle, on_uv_client_closed);
}

static void on_uv_alloc(uv_handle_t *handle [[maybe_unused]],
                        size_t suggested_size [[maybe_unused]], uv_buf_t *buf) {
  buf->base = (char *)calloc(READ_CHUNK, sizeof(uint8_t));
  buf->len = buf->base == nullptr ? 0U : (unsigned int)READ_CHUNK;
}

static void on_uv_read(uv_stream_t *stream, ssize_t nread,
                       const uv_buf_t *buf) {
  auto ctx = (client_ctx_t *)stream->data;
  if (nread > 0) {
    ws_conn_feed(ctx->ws, (uint8_t *)buf->base, (size_t)nread);
  } else if (nread < 0) {
    ctx->transport.close(&ctx->transport);
  }
  free(buf->base);
}

static void on_new_connection(uv_stream_t *server, int status) {
  if (status < 0) {
    return;
  }

  auto ctx = (client_ctx_t *)calloc(1, sizeof(client_ctx_t));
  if (ctx == nullptr) {
    return;
  }

  const int init_status = uv_tcp_init(server->loop, &ctx->tcp);
  if (init_status != 0) {
    fprintf(stderr, "uv_tcp_init failed: %s\n", uv_strerror(init_status));
    free(ctx);
    return;
  }
  ctx->tcp.data = ctx;

  if (uv_accept(server, (uv_stream_t *)&ctx->tcp) == 0) {
    ctx->transport.user_data = ctx;
    ctx->transport.send_raw = transport_send_raw;
    ctx->transport.close = transport_close;

    ctx->ws = ws_conn_new(ctx->transport, server_get_callbacks(), nullptr);
    if (ctx->ws == nullptr) {
      transport_close(&ctx->transport);
      return;
    }

    const int read_status =
        uv_read_start((uv_stream_t *)&ctx->tcp, on_uv_alloc, on_uv_read);
    if (read_status != 0) {
      fprintf(stderr, "uv_read_start failed: %s\n", uv_strerror(read_status));
      transport_close(&ctx->transport);
    }
  } else {
    uv_close((uv_handle_t *)&ctx->tcp, on_uv_client_closed);
  }
}

void start_ws_server(int32_t port, ws_callbacks_t callbacks) {
  server_set_callbacks(callbacks);

  // Ignore SIGPIPE so a peer hangup does not terminate the process mid-write.
  (void)signal(SIGPIPE, SIG_IGN);

  g_shutdown_requested = false;
  g_loop = uv_default_loop();
  if (g_loop == nullptr) {
    fprintf(stderr, "uv_default_loop failed\n");
    return;
  }
  int run_status = 0;

  const int tcp_status = uv_tcp_init(g_loop, &g_server);
  if (tcp_status != 0) {
    fprintf(stderr, "uv_tcp_init failed: %s\n", uv_strerror(tcp_status));
    goto cleanup;
  }
  g_server.data = nullptr;

  struct sockaddr_in addr;
  const int addr_status = uv_ip4_addr("0.0.0.0", (int)port, &addr);
  if (addr_status != 0) {
    fprintf(stderr, "uv_ip4_addr failed: %s\n", uv_strerror(addr_status));
    goto cleanup;
  }
  const int bind_status =
      uv_tcp_bind(&g_server, (const struct sockaddr *)&addr, 0);
  if (bind_status != 0) {
    fprintf(stderr, "uv_tcp_bind failed: %s\n", uv_strerror(bind_status));
    goto cleanup;
  }

  const int listen_status =
      uv_listen((uv_stream_t *)&g_server, 128, on_new_connection);
  if (listen_status != 0) {
    fprintf(stderr, "uv_listen failed: %s\n", uv_strerror(listen_status));
    goto cleanup;
  }

  run_status = uv_run(g_loop, UV_RUN_DEFAULT);
  if (g_shutdown_requested) {
    // Drain close callbacks to free contexts before exit.
    run_status = uv_run(g_loop, UV_RUN_DEFAULT);
  }

cleanup:
  if (!g_shutdown_requested && g_loop != nullptr) {
    uv_walk(g_loop, close_handle, nullptr);
    (void)uv_run(g_loop, UV_RUN_DEFAULT);
  }

  if (g_loop != nullptr) {
    const int loop_status = uv_loop_close(g_loop);
    if (loop_status != 0) {
      fprintf(stderr, "uv_loop_close failed: %s\n", uv_strerror(loop_status));
    }
  }
  g_loop = nullptr;

  if (run_status != 0) {
    fprintf(stderr, "uv_run exited with active handles (%d).\n", run_status);
  }
}
