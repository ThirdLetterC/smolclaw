# Module Map

This page maps public headers to their owning implementation directories. The
longer subsystem contracts live in [Modular architecture map](./architecture-map.md).

## Public Surfaces

| Public surface | Implementation | Notes |
|---|---|---|
| `sc/api.h`, `sc/version.h` | `src/core/`, build metadata | Base macros, ABI version, build feature reporting. |
| `sc/allocator.h` | `src/core/allocator.c` | Heap, arena, and test allocation contracts. |
| `sc/string.h`, `sc/buffer.h`, `sc/vector.h`, `sc/map.h` | `src/core/` | Borrowed spans, owned strings/bytes, containers, checked growth. |
| `sc/result.h` | `src/core/result.c` | `sc_status`, stable error keys, diagnostics. |
| `sc/json.h`, `sc/toml.h`, `sc/url.h` | `src/json/`, `src/config/`, `src/net/` | Dependency-wrapped parsers and URL helpers. |
| `sc/provider.h` | `src/providers/` | Provider vtables, request/response types, streaming events, retry helpers. |
| `sc/channel.h`, `sc/acp.h` | `src/channels/` | Inbound envelopes, outbound replies, channel orchestration, ACP JSON-RPC. |
| `sc/tool.h` | `src/tools/` | Tool specs, schemas, side-effect metadata, invocation, receipts. |
| `sc/memory.h` | `src/memory/` | No-op, markdown, SQLite, hydration, and memory tools. |
| `sc/security.h`, `sc/sandbox.h` | `src/security/` | Policy, approvals, path/domain checks, receipts, sandbox selection, emergency stop. |
| `sc/runtime.h`, `sc/bootstrap.h` | `src/runtime/` | Agent turns, runtime loop, turn queue, cancellation, bootstrap wiring. |
| `sc/gateway.h` | `src/gateway/` | HTTP-style control-plane contract and request handling. |
| `sc/media.h` | `src/media/` | Attachment storage, ASR, TTS, Opus/WAV helpers. |
| `sc/observer.h`, `sc/log.h`, `sc/i18n.h` | `src/observability/`, `src/i18n/` | Logs, observer events, metrics snapshots, localization lookup. |
| `sc/plugin.h` | `src/plugins/` | Native plugin host and optional Python/WASM host surfaces. |
| `sc/hardware.h`, `sc/peripheral.h` | `src/hardware/` | Peripheral registry, fake hardware, policy validation, emergency stop integration. |
| `sc/registry.h`, `sc/contract.h`, `sc/autonomy.h`, `sc/cli.h` | `src/registry/`, `src/contracts/`, `src/autonomy/`, `src/app/` | Shared registries, stability/capability metadata, autonomy helpers, CLI command descriptions. |

## Private Headers

Most private interfaces live under `include/<subsystem>/` today, for example
`include/runtime/loop_internal.h`, `include/security/security_internal.h`, and
`include/tools/tool_internal.h`. Treat them as owned by the matching
`src/<subsystem>/` directory. Do not include a private header from another
subsystem unless the owning module intentionally exposes that narrow internal
contract.

## Implementation Directories

| Directory | Responsibility |
|---|---|
| `src/app/` | CLI commands, onboarding, service management, process lifecycle. |
| `src/autonomy/` | Autonomy levels, SOP/cron delivery targets, policy helper data. |
| `src/channels/` | CLI/fake channels, Telegram, webhooks, ACP, mail/RabbitMQ/transport adapters. |
| `src/config/` | Config loading, generated schema metadata, source parsing, secret stores. |
| `src/contracts/`, `src/registry/` | Shared handle wrappers, descriptor registries, observer lists. |
| `src/core/` | Allocators, containers, strings, buffers, time, status. |
| `src/gateway/` | Gateway request handling and runtime dispatch. |
| `src/hardware/` | Peripheral registry, fake devices, hardware tool registration. |
| `src/json/`, `src/net/` | JSON/schema helpers, URL parsing, libcurl/libuv HTTP support. |
| `src/media/` | Local ASR/TTS clients and media encoding. |
| `src/memory/` | Memory backends, persistence, hydration, common helpers. |
| `src/observability/`, `src/i18n/` | Logging, observer implementations, metrics, localization. |
| `src/plugins/` | Plugin loading, C ABI descriptors, Python script host. |
| `src/providers/` | HTTP providers, OpenAI-compatible adapters, local process adapters, router/reliable wrappers. |
| `src/runtime/` | Agent loop, bootstrap, runtime event loop, turn queue, channel policy. |
| `src/security/` | Policy, approvals, audit, receipts, sandbox backends, workspace boundaries. |
| `src/tools/` | Built-in tools, wrappers, process runner, MCP, browser, HTTP, memory, cron/SOP tools. |
| `tests/` | Unit tests, fixtures, fake modules, plugin tests, sanitizer and validation support. |

## Related Docs

- [Documentation index](./README.md)
- [Architecture overview](./overview.md)
- [Request lifecycle](./request-lifecycle.md)
- [ABI policy](./abi-policy.md)
- [C23 style guide](./c-style-guide.md)
