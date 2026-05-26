#include "websocket_client/websocket_client.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint16_t parse_port_or_default(const char *port_text,
                                      uint16_t default_port) {
  if (port_text == nullptr) {
    return default_port;
  }

  char *end = nullptr;
  auto parsed = strtoul(port_text, &end, 10);
  if (end == port_text || *end != '\0' || parsed == 0 || parsed > UINT16_MAX) {
    return default_port;
  }

  return (uint16_t)parsed;
}

typedef struct {
  const char *host;
  uint16_t port;
  const char *path;
} ws_default_endpoint_t;

static const ws_default_endpoint_t WS_DEFAULT_ENDPOINTS[] = {
    {
        .host = "ws.postman-echo.com",
        .port = 443,
        .path = "/raw",
    },
    {
        .host = "echo-websocket.fly.dev",
        .port = 443,
        .path = "/",
    },
};

int main(int argc, char **argv) {
  bool binary_mode = false;
  auto first_arg_index = 1;
  if (argc > 1 && strcmp(argv[1], "--binary") == 0) {
    binary_mode = true;
    first_arg_index = 2;
  }

  const char *host = nullptr;
  auto port = (uint16_t)443;
  const char *path = "/";
  if (argc > first_arg_index) {
    host = argv[first_arg_index];
    port = parse_port_or_default(
        (argc > first_arg_index + 1) ? argv[first_arg_index + 1] : nullptr,
        443);
    path = (argc > first_arg_index + 2) ? argv[first_arg_index + 2] : "/";
  }

  auto message = (argc > first_arg_index + 3)
                     ? argv[first_arg_index + 3]
                     : "hello from c23 websocket client";

  auto client = ws_client_create();
  if (client == nullptr) {
    fprintf(stderr, "failed to allocate websocket client\n");
    return 1;
  }

  if (host != nullptr) {
    if (!ws_client_connect_secure(client, host, port, path)) {
      fprintf(stderr, "connect failed: %s\n", ws_client_last_error(client));
      ws_client_destroy(client);
      return 1;
    }
  } else {
    bool connected = false;
    for (size_t i = 0;
         i < sizeof(WS_DEFAULT_ENDPOINTS) / sizeof(WS_DEFAULT_ENDPOINTS[0]);
         ++i) {
      const ws_default_endpoint_t *endpoint = &WS_DEFAULT_ENDPOINTS[i];
      if (ws_client_connect_secure(client, endpoint->host, endpoint->port,
                                   endpoint->path)) {
        connected = true;
        break;
      }

      fprintf(stderr, "default endpoint %s:%u%s failed: %s\n", endpoint->host,
              (unsigned)endpoint->port, endpoint->path,
              ws_client_last_error(client));
    }

    if (!connected) {
      fprintf(
          stderr,
          "connect failed: unable to reach default websocket echo endpoints\n");
      ws_client_destroy(client);
      return 1;
    }
  }

  if (!binary_mode) {
    if (!ws_client_send_text(client, message, strlen(message))) {
      fprintf(stderr, "send failed: %s\n", ws_client_last_error(client));
      ws_client_destroy(client);
      return 1;
    }

    char receive_buffer[1024] = {0};
    size_t received = 0;
    if (!ws_client_receive_text(client, receive_buffer, sizeof(receive_buffer),
                                &received)) {
      fprintf(stderr, "receive failed: %s\n", ws_client_last_error(client));
      ws_client_destroy(client);
      return 1;
    }

    printf("received %zu text bytes: %s\n", received, receive_buffer);
  } else {
    constexpr uint8_t binary_payload[] = {0x00U, 0x11U, 0x22U,
                                          0x33U, 0x44U, 0x55U};
    if (!ws_client_send_binary(client, binary_payload,
                               sizeof(binary_payload))) {
      fprintf(stderr, "binary send failed: %s\n", ws_client_last_error(client));
      ws_client_destroy(client);
      return 1;
    }

    uint8_t receive_buffer[1024] = {0};
    size_t received = 0;
    if (!ws_client_receive_binary(client, receive_buffer,
                                  sizeof(receive_buffer), &received)) {
      fprintf(stderr, "binary receive failed: %s\n",
              ws_client_last_error(client));
      ws_client_destroy(client);
      return 1;
    }

    printf("received %zu binary bytes:", received);
    for (size_t i = 0; i < received; ++i) {
      printf(" %02X", receive_buffer[i]);
    }
    putchar('\n');
  }

  ws_client_destroy(client);
  return 0;
}
