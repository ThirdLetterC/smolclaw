# hiredis

High-performance C23 Redis client library with synchronous and asynchronous APIs, optional TLS support, and adapter headers.

**Build**

1. `zig build`
2. `zig build install -Dprefix=/path` (optional)

Or with `just`:
1. `just build`
2. `just build-sanitize`
3. `just clean`

**Build Options**

1. `-Dssl=true` enables wolfSSL support (links `wolfssl`, `pthread` for shared builds).
2. `-Dexamples=true` installs the example binaries.
3. `-Dlibuv=true` builds the libuv example (requires system `libuv`).
4. `-Dshared=false` disables the shared library.
5. `-Dstatic=false` disables the static library.
6. `-Dsanitize=true` enables `-fsanitize=address,undefined,leak` (enabled by default for debug builds on non-Windows targets).

**Examples**

1. `zig build examples`
2. `just examples`

Example binaries:

1. `example`
2. `example-push`
3. `example-poll`
4. `example-streams-threads`
5. `example-ssl` (requires `-Dssl=true`)
6. `example-libuv` (requires `-Dlibuv=true`)

**Headers**

1. Public headers live in `include/hiredis`.
2. Adapter headers live in `adapters`.

**Formatting**

1. `just format`
2. `just format-check`

**License**

1. See `COPYING`.
