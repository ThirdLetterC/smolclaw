#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "websocket-server/server.h"
#include "websocket-server/websocket.h"

/**
 * @brief Application-level callback triggered when a WebSocket handshake
 * completes.
 */
void my_on_open([[maybe_unused]] ws_conn_t *conn) {
  printf("[Server] New WebSocket connection opened.\n");
}

/**
 * @brief Application-level callback triggered when a message is received.
 * Logic: Echoes the message back to the client with a prefix.
 */
void my_on_message(ws_conn_t *conn, const uint8_t *data, size_t len,
                   ws_opcode_t opcode) {
  // Respond to simple text ping with pong, otherwise echo.
  if (opcode == WS_OP_TEXT && len == 4U && memcmp(data, "ping", 4U) == 0) {
    constexpr char pong[] = "pong";
    ws_conn_send(conn, (const uint8_t *)pong, sizeof(pong) - 1U, WS_OP_TEXT);
    return;
  }

  if (opcode == WS_OP_TEXT) {
    printf("[Server] Received: %.*s\n", (int)len, (const char *)data);
  } else {
    printf("[Server] Received binary data of length: %zu\n", len);
  }

  ws_conn_send(conn, data, len, opcode);
}

/**
 * @brief Application-level callback triggered when the connection is closed.
 */
void my_on_close([[maybe_unused]] ws_conn_t *conn) {
  printf("[Server] WebSocket connection closed.\n");
}

static void on_signal(uv_signal_t *handle, int signum [[maybe_unused]]) {
  printf("[Server] Shutdown signal received, closing...\n");
  (void)uv_signal_stop(handle);
  uv_close((uv_handle_t *)handle, nullptr);
  server_request_shutdown();
}

int main(int argc, char **argv) {
  constexpr int32_t DEFAULT_PORT = 8'080;
  auto port = DEFAULT_PORT;
  if (argc > 1) {
    char *end = nullptr;
    errno = 0;
    const auto parsed = strtol(argv[1], &end, 10);
    const bool valid = (errno == 0) && (end != argv[1]) && (*end == '\0');
    if (valid && parsed > 0 && parsed <= UINT16_MAX) {
      port = (int32_t)parsed;
    } else {
      fprintf(stderr, "Invalid port '%s', falling back to %" PRId32 "\n",
              argv[1], port);
    }
  }

  // 1. Define application callbacks
  ws_callbacks_t callbacks = {.on_open = my_on_open,
                              .on_message = my_on_message,
                              .on_close = my_on_close};

  /**
   * NOTE: In a full implementation, you would pass these callbacks
   * into the server initialization. Here, we assume the server
   * start logic (provided in the previous turn) uses these.
   */
  printf("Starting WebSocket Echo Server on port %" PRId32 "...\n", port);

  auto loop = uv_default_loop();
  if (loop == nullptr) {
    fprintf(stderr, "uv_default_loop failed\n");
    return EXIT_FAILURE;
  }
  uv_signal_t sigint_handle;
  uv_signal_t sigterm_handle;

  if (uv_signal_init(loop, &sigint_handle) == 0) {
    (void)uv_signal_start(&sigint_handle, on_signal, SIGINT);
  }
  if (uv_signal_init(loop, &sigterm_handle) == 0) {
    (void)uv_signal_start(&sigterm_handle, on_signal, SIGTERM);
  }

  // For this demonstration, we call the placeholder start function
  start_ws_server(port, callbacks);

  return 0;
}
