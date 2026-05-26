# SmolClaw Feature Flags

This inventory must stay in sync with `cmake/options.cmake`,
`cmake/deps.cmake`, and `include/sc/version.h.in`. Minimal builds should leave
optional service surfaces off and enable only the provider, channel, and
dependency capabilities the deployment needs.

| Flag | Default | Purpose |
|---|---:|---|
| `SC_BUILD_FULL` | OFF | Convenience preset that enables the gateway, provider families, and webhook/RabbitMQ/mail channel surfaces. |
| `SC_ENABLE_RUNTIME` | ON | Build the agent runtime. |
| `SC_ENABLE_GATEWAY` | OFF | Enable the HTTP/WebSocket/SSE gateway feature surface. |
| `SC_ENABLE_PLUGINS` | OFF | Enable dynamic C ABI plugin modules. |
| `SC_ENABLE_WASM_PLUGINS` | OFF | Enable the WAMR-backed WASM plugin host. Requires `SC_ENABLE_PLUGINS=ON`. |
| `SC_ENABLE_PYTHON_PLUGINS` | OFF | Enable trusted in-process Python tool plugins through a private statically linked CPython runtime. Requires `SC_ENABLE_PLUGINS=ON`. |
| `SC_ENABLE_HARDWARE` | OFF | Enable hardware support and hardware tools. |
| `SC_ENABLE_POSTGRES` | OFF | Enable PostgreSQL memory backend work. |
| `SC_ENABLE_VOICE` | OFF | Enable voice wake/call features. |
| `SC_ENABLE_AUDIO_PREPROCESSING` | OFF | Enable optional audio preprocessing libraries. |
| `SC_ENABLE_CURL_HTTP2` | OFF | Build GitHub-provided libcurl with HTTP/2 support through nghttp2. |
| `SC_ENABLE_MIMALLOC` | OFF | Use mimalloc when available. |
| `SC_ENABLE_JEMALLOC` | OFF | Use jemalloc when available. |
| `SC_PROVIDER_OPENAI` | ON | Build/report the OpenAI provider. |
| `SC_PROVIDER_ANTHROPIC` | ON | Build/report the Anthropic provider. |
| `SC_PROVIDER_GEMINI` | ON | Build/report the Gemini provider. |
| `SC_PROVIDER_OLLAMA` | ON | Build/report the Ollama provider. |
| `SC_PROVIDER_BEDROCK` | ON | Build/report the Amazon Bedrock provider. |
| `SC_PROVIDER_OPENAI_COMPATIBLE` | ON | Build/report OpenAI-compatible adapters and presets. |
| `SC_PROVIDER_PROCESS_ADAPTERS` | ON | Build/report process-backed provider adapter stubs. |
| `SC_CHANNEL_TELEGRAM` | OFF | Build/report the Telegram channel. Enable it for Telegram runbooks and feature reporting. |
| `SC_CHANNEL_DISCORD` | OFF | Build/report the Discord channel. |
| `SC_CHANNEL_WEBHOOK` | OFF | Build/report the webhook channel. |
| `SC_CHANNEL_RABBITMQ` | OFF | Build/report the RabbitMQ channel. |
| `SC_CHANNEL_MAIL` | OFF | Build/report the mail channel. |
| `SC_SANITIZERS` | OFF | Enable sanitizer compiler/linker flags. |
| `SC_HARDENING` | ON | Enable production hardening flags where supported. |
| `SC_ENABLE_THIRD_PARTY_DEPS` | ON | Build vendored C dependency targets. |
| `SC_ENABLE_THIRD_PARTY_WEBSOCKET_CLIENT` | ON | Build the wolfSSL-backed websocket-client target for Lightpanda CDP. |
| `SC_ENABLE_THIRD_PARTY_WEBSOCKET_SERVER` | ON | Build the libuv-backed websocket-server target. |
| `SC_REQUIRE_SQLITE_FTS5` | ON | Require SQLite FTS5 support, falling back to the GitHub SQLite build when system SQLite lacks it. |

## Dependency Cache Variables

| Variable | Values | Purpose |
|---|---|---|
| `SC_DEPS_PROVIDER` | `github`, `system`, `auto` | Select pinned GitHub builds, system packages, or system-then-GitHub fallback. |
| `SC_CURL_TLS_BACKEND` | `wolfssl`, `mbedtls` | TLS backend for the GitHub-built curl path. |

`smolclaw --features` prints the configured feature flags for the current
binary. `smolclaw doctor` also prints dependency-backed capabilities such as
libcurl, libsodium, TOML, JSON-RPC, websocket support, `SC_HAVE_WAMR` when WASM
plugins are enabled, and `SC_HAVE_CPYTHON` when Python plugins are enabled.
libuv is required for all builds and is not reported as an optional capability.

WASM plugin builds clone the pinned Bytecode Alliance WAMR dependency through CMake and keep it private to the plugin loader:

```sh
cmake -S . -B build/wamr -DSC_DEPS_PROVIDER=auto -DSC_ENABLE_PLUGINS=ON -DSC_ENABLE_WASM_PLUGINS=ON
cmake --build build/wamr
ctest --test-dir build/wamr --output-on-failure
```

Python script plugin builds clone the pinned CPython dependency through CMake and keep all CPython headers private to `src/plugins/`:

```sh
cmake -S . -B build/python -DSC_DEPS_PROVIDER=auto -DSC_ENABLE_PLUGINS=ON -DSC_ENABLE_PYTHON_PLUGINS=ON
cmake --build build/python
ctest --test-dir build/python --output-on-failure
```

Use the checked-in presets for release surfaces:

```sh
cmake --preset release
cmake --build --preset release

cmake --preset release-full
cmake --build --preset release-full
```

When binary size matters, record the executable size from the relevant release
builds in the release notes together with the feature flag set printed by
`smolclaw --features`.
