#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "jsonrpc/jsonrpc.h"
#include "jsonrpc/server.h"

constexpr int32_t JSONRPC_ERR_INVALID_PARAMS = -32'602;
constexpr int32_t JSONRPC_ERR_INTERNAL = -32'603;

static void on_open([[maybe_unused]] jsonrpc_conn_t *conn) {
  puts("[simple] connection opened");
}

static void on_close([[maybe_unused]] jsonrpc_conn_t *conn) {
  puts("[simple] connection closed");
}

static bool on_request([[maybe_unused]] jsonrpc_conn_t *conn,
                       const char *method, const JSON_Value *params,
                       jsonrpc_response_t *response) {
  if (strcmp(method, "ping") == 0) {
    response->result = json_value_init_string("pong");
    if (response->result == nullptr) {
      response->error_code = JSONRPC_ERR_INTERNAL;
      response->error_message = "Out of memory";
    }
    return true;
  }

  if (strcmp(method, "echo") == 0) {
    if (params == nullptr) {
      response->error_code = JSONRPC_ERR_INVALID_PARAMS;
      response->error_message = "Missing params";
      return true;
    }

    response->result = json_value_deep_copy(params);
    if (response->result == nullptr) {
      response->error_code = JSONRPC_ERR_INTERNAL;
      response->error_message = "Out of memory";
    }
    return true;
  }

  return false;
}

static void on_notification([[maybe_unused]] jsonrpc_conn_t *conn,
                            const char *method,
                            [[maybe_unused]] const JSON_Value *params) {
  printf("[simple] notification: %s\n", method);
}

int main(int argc, char **argv) {
  constexpr int32_t DEFAULT_PORT = 8'080;
  auto port = DEFAULT_PORT;
  if (argc > 1) {
    char *end = nullptr;
    errno = 0;
    const auto parsed = strtol(argv[1], &end, 10);
    const bool valid = (errno == 0) && (end != argv[1]) && (*end == '\0');
    if (valid && parsed > 0 && parsed <= INT32_MAX) {
      port = (int32_t)parsed;
    } else {
      fprintf(stderr, "Invalid port '%s', using %" PRId32 "\n", argv[1], port);
    }
  }

  jsonrpc_callbacks_t callbacks = {.on_open = on_open,
                                   .on_close = on_close,
                                   .on_request = on_request,
                                   .on_notification = on_notification};

  printf("[simple] JSON-RPC server listening on %" PRId32 "\n", port);
  start_jsonrpc_server(port, callbacks);
  return 0;
}
