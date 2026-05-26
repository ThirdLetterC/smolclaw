# Architecture Overview

SmolClaw is a modular C23 codebase. The public ABI lives in
`include/sc/`; concrete implementations live under
`src/<subsystem>/`.

## High-level Shape

```mermaid
flowchart TB
    subgraph External["External world"]
        UI["CLI / chat platforms / gateway clients"]
        LLM["LLM providers"]
        FS["Filesystem · shell · network · hardware"]
    end

    subgraph Edge["Edge subsystems"]
        CH["src/channels"]
        GW["src/gateway"]
        PR["src/providers"]
        TL["src/tools"]
    end

    subgraph Core["Core subsystems"]
        RT["src/runtime"]
        SEC["src/security"]
        MEM["src/memory"]
        CFG["src/config"]
    end

    UI --> CH
    UI --> GW
    CH --> RT
    GW --> RT
    RT --> SEC
    RT --> PR
    RT --> TL
    RT --> MEM
    RT --> CFG
    PR --> LLM
    TL --> FS
```

## Modules In Scope

| Module | Role |
|---|---|
| `include/sc/` | Stable public headers, opaque handles, versioned vtables |
| `src/app/` | CLI, service integration, process lifecycle |
| `src/core/` | Allocators, strings, buffers, vectors, maps, errors |
| `src/runtime/` | Agent loop, prompt assembly, history, tool orchestration |
| `src/security/` | Policy checks, approvals, workspace boundaries, audit, receipts |
| `src/providers/` | Provider registry and built-in model providers |
| `src/tools/` | Tool registry and built-in tools |
| `src/channels/` | Channel registry and orchestration |
| `src/memory/` | Memory backends and retrieval |
| `src/gateway/` | HTTP, WebSocket, SSE, pairing, dashboard APIs |
| `src/hardware/` | USB, serial, GPIO, board support |
| `src/plugins/` | C ABI plugins and optional isolated plugin hosts |

## Request Lifecycle

```mermaid
sequenceDiagram
    participant U as User
    participant CH as Channel/Gateway
    participant RT as Runtime
    participant SEC as Security
    participant PR as Provider
    participant TL as Tool

    U->>CH: message / webhook / API request
    CH->>RT: normalized inbound event
    RT->>PR: chat request with tool schemas
    PR-->>RT: stream: text or tool call
    RT->>SEC: evaluate tool call
    SEC-->>RT: approved or denied
    RT->>TL: invoke approved tool
    TL-->>RT: result and receipt data
    RT->>PR: continue with tool result
    PR-->>RT: final text
    RT-->>CH: response
    CH-->>U: platform-native reply
```

Full detail: [Request lifecycle](./request-lifecycle.md).

## Extension Points

Providers, channels, tools, memory backends, observers, hardware adapters, and
plugins all use opaque handles plus `const` vtables. Registries own descriptor
metadata, and runtime code depends on registry contracts rather than concrete
implementations.

## Where To Read Next

- [Modular architecture map](./architecture-map.md)
- [Request lifecycle](./request-lifecycle.md)
- [Module map](./modules.md)
- [Dependency inventory](./dependency-inventory.md)
