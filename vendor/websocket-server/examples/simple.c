#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "websocket-server/server.h"
#include "websocket-server/websocket.h"

static void demo_on_open([[maybe_unused]] ws_conn_t *conn) {
  puts("[simple] connection opened");
}

static void demo_on_message(ws_conn_t *conn, const uint8_t *data, size_t len,
                            ws_opcode_t opcode) {
  if (opcode == WS_OP_TEXT) {
    printf("[simple] text: %.*s\n", (int)len, (const char *)data);
  } else {
    printf("[simple] opcode=0x%X bytes=%zu\n", (unsigned int)opcode, len);
  }

  ws_conn_send(conn, data, len, opcode);
}

static void demo_on_close([[maybe_unused]] ws_conn_t *conn) {
  puts("[simple] connection closed");
}

int main(int argc, char **argv) {
  constexpr int32_t default_port = 8'080;
  auto port = default_port;

  if (argc > 1) {
    char *end = nullptr;
    errno = 0;
    const auto parsed = strtol(argv[1], &end, 10);
    const bool valid = (errno == 0) && (end != argv[1]) && (*end == '\0');
    if (valid && parsed > 0 && parsed <= UINT16_MAX) {
      port = (int32_t)parsed;
    }
  }

  ws_callbacks_t callbacks = {.on_open = demo_on_open,
                              .on_message = demo_on_message,
                              .on_close = demo_on_close};

  printf("[simple] starting on port %" PRId32 "\n", port);
  start_ws_server(port, callbacks);
  return EXIT_SUCCESS;
}
