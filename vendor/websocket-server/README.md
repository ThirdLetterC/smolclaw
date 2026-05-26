# WebSocket Server (C23 + libuv)

Minimal WebSocket echo server written in strict C23 with a libuv transport layer and built with Zig. Protocol handling is separated from the event loop via a small transport interface and exposed to the application through `on_open`, `on_message`, and `on_close` callbacks.

## Features
- HTTP Upgrade handshake with Sec-WebSocket-Key SHA1 + Base64 response and required header validation.
- Frame parsing with client masking enforcement, control frames (ping/pong/close), and text/binary payload delivery.
- Server replies are framed (FIN set, unmasked) for text/binary/close/pong responses.
- Echo behavior: logs text/binary messages and echoes payloads; a text message of `ping` receives a `pong` reply.

## Limitations
- Fragmented data frames are not supported; continuation frames are closed with a protocol error.
- Payloads are capped at 1 MiB to avoid unbounded buffering; control frames are limited to 125 bytes.
- No TLS, authentication, permessage-deflate, or extensions; connections are plain TCP.
- Suitable as a starting point for experimenting with libuv and C23 patterns, not for production use.

## Prerequisites
- Zig (for the build system) and a C toolchain that supports `-std=c23`.
- libuv headers and libraries (e.g., `sudo apt install libuv1-dev` or `brew install libuv`).
- Optional tools: `just` for task shortcuts, `clang-format` for formatting, `valgrind` for leak checks, and `wscat`/`k6` for quick client tests.

## Building
- Default debug build: `just build` or `zig build`.
- Release-style builds: `zig build -Doptimize=ReleaseSafe` (or `ReleaseFast`/`ReleaseSmall`).
- Address/UB/leak sanitizers (Debug only): `zig build -Dsanitizers=true`.

## Running
- Start the server on the default port 8080: `just run` or `zig build run`.
- Choose a port: `zig build run -- 9090` or `just run p=9090`.
- Shutdown signals: SIGINT/SIGTERM trigger a graceful loop stop.
- The server logs connections, message receipt, and close events to stdout.

## Quick Test
- Connect with a WebSocket client: `wscat -c ws://localhost:8080`.
- Send `ping` to receive `pong`, or send any text/binary payload to see it echoed.
- A basic k6 script is available at `ws_test.js` for light load checks.

## Project Layout
- `src/main.c` — wires CLI args, signal handling, and application callbacks.
- `src/server.c` — libuv server setup, connection lifecycle, and transport glue.
- `src/websocket.c` / `include/websocket-server/websocket.h` — protocol handling, framing, and callbacks.
- `build.zig` — Zig build graph, compiler flags (`-std=c23 -Wall -Wextra -Wpedantic -Werror`), and sanitizer toggles.
- `justfile` — helper tasks for build, run, deps, format, and leak checks.
