# SmolClaw

SmolClaw is a modular autonomous-agent runtime. The repository contains the
runtime core, provider and channel integrations, tool execution surfaces,
security policy layers, plugin support, tests, and deployment helpers.

## Features

- C runtime core with explicit ownership, structured `sc_status` errors, and
  stable public contracts under `include/sc/`.
- Provider routing for built-in and OpenAI-compatible model providers.
- Channel orchestration for CLI, Telegram, webhooks, and other integration
  surfaces.
- Tool execution framework with policy checks, receipts, workspace boundaries,
  and sandbox backends.
- Memory backends for local retrieval and persistence, including markdown,
  SQLite, and no-op implementations.
- Gateway, observability, configuration, localization, media, hardware, and
  plugin subsystems behind modular build flags.
- Native plugin support with optional Python script and WASM-related surfaces
  depending on build configuration.
- Validation support for unit tests, sanitizers, static analysis, dependency
  inventory, and generated documentation checks.

## Repository Map

| Path | Purpose |
|---|---|
| [`include/sc/`](include/sc/) | Stable public C API and ABI-facing contracts. |
| [`src/`](src/) | Runtime, providers, channels, tools, security, memory, gateway, plugins, hardware, and support modules. |
| [`tests/`](tests/) | Unit tests, fixtures, fake modules, and plugin test support. |
| [`docs/`](docs/) | Architecture, module, configuration, lifecycle, and operations documentation. |
| [`examples/`](examples/) | Script and WASM plugin examples. |
| [`vendor/`](vendor/) | Isolated third-party dependency sources. |
| [`tools/`](tools/) | Validation, generated-doc, and static-analysis helpers. |

## Build And Test

```sh
make build
make test
make validate
```

Useful direct CMake entry points:

```sh
cmake --preset release
cmake --build --preset release

cmake --preset asan
cmake --build --preset asan
ctest --test-dir build/asan --output-on-failure
```

## Documentation

- [Architecture overview](docs/overview.md)
- [Modular architecture map](docs/architecture-map.md)
- [Module map](docs/modules.md)
- [Request lifecycle](docs/request-lifecycle.md)
- [Runtime event loop](docs/runtime-event-loop.md)
- [Configuration](docs/configuration.md)
- [Feature flags](docs/feature-flags.md)
- [ABI policy](docs/abi-policy.md)
- [C23 style guide](docs/c-style-guide.md)
- [Security model](SECURITY.md)
- [Dependency inventory](docs/dependency-inventory.md)
- [Service management](docs/service-management.md)
- [Run Telegram Bot With OpenAI](docs/run-with-openai.md)

## Acknowledgements

This project is inspired by the design and architecture of the [ZeroClaw](https://github.com/zeroclaw-labs/zeroclaw) runtime. 
