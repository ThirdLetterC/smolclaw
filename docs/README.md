# SmolClaw Documentation

This directory documents the C23 runtime contracts, subsystem boundaries,
configuration surface, and operating runbooks. Source code and generated schema
files remain the source of truth; docs should name the owning file or generated
input whenever behavior is implementation-specific.

## Start Here

| Document | Use |
|---|---|
| [Architecture overview](./overview.md) | One-page map of the runtime shape. |
| [Module map](./modules.md) | Public headers, implementation directories, and private-header ownership. |
| [Modular architecture map](./architecture-map.md) | Longer design map and extension guidance. |
| [Request lifecycle](./request-lifecycle.md) | Message, provider, tool, receipt, and channel flow. |
| [Runtime event loop](./runtime-event-loop.md) | Scheduler, cancellation, timers, and shutdown contract. |

## Contracts And Style

| Document | Use |
|---|---|
| [ABI policy](./abi-policy.md) | Public ABI, plugin, vtable, and versioning rules. |
| [C23 style guide](./c-style-guide.md) | Ownership, formatting, error, and test style. |
| [Dependency inventory](./dependency-inventory.md) | Pinned third-party dependencies and wrapper policy. |
| [Feature flags](./feature-flags.md) | CMake options and dependency cache variables. |

## Operations

| Document | Use |
|---|---|
| [Configuration](./configuration.md) | Schema-backed config and selected field groups. |
| [Local state](./local-state.md) | Workspace-local files, logs, caches, receipts, and secrets guidance. |
| [Service management](./service-management.md) | `smolclaw service` descriptors for systemd and launchctl. |
| [Media services](./media-services.md) | Local Piper and whisper.cpp setup. |
| [Run Telegram Bot With OpenAI](./run-with-openai.md) | Telegram plus OpenAI runbook. |

## Subsystem Rules

The [rules](./rules/) directory contains implementation guardrails for runtime,
providers, tools, channels, gateway, memory, observability, plugins, and
hardware. Treat them as coding checklists before changing those subsystems.
