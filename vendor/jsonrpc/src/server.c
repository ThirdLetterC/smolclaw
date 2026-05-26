#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "jsonrpc/jsonrpc.h"
#include "jsonrpc/server.h"

constexpr size_t READ_CHUNK_MIN = 1'024;
constexpr size_t READ_CHUNK_MAX = 4'096;
constexpr int32_t SERVER_BACKLOG = 4'096;
static uv_loop_t *g_loop = nullptr;
static uv_tcp_t g_server;
static bool g_shutdown_requested = false;

static void on_uv_client_closed(uv_handle_t *handle);
static void transport_close(jsonrpc_transport_t *self);

static void close_handle(uv_handle_t *handle, void *arg [[maybe_unused]]) {
  if (!uv_is_closing(handle)) {
    if (uv_handle_get_type(handle) == UV_TCP && handle->data != nullptr) {
      uv_close(handle, on_uv_client_closed);
      return;
    }
    uv_close(handle, nullptr);
  }
}

typedef struct {
  uv_write_t req;
  jsonrpc_transport_t *transport;
  uint8_t data[];
} write_ctx_t;

/**
 * @brief Internal wrapper linking the protocol and libuv handle.
 */
typedef struct {
  uv_tcp_t tcp;
  jsonrpc_conn_t *rpc;
  jsonrpc_transport_t transport;
  uint8_t *read_buffer;
  size_t read_capacity;
} client_ctx_t;

static jsonrpc_callbacks_t g_callbacks = {.on_open = nullptr,
                                          .on_close = nullptr,
                                          .on_request = nullptr,
                                          .on_notification = nullptr};

void server_set_callbacks(jsonrpc_callbacks_t callbacks) {
  g_callbacks = callbacks;
}

void server_request_shutdown() {
  if (g_loop == nullptr) {
    return;
  }

  g_shutdown_requested = true;
  uv_stop(g_loop);
  uv_walk(g_loop, close_handle, nullptr);
}

[[nodiscard]] jsonrpc_callbacks_t server_get_callbacks() { return g_callbacks; }

static void on_uv_write(uv_write_t *req, int status) {
  auto write_ctx = (write_ctx_t *)req;
  jsonrpc_transport_t *transport =
      write_ctx != nullptr ? write_ctx->transport : nullptr;
  free(write_ctx);

  if (status < 0 && transport != nullptr && transport->close != nullptr) {
    fprintf(stderr, "uv_write callback failed: %s\n", uv_strerror(status));
    transport->close(transport);
  }
}

[[nodiscard]] static bool transport_send_raw(jsonrpc_transport_t *self,
                                             const uint8_t *data, size_t len) {
  if (self == nullptr || data == nullptr || len == 0U) {
    return false;
  }
  auto ctx = (client_ctx_t *)self->user_data;
  if (ctx == nullptr || uv_is_closing((uv_handle_t *)&ctx->tcp)) {
    return false;
  }

  if (len > (size_t)UINT_MAX || len > SIZE_MAX - sizeof(write_ctx_t)) {
    transport_close(self);
    return false;
  }
  const size_t alloc_size = sizeof(write_ctx_t) + len;
  auto write_ctx = (write_ctx_t *)calloc(1, alloc_size);
  if (write_ctx == nullptr) {
    transport_close(self);
    return false;
  }
  write_ctx->transport = &ctx->transport;

  memcpy(write_ctx->data, data, len);

  uv_buf_t buf = uv_buf_init((char *)write_ctx->data, (unsigned int)len);
  const int write_status =
      uv_write(&write_ctx->req, (uv_stream_t *)&ctx->tcp, &buf, 1, on_uv_write);
  if (write_status != 0) {
    fprintf(stderr, "uv_write failed: %s\n", uv_strerror(write_status));
    free(write_ctx);
    transport_close(self);
    return false;
  }
  return true;
}

static void on_uv_client_closed(uv_handle_t *handle) {
  if (handle == nullptr) {
    return;
  }

  auto ctx = (client_ctx_t *)handle->data;
  if (ctx == nullptr) {
    return;
  }

  handle->data = nullptr;
  if (ctx->rpc != nullptr) {
    jsonrpc_conn_free(ctx->rpc);
    ctx->rpc = nullptr;
  }
  free(ctx->read_buffer);
  free(ctx);
}

static void transport_close(jsonrpc_transport_t *self) {
  if (self == nullptr || self->user_data == nullptr) {
    return;
  }

  auto ctx = (client_ctx_t *)self->user_data;
  if (!uv_is_closing((uv_handle_t *)&ctx->tcp)) {
    uv_close((uv_handle_t *)&ctx->tcp, on_uv_client_closed);
  }
}

static void on_uv_alloc(uv_handle_t *handle, size_t suggested_size,
                        uv_buf_t *buf) {
  auto ctx = (client_ctx_t *)handle->data;
  if (ctx == nullptr) {
    buf->base = nullptr;
    buf->len = 0U;
    return;
  }
  if (ctx->read_buffer == nullptr) {
    size_t alloc_size = suggested_size;
    if (alloc_size < READ_CHUNK_MIN) {
      alloc_size = READ_CHUNK_MIN;
    } else if (alloc_size > READ_CHUNK_MAX) {
      alloc_size = READ_CHUNK_MAX;
    }

    ctx->read_buffer = (uint8_t *)calloc(alloc_size, sizeof(uint8_t));
    ctx->read_capacity = ctx->read_buffer == nullptr ? 0U : alloc_size;
    if (ctx->read_buffer == nullptr) {
      buf->base = nullptr;
      buf->len = 0U;
      transport_close(&ctx->transport);
      return;
    }
  }
  buf->base = (char *)ctx->read_buffer;
  buf->len = (unsigned int)ctx->read_capacity;
}

static void on_uv_read(uv_stream_t *stream, ssize_t nread,
                       const uv_buf_t *buf) {
  auto ctx = (client_ctx_t *)stream->data;
  if (ctx == nullptr) {
    return;
  }

  if (nread > 0) {
    if (buf == nullptr || buf->base == nullptr) {
      ctx->transport.close(&ctx->transport);
      return;
    }

    if (ctx->rpc != nullptr) {
      jsonrpc_conn_feed(ctx->rpc, (uint8_t *)buf->base, (size_t)nread);
    }
  } else if (nread < 0) {
    ctx->transport.close(&ctx->transport);
  }
}

static void on_new_connection(uv_stream_t *server, int status) {
  if (status < 0) {
    fprintf(stderr, "on_new_connection failed: %s\n", uv_strerror(status));
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

    ctx->rpc =
        jsonrpc_conn_new(ctx->transport, server_get_callbacks(), nullptr);
    if (ctx->rpc == nullptr) {
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

void start_jsonrpc_server(int32_t port, jsonrpc_callbacks_t callbacks) {
  server_set_callbacks(callbacks);

  // Ignore SIGPIPE so a peer hangup does not terminate the process mid-write.
  (void)signal(SIGPIPE, SIG_IGN);

  g_shutdown_requested = false;
  g_loop = uv_default_loop();
  if (g_loop == nullptr) {
    fprintf(stderr, "uv_default_loop failed.\n");
    return;
  }

  int run_status = 0;
  const int tcp_status = uv_tcp_init(g_loop, &g_server);
  if (tcp_status != 0) {
    fprintf(stderr, "uv_tcp_init failed: %s\n", uv_strerror(tcp_status));
    goto cleanup_loop;
  }

  struct sockaddr_in addr;
  const int addr_status = uv_ip4_addr("0.0.0.0", (int)port, &addr);
  if (addr_status != 0) {
    fprintf(stderr, "uv_ip4_addr failed: %s\n", uv_strerror(addr_status));
    goto cleanup_loop;
  }
  const int bind_status =
      uv_tcp_bind(&g_server, (const struct sockaddr *)&addr, 0);
  if (bind_status != 0) {
    fprintf(stderr, "uv_tcp_bind failed: %s\n", uv_strerror(bind_status));
    goto cleanup_loop;
  }

  const int listen_status = uv_listen((uv_stream_t *)&g_server,
                                      (int)SERVER_BACKLOG, on_new_connection);
  if (listen_status != 0) {
    fprintf(stderr, "uv_listen failed: %s\n", uv_strerror(listen_status));
    goto cleanup_loop;
  }

  run_status = uv_run(g_loop, UV_RUN_DEFAULT);
  if (g_shutdown_requested) {
    // Drain close callbacks to free contexts before exit.
    run_status = uv_run(g_loop, UV_RUN_DEFAULT);
  }

cleanup_loop:
  uv_walk(g_loop, close_handle, nullptr);
  (void)uv_run(g_loop, UV_RUN_DEFAULT);

  const int loop_status = uv_loop_close(g_loop);
  if (loop_status != 0) {
    fprintf(stderr, "uv_loop_close failed: %s\n", uv_strerror(loop_status));
  }
  g_loop = nullptr;

  if (run_status != 0) {
    fprintf(stderr, "uv_run exited with active handles (%d).\n", run_status);
  }
}
