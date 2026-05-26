#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <uv.h>

#include "jsonrpc/jsonrpc.h"
#include "jsonrpc/server.h"

constexpr int32_t JSONRPC_ERR_INVALID_PARAMS = -32'602;
constexpr int32_t JSONRPC_ERR_INTERNAL = -32'603;

static const char *libuv_fs_runtime() {
#if defined(__linux__)
  constexpr unsigned int UV_VER_1_45_0 = (1U << 16) | (45U << 8) | 0U;
  constexpr unsigned int UV_VER_1_49_0 = (1U << 16) | (49U << 8) | 0U;
  const auto runtime_version = uv_version();
  if (runtime_version >= UV_VER_1_45_0 && runtime_version < UV_VER_1_49_0) {
    return "io_uring";
  }
#endif
  return "thread pool";
}

void my_on_open([[maybe_unused]] jsonrpc_conn_t *conn) {
  printf("[Server] New JSON-RPC connection opened.\n");
}

static bool handle_add(const JSON_Value *params, jsonrpc_response_t *response) {
  if (params == nullptr || json_value_get_type(params) != JSONArray) {
    response->error_code = JSONRPC_ERR_INVALID_PARAMS;
    response->error_message = "Expected array params";
    return true;
  }

  auto array = json_value_get_array(params);
  const size_t count = json_array_get_count(array);
  double sum = 0.0;
  for (size_t i = 0U; i < count; ++i) {
    const JSON_Value *item = json_array_get_value(array, i);
    if (item == nullptr || json_value_get_type(item) != JSONNumber) {
      response->error_code = JSONRPC_ERR_INVALID_PARAMS;
      response->error_message = "All params must be numbers";
      return true;
    }
    sum += json_value_get_number(item);
  }

  response->result = json_value_init_number(sum);
  if (response->result == nullptr) {
    response->error_code = JSONRPC_ERR_INTERNAL;
    response->error_message = "Out of memory";
  }
  return true;
}

bool my_on_request([[maybe_unused]] jsonrpc_conn_t *conn, const char *method,
                   const JSON_Value *params, jsonrpc_response_t *response) {
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

  if (strcmp(method, "add") == 0) {
    return handle_add(params, response);
  }

  return false;
}

void my_on_notification([[maybe_unused]] jsonrpc_conn_t *conn,
                        const char *method, const JSON_Value *params) {
  if (params != nullptr && json_value_get_type(params) == JSONString) {
    printf("[Server] Notification %s: %s\n", method,
           json_value_get_string(params));
    return;
  }
  printf("[Server] Notification %s\n", method);
}

void my_on_close([[maybe_unused]] jsonrpc_conn_t *conn) {
  printf("[Server] JSON-RPC connection closed.\n");
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
      fprintf(stderr,
              "Invalid port '%s' (expected 1..65535), falling back to %" PRId32
              "\n",
              argv[1], port);
    }
  }

  // Define application callbacks
  jsonrpc_callbacks_t callbacks = {.on_open = my_on_open,
                                   .on_close = my_on_close,
                                   .on_request = my_on_request,
                                   .on_notification = my_on_notification};
  printf("Starting JSON-RPC Server on port %" PRId32 "...\n", port);
  printf("libuv fs runtime: %s\n", libuv_fs_runtime());

  auto loop = uv_default_loop();
  uv_signal_t sigint_handle;
  uv_signal_t sigterm_handle;

  if (uv_signal_init(loop, &sigint_handle) == 0) {
    (void)uv_signal_start(&sigint_handle, on_signal, SIGINT);
  }
  if (uv_signal_init(loop, &sigterm_handle) == 0) {
    (void)uv_signal_start(&sigterm_handle, on_signal, SIGTERM);
  }

  start_jsonrpc_server(port, callbacks);

  return 0;
}
