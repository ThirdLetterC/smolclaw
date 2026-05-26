# Dependency Inventory

This inventory tracks dependency provenance for the C23 runtime. Dependency
wrappers translate dependency behavior into SmolClaw-owned values and
`sc_status`; Public headers must not expose dependency types, constants, or
dependency-owned memory.

`SC_DEPS_PROVIDER` selects `github`, `system`, or `auto`. The GitHub path uses
the pinned versions below. The system path may use distribution packages when
the wrapper probes accept their feature set. libuv is required for all builds;
configuration fails if neither the system probe nor the GitHub provider can
create `SC::libuv`.

## GitHub-Built Pins

| CMake pin | Dependency | Version | Source |
|---|---|---:|---|
| `SC_LIBUV_TAG` | libuv | `v1.52.1` | `https://github.com/libuv/libuv.git` |
| `SC_CURL_TAG` | curl | `curl-8_20_0` | `https://github.com/curl/curl.git` |
| `SC_WOLFSSL_TAG` | wolfSSL | `v5.9.1-stable` | `https://github.com/wolfSSL/wolfssl.git` |
| `SC_MBEDTLS_TAG` | Mbed TLS | `mbedtls-4.1.0` | `https://github.com/Mbed-TLS/mbedtls.git` |
| `SC_NGHTTP2_TAG` | nghttp2 | `v1.69.0` | `https://github.com/nghttp2/nghttp2.git` |
| `SC_LIBSODIUM_TAG` | libsodium | `1.0.22-RELEASE` | `https://github.com/jedisct1/libsodium.git` |
| `SC_CMARK_TAG` | cmark | `0.31.2` | `https://github.com/commonmark/cmark.git` |
| `SC_OPUS_TAG` | Opus | `v1.5.2` | `https://github.com/xiph/opus.git` |
| `SC_SQLITE_TAG` | SQLite | `version-3.53.0` | `https://github.com/sqlite/sqlite.git` |
| `SC_ISOCLINE_TAG` | isocline | `v1.1.0` | `https://github.com/daanx/isocline.git` |
| `SC_WAMR_TAG` | wasm-micro-runtime | `WAMR-2.4.4` | `https://github.com/bytecodealliance/wasm-micro-runtime.git` |
| `SC_CPYTHON_TAG` | CPython | `v3.14.4` | `https://github.com/python/cpython.git` |
| `SC_MIMALLOC_TAG` | mimalloc | `v3.3.2` | `https://github.com/microsoft/mimalloc.git` |
| `SC_JEMALLOC_TAG` | jemalloc | `5.3.1` | `https://github.com/jemalloc/jemalloc/releases` |

`SC_JEMALLOC_SHA256` is
`3826bc80232f22ed5c4662f3034f799ca316e819103bdc7bb99018a421706f92` for the
jemalloc `5.3.1` release archive.

## Local Vendor Targets

The `vendor/` tree is built only through wrapper targets registered in
`cmake/deps.cmake`:

| Target | Role |
|---|---|
| `sc_tp_arena` | Arena allocator support. |
| `sc_tp_clags` | CLI argument parsing. |
| `sc_tp_hiredis` | Redis client support for optional memory or channel work. |
| `sc_tp_mqtt` | MQTT client support. |
| `sc_tp_microjson` | Small JSON parser support. |
| `sc_tp_nanocron` | Cron expression scheduling. |
| `sc_tp_parson` | JSON DOM support for JSON-RPC and wrappers. |
| `sc_tp_jsonrpc` | JSON-RPC helpers. |
| `sc_tp_psimd` | Header-only SIMD support. |
| `sc_tp_rabbitmq` | RabbitMQ channel support. |
| `sc_tp_toml` | TOML parsing. |
| `sc_tp_ulog` | Logging backend support. |
| `sc_tp_url` | URL parsing helpers. |
| `sc_tp_websocket_protocol` | WebSocket protocol framing. |
| `sc_tp_websocket_server` | WebSocket server transport. |
| `sc_tp_websocket_client` | WebSocket client transport. |

## Wrapper Policy

- Dependency code stays behind `src/<subsystem>/` wrappers or private helper
  headers.
- Public headers include only standard C headers and `sc/*.h` headers.
- Dependency errors are translated into stable `sc_status` values with redacted
  diagnostics.
- Dependency-owned memory is copied into `sc_string`, `sc_bytes`, or other
  SmolClaw-owned values before crossing public boundaries.
- Feature probes and build flags live in CMake, not in public ABI contracts.
