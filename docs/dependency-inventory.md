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

| CMake pin | Dependency | Version | Commit | Source |
|---|---|---:|---|---|
| `SC_LIBUV_TAG` | libuv | `v1.52.1` | `1cfa32ff59c076ffb6ed735bbc8c18361558661f` | `https://github.com/libuv/libuv.git` |
| `SC_CURL_TAG` | curl | `curl-8_20_0` | `a05f34973e6c4bb629d018f7cb51487be1c904d8` | `https://github.com/curl/curl.git` |
| `SC_WOLFSSL_TAG` | wolfSSL | `v5.9.1-stable` | `1d363f3adceba9d1478230ede476a37b0dcdef24` | `https://github.com/wolfSSL/wolfssl.git` |
| `SC_MBEDTLS_TAG` | Mbed TLS | `mbedtls-4.1.0` | `0fe989b6b514192783c469039edd325fd0989806` | `https://github.com/Mbed-TLS/mbedtls.git` |
| `SC_NGHTTP2_TAG` | nghttp2 | `v1.69.0` | `68cb6900fde14c77f0cd7add0e094a862960eb99` | `https://github.com/nghttp2/nghttp2.git` |
| `SC_LIBSODIUM_TAG` | libsodium | `1.0.22-RELEASE` | `77e1ce5d6dee871c49ef211222ba18ef0c486bda` | `https://github.com/jedisct1/libsodium.git` |
| `SC_CMARK_TAG` | cmark | `0.31.2` | `eec0eeba6d31189fd828314576494566d539b1e3` | `https://github.com/commonmark/cmark.git` |
| `SC_OPUS_TAG` | Opus | `v1.5.2` | `ddbe48383984d56acd9e1ab6a090c54ca6b735a6` | `https://github.com/xiph/opus.git` |
| `SC_SQLITE_TAG` | SQLite | `version-3.53.0` | `4ebc7fdcf459e8d88eb5b019c2949bda86565528` | `https://github.com/sqlite/sqlite.git` |
| `SC_ISOCLINE_TAG` | isocline | `v1.1.0` | `d55a58139badbe83d61c5d89954fa5bddcabe6d7` | `https://github.com/daanx/isocline.git` |
| `SC_WAMR_TAG` | wasm-micro-runtime | `WAMR-2.4.4` | `8c18e3f68b16c4bcaf05996b2636f6ed2b4cf629` | `https://github.com/bytecodealliance/wasm-micro-runtime.git` |
| `SC_CPYTHON_TAG` | CPython | `v3.14.4` | `23116f998f6789d8c2fbe5ed5b8146854c8c2a4f` | `https://github.com/python/cpython.git` |
| `SC_MIMALLOC_TAG` | mimalloc | `v3.3.2` | `30b2d9d89099bee08e9f67a1ffb3e12e7ba45227` | `https://github.com/microsoft/mimalloc.git` |
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
