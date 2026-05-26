# JSON-RPC Server (C23 + libuv)

Minimal JSON-RPC 2.0 server skeleton written in strict C23 with a libuv TCP transport layer and built with Zig. Protocol handling lives in `src/jsonrpc.c` behind a small transport interface and exposes application callbacks for `on_open`, `on_request`, `on_notification`, and `on_close`. JSON parsing uses the embedded Parson library.

## Status and Limitations

- Implements JSON-RPC 2.0 request/response handling with batch support.
- Framing is newline-delimited JSON (`\n` or `\r\n`); blank lines are ignored.
- Per-message limit: 64 KiB (line length after trimming `\r`).
- Inbound buffer cap: 128 KiB; exceeding either limit sends `Invalid Request` and closes the connection.
- Notifications (including batches of only notifications) do not produce responses.
- Suitable as a starting point for experimenting with libuv and C23 patterns, not for production use.
- No TLS, authentication, or HTTP transport; connections are plain TCP.

## Built-in Methods

- `ping` -> `"pong"`
- `echo` -> returns params (array or object); error if params are missing
- `add` -> sums an array of numbers

## Prerequisites

- Zig (for the build system) and a C toolchain that supports `-std=c23`.
- libuv headers and libraries (e.g., `sudo apt install libuv1-dev` or `brew install libuv`).
- Optional tools: `just` for task shortcuts, `clang-format` for formatting, and `valgrind` for leak checks.

## Building

- Default debug build: `just build` or `zig build`.
- Release build: `zig build release` (override with `-Drelease_mode=ReleaseSafe|ReleaseFast|ReleaseSmall`).
- Standard optimize path: `zig build -Doptimize=ReleaseSafe` (or `ReleaseFast`/`ReleaseSmall`).
- Non-Debug builds enable hardening flags (`-fstack-protector-strong`, `_FORTIFY_SOURCE=3`, `-fPIE`) and RELRO/PIE linker posture.
- Address/UB/leak sanitizers (Debug only): `zig build -Dsanitizers=true`.
- Benchmark tool (`bench_rps`) is installed to `zig-out/bin/bench_rps` by `zig build`.

## Running

- Start the server on the default port 8080: `just run` or `zig build run`.
- Choose a port: `zig build run -- 9090` or `just run p=9090`.
- Release run: `zig build run-release -- 9090`.
- The server listens on `0.0.0.0` and logs connection lifecycle events.
- Shutdown signals: SIGINT/SIGTERM trigger a graceful loop stop.

## Testing (lightweight)

- Use `just test-client` or `nc localhost 8080` and send one JSON-RPC message per line.
- Example request:
  `{"jsonrpc":"2.0","id":1,"method":"ping"}` -> `{"jsonrpc":"2.0","id":1,"result":"pong"}`
- Example echo:
  `{"jsonrpc":"2.0","id":2,"method":"echo","params":{"hello":"world"}}` -> `{"jsonrpc":"2.0","id":2,"result":{"hello":"world"}}`
- Example add:
  `{"jsonrpc":"2.0","id":3,"method":"add","params":[1,2,3]}` -> `{"jsonrpc":"2.0","id":3,"result":6}`
- Notifications omit `id` and receive no response.

## Benchmarking

- Build (if not already): `zig build`
- Example run:
  `./zig-out/bin/bench_rps --host 127.0.0.1 --port 8080 --connections 50 --duration 5 --timeout 5 --method ping`
- Example with params:
  `./zig-out/bin/bench_rps --method echo --params '{"hello":"bench"}'`

## Project Layout

- `src/main.c` — wires CLI args, signal handling, and application callbacks.
- `src/server.c` / `include/jsonrpc/server.h` — libuv server setup, connection lifecycle, and transport glue.
- `src/jsonrpc.c` / `include/jsonrpc/jsonrpc.h` — JSON-RPC protocol handling and callback surfaces.
- `src/parson.c` / `include/jsonrpc/parson.h` — embedded JSON parser.
- `src/arena.c` / `include/jsonrpc/arena.h` — small arena allocator used by the protocol layer.
- `tools/bench_rps.c` — JSON-RPC benchmark client.
- `build.zig` — Zig build graph, compiler flags (`-std=c23 -Wall -Wextra -Wpedantic -Werror`), and sanitizer toggles.
- `justfile` — helper tasks for build, run, deps, format, and leak checks.

## Valgrind Memory Profiling

Leak checks (Linux):

- `zig build valgrind -- 8080`
- `just check-leaks`

Massif profiling:

```bash
zig build -Dvalgrind

valgrind --tool=massif --stacks=yes ./zig-out/bin/jsonrpc_server
ms_print massif.out.<pid>
```
