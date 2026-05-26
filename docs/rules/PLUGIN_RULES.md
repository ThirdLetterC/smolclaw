# SmolClaw Plugin Rules

This file guides implementation of `src/plugins/` and `include/sc/plugin.h`.
Plugins extend SmolClaw through C ABI modules, WASM modules, and trusted
in-process script modules when those hosts are enabled at build time.

## Plugin Boundary

Plugins may provide:

- Tools.
- Channels.
- Providers.
- Memory backends.
- Hardware peripherals.
- Observers.

Python script plugins are v1 tool-only hosts. They must not register providers,
channels, memory backends, observers, peripherals, async callbacks, or binary
attachment producers.

Plugins must not bypass:

- Security policy.
- Capability checks.
- Runtime mediation.
- Registry validation.
- Secret handling.

## ABI Descriptor

Plugin descriptors must include:

- Struct size.
- ABI version.
- Plugin name.
- Plugin version.
- Requested capabilities.
- Entry points.
- Optional metadata.

Host code must check size and ABI version before reading fields beyond the
minimum supported layout.

## Loading Rules

Plugin loading follows:

1. Locate plugin through configured paths.
2. Validate path and file type.
3. Route by manifest capability or artifact extension.
4. Load dynamic module, WASM module, or trusted script module.
5. Resolve required symbols or script exports.
6. Read descriptor or script registration metadata.
7. Check ABI version and size.
8. Register requested capabilities as denied by default.
9. Enable capabilities only through policy or config.

Loading errors must not leave partially registered plugin entries.

## Script Plugin Rules

Script plugin hosts are available only when:

- `SC_ENABLE_PLUGINS=ON`.
- `SC_ENABLE_PYTHON_PLUGINS=ON` for Python artifacts.
- `sc_plugin_host_options.python_enabled` is true for the host instance.

Routing rules:

- Python manifests use `capabilities: ["python", "tool"]` and a `.py`
  artifact.
- Extension routing exists so disabled-runtime denials happen before dynamic
  module loading.

Script API v1:

- Python exports `sc_plugin_init(host)` and may export
  `sc_plugin_shutdown()`.
- `host.register_tool(spec, invoke)` registers a text-output tool.
- `spec` is JSON-compatible and includes `name`, `description`, optional
  `input_schema`, `output_schema`, `risk`, `capability_category`,
  `side_effect`, and `default_autonomy`.
- `invoke(args, call)` returns a string or an object with `success` and
  `output`.

Script plugins are trusted in-process code. They are not a sandbox. A script can
compromise process memory or bypass host policy through direct runtime APIs,
native extensions, child processes, or network/filesystem APIs. SmolClaw policy
applies only to host-mediated tool registration and tool execution.

## Memory Boundary

Rules:

- Host-owned memory is released by host functions.
- Plugin-owned memory is released by plugin release callbacks.
- Do not free plugin memory with host allocators unless the ABI explicitly says
  the host allocator was used.
- Strings crossing the boundary use explicit pointer and length.
- Plugin callbacks must not store borrowed host pointers beyond the call unless
  the ABI grants that lifetime.

## Capability Rules

Capabilities are denied by default.

Examples:

- Register tool.
- Register channel.
- Perform network I/O.
- Read workspace.
- Write workspace.
- Execute process.
- Access hardware.
- Persist memory.

Capability checks happen before every plugin callback that can cause side
effects.

## Unload Rules

Unload must:

- Stop active callbacks.
- Cancel timers.
- Unregister descriptors.
- Release plugin resources.
- Avoid unloading while host-owned references remain.
- Be idempotent for normal shutdown paths.

## Plugin Tests

Required cases:

- Missing required symbol.
- ABI version mismatch.
- Smaller descriptor size.
- Duplicate plugin name.
- Capability denied.
- Tool registration success.
- Plugin callback error.
- Plugin unload with active registration.
- Host/plugin memory ownership mismatch detection where practical.
