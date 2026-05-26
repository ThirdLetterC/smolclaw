# websocket-client

A small C23 WebSocket client library with support for both `ws://` and `wss://` connections.

The project builds with Zig and enforces strict warning hygiene (`-Wall -Wextra -Wpedantic -Werror`) with C23 mode (`-std=c23`).

## Features

- RFC6455 handshake validation (`Sec-WebSocket-Accept`, `Upgrade`, `Connection`, `101` status line)
- Plain TCP (`ws://`) and TLS (`wss://`) connection support
- Text and binary send/receive APIs
- Automatic ping->pong handling during receive
- Defensive input validation for handshake host/path values (rejects control characters and CR/LF injection)
- Protocol invariants enforced on receive (`RSV` bits clear, control-frame constraints, and unmasked server frames)
- Optional sanitizer support in debug builds (ASAN/UBSAN/LSAN)
- C23 checked arithmetic (`<stdckdint.h>`) for overflow-sensitive size math
- Toolchain hardening flags (`-fstack-protector-strong`, `-fPIE`, `RELRO`, and `-D_FORTIFY_SOURCE=3`)
- Fail-closed parsing for malformed/oversized inbound frames

## Requirements

- Zig (build system)
- C toolchain with C23 support
- wolfSSL development library (`wolfssl`)
- POSIX-like environment (sockets, `getaddrinfo`, `/dev/urandom`)

See `SECURITY.md` for the threat model and hardening posture.

## Build

Build the static library and example executable:

```bash
zig build
```

By default, C sources are compiled with strict warnings, stack protection, FORTIFY, and PIE code generation. On Linux, the example executable enables PIE and RELRO.

Install outputs are generated under `zig-out/`, including:

- `zig-out/lib/libwebsocket_client.a`
- `zig-out/bin/ws_simple`

Build without C sanitizers:

```bash
zig build -Dsanitize=false
```

## Test

Run the unit tests:

```bash
zig build test
```

The suite uses a local loopback mock WebSocket server and does not require internet access.

## Coverage

Run unit tests with source-based coverage:

```bash
zig build coverage
```

This generates:

- console coverage summary via `llvm-cov report`
- HTML coverage report under `coverage/html/`

If your tool names differ, override them at build time:

```bash
zig build coverage -Dcoverage-cc=clang -Dcoverage-profdata=llvm-profdata -Dcoverage-cov=llvm-cov
```

## Run the Example

Run the example target:

```bash
zig build run-example -- <host> <port> <path> <message>
```

Example (text mode):

```bash
zig build run-example -- websocket.example.com 443 /chat "hello"
```

Example binary mode:

```bash
zig build run-example -- --binary websocket.example.com 443 /chat
```

If no arguments are provided, `examples/simple.c` attempts these defaults in order:
- `ws.postman-echo.com:443/raw`
- `echo-websocket.fly.dev:443/`

## Public API

Header: `include/websocket_client/websocket_client.h`

- `ws_client_t *ws_client_create()`
- `void ws_client_destroy(ws_client_t *client)`
- `bool ws_client_connect(ws_client_t *client, const char *host, uint16_t port, const char *path)`
- `bool ws_client_connect_secure(ws_client_t *client, const char *host, uint16_t port, const char *path)`
- `bool ws_client_send_text(ws_client_t *client, const char *text, size_t length)`
- `bool ws_client_send_binary(ws_client_t *client, const uint8_t *data, size_t length)`
- `bool ws_client_receive_text(ws_client_t *client, char *buffer, size_t capacity, size_t *out_length)`
- `bool ws_client_receive_binary(ws_client_t *client, uint8_t *buffer, size_t capacity, size_t *out_length)`
- `void ws_client_close(ws_client_t *client)`
- `const char *ws_client_last_error(const ws_client_t *client)`

## Minimal Usage

```c
#include "websocket_client/websocket_client.h"

#include <stdio.h>
#include <string.h>

int main() {
    ws_client_t *client = ws_client_create();
    if (client == nullptr) {
        return 1;
    }

    if (!ws_client_connect_secure(client, "websocket.example.com", 443, "/chat")) {
        fprintf(stderr, "connect failed: %s\n", ws_client_last_error(client));
        ws_client_destroy(client);
        return 1;
    }

    const char message[] = "hello";
    if (!ws_client_send_text(client, message, strlen(message))) {
        fprintf(stderr, "send failed: %s\n", ws_client_last_error(client));
        ws_client_destroy(client);
        return 1;
    }

    char buffer[512] = {0};
    size_t received = 0;
    if (!ws_client_receive_text(client, buffer, sizeof(buffer), &received)) {
        fprintf(stderr, "receive failed: %s\n", ws_client_last_error(client));
        ws_client_destroy(client);
        return 1;
    }

    printf("received %zu bytes: %s\n", received, buffer);
    ws_client_destroy(client);
    return 0;
}
```

## Notes

- `ws_client_receive_text` writes a NUL terminator and needs room for payload + 1 byte.
- Text/binary receive paths reassemble fragmented messages and allow interleaved ping/pong control frames.
- Use `ws_client_last_error()` after a failure to retrieve a human-readable reason.
