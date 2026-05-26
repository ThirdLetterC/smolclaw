# MQTT-C (C23)

A small, portable MQTT 3.1.1 client library implemented in strict C23, with
examples and tests.

**Highlights**

- Strict C23 codebase (`nullptr`, `auto`, `constexpr`) with `-Wall -Wextra -Wpedantic -Werror`.
- Single public header: `include/mqtt/mqtt.h`.
- Platform abstraction in `include/mqtt/mqtt_pal.h`, with an override hook.
- Examples for TCP, reconnects, and TLS (OpenSSL).
- Thread-safe API suitable for single-threaded and multi-threaded clients.

**Requirements**
- A C23-capable compiler (`-std=c23` or `-std=c2x`).
- POSIX sockets and pthreads for the default PAL and examples.
- OpenSSL and `pkg-config` for the TLS example.
- Optional: `just` or Zig for build convenience.

**Build With just**
```sh
just
just test
just build-examples
```

Debug with sanitizers:
```sh
DEBUG=1 just test
```

Outputs land in `bin/`.

**Build With Zig**
```sh
zig build
zig build test
zig build examples
```

Enable sanitizers in Debug builds:
```sh
zig build -Doptimize=Debug -Dsanitize=true
```

Outputs land in `zig-out/bin/`.

**Quick Usage Sketch**
```c
#include "mqtt/mqtt.h"

int main() {
  struct mqtt_client client;
  uint8_t sendbuf[2'048];
  uint8_t recvbuf[1'024];
  mqtt_pal_socket_handle sockfd = /* open socket */;

  mqtt_init(&client, sockfd, sendbuf, sizeof(sendbuf), recvbuf, sizeof(recvbuf),
            nullptr);
  mqtt_connect(&client, nullptr, nullptr, nullptr, 0, nullptr, nullptr,
               MQTT_CONNECT_CLEAN_SESSION, 400);

  while (true) {
    mqtt_sync(&client);
  }
}
```

See `examples/` for complete programs using POSIX or OpenSSL sockets.

**Tests**
- `just test` or `zig build test` runs `testing/tests.c`.
- Tests and examples default to `test.mosquitto.org` and require network access.

**Platform Abstraction**
- The default PAL lives in `include/mqtt/mqtt_pal.h` and `src/mqtt_pal.c`.
- To supply your own PAL, define `MQTTC_PAL_FILE` to a header that provides the
  required types, macros, and function declarations:
```sh
cc -DMQTTC_PAL_FILE=my_mqtt_pal.h ...
```

**Directory Layout**
- `include/mqtt/mqtt.h` public API.
- `src/mqtt.c` core client.
- `src/mqtt_pal.c` platform helpers.
- `examples/` example clients.
- `testing/tests.c` unit and protocol tests.

**License**
MIT. See `LICENSE`.
