# SmolClaw Security Model

This document describes the intended security posture for SmolClaw's C23
modular autonomous agent runtime.

The C implementation must assume hostile inputs, explicit ownership bugs,
malicious remote systems, compromised plugins, malformed provider streams, and
operator mistakes. Security is a core architecture boundary, not an add-on after
tools work.

## Scope

The model covers the C23 implementation in the repository root:

- CLI and service process lifecycle.
- Runtime agent loop and tool orchestration.
- Providers and streaming provider parsers.
- Built-in tools and optional tool modules.
- Channels and gateway control plane.
- Memory backends and retrieval.
- Hardware and peripheral control.
- C ABI plugins and optional WASM plugins.
- Config, secrets, observability, and localization.

Firmware-specific security and platform deployment hardening require additional
documents when that work is active.

## Trust Boundaries

- `include/sc/*.h` is the public ABI boundary. Callers are untrusted unless a
  narrower embedding contract says otherwise.
- `src/security/` is the mandatory boundary for side effects.
- `src/runtime/` mediates provider responses, tool calls, memory access, and
  receipts.
- `src/gateway/` accepts untrusted network input.
- `src/channels/` accepts untrusted platform input and media.
- `src/providers/` sends private prompts to remote systems and parses untrusted
  responses.
- `src/tools/` can read, write, execute, browse, call SaaS APIs, and mutate
  external systems.
- `src/plugins/` loads untrusted or semi-trusted code across an ABI boundary.
- `src/hardware/` controls devices that can affect physical systems.
- `src/config/` reads local files and environment data that may be malformed or
  attacker-influenced.
- Third-party libraries are trusted only through local wrapper contracts and
  pinned dependency review.

## Attacker Model

An attacker may control or influence:

- CLI arguments and environment variables.
- Configuration files, workspace contents, symlinks, and file metadata.
- Gateway HTTP, WebSocket, SSE, and static-file requests.
- Channel webhooks, chat messages, attachments, and voice/media payloads.
- Provider responses, streaming chunks, tool-call JSON, token usage metadata,
  and retry behavior.
- Plugin binaries, plugin metadata, and plugin callbacks.
- Hardware serial frames, USB descriptors, and board responses.
- Network DNS, redirects, proxy behavior, slow responses, and large bodies.
- Test fixtures, generated artifacts, and logs submitted in bug reports.

The model also accounts for accidental misuse by operators, such as enabling a
dangerous tool, pasting secrets into prompts, or exposing the gateway on a
broader network than intended.

## Protected Assets

- API keys, channel tokens, gateway pairing secrets, OTP material, and local
  credentials.
- User prompts, conversation history, channel messages, memory records, and
  imported documents.
- Workspace files and directories.
- Host process memory safety and control flow integrity.
- Tool execution boundaries and approval decisions.
- Provider, channel, tool, memory, plugin, and hardware registries.
- Audit logs and receipts.
- Device safety for connected boards and peripherals.
- Public ABI compatibility for plugins and embedders.
- Build and dependency provenance.

## Defensive Architecture

Security-critical decisions are centralized:

- Policy evaluation happens before side effects.
- Approvals and autonomy levels are represented as data, not scattered boolean
  checks.
- Sandbox backends are selected through a `sc_sandbox` contract.
- Receipts are emitted for all meaningful tool side effects.
- Audit events use stable keys and redacted fields.
- Secrets are loaded through config/secrets helpers, never directly by provider
  or channel modules.
- URL parsing and SSRF policy are shared by gateway, providers, and network
  tools.
- Workspace boundary checks are shared by all file and shell tools.
- Plugin capability checks happen before callback dispatch.

The runtime should fail closed. Missing policy, missing capability, malformed
input, allocation failure, unsupported ABI version, or uncertain path identity
must deny the operation or return an explicit error.

## Memory-Safety Posture

C23 does not provide C23's safety guarantees. The implementation compensates
with strict local rules:

- All public objects are opaque unless they are explicitly ABI-stable value
  structs.
- All ownership transfer is documented in the function contract.
- Long-lived allocations go through `sc_allocator`.
- Per-turn temporary allocations use arenas that cannot outlive the request.
- Arena-backed data is never returned as long-lived owned data.
- Sensitive buffers are zeroed before free.
- `sc_status` and output parameters make partial failure explicit.
- Destroy functions tolerate `nullptr` and partially initialized objects.
- Public structs carry size/version fields when ABI evolution is expected.
- Parser and protocol code has fuzz targets.
- CI runs sanitizers and static analysis before C releases.

## Module Security Requirements

### Runtime

- Validate provider tool calls before dispatch.
- Enforce tool recursion and nested-call limits.
- Keep prompt assembly deterministic and bounded.
- Redact private prompt and memory data in logs.
- Emit receipts for tool side effects.
- Preserve cancellation and timeout behavior across provider, tool, and memory
  calls.

### Tools

- Deny execution unless policy allows the exact tool and capability.
- Validate argument JSON against the tool schema.
- Enforce workspace, network, process, and filesystem restrictions.
- Use bounded output capture and timeout limits.
- Avoid shell invocation unless the shell tool specifically owns that behavior.
- Record redacted receipts for success, denial, timeout, and failure.

### Gateway And Channels

- Authenticate or pair before privileged operations.
- Apply request size limits, rate limits, timeouts, and schema validation.
- Validate platform signatures or tokens where available.
- Normalize inbound messages before runtime dispatch.
- Treat attachments and media as untrusted binary input.
- Never expose local files or static assets outside the configured root.

### Providers

- Keep provider credentials in secret-managed storage.
- Apply timeout, retry, redirect, proxy, and TLS policy.
- Parse streaming responses incrementally and defensively.
- Do not trust provider-returned tool names or arguments.
- Avoid logging raw provider payloads.
- Keep OpenAI-compatible providers behind one compatibility wrapper where
  possible.

### Memory

- Bound imported document sizes and record counts.
- Preserve deletion and redaction semantics across backends.
- Avoid storing secrets in embeddings, snapshots, or debug artifacts.
- Validate vector dimensions and metadata before indexing.
- Treat remote vector stores and databases as untrusted I/O.

### Plugins

- Check ABI version, struct sizes, symbol names, and capability descriptors.
- Deny capabilities by default.
- Keep plugin-owned memory and host-owned memory clearly separated.
- Make unload and shutdown paths idempotent.
- Prefer process or WASM isolation for untrusted plugins.
- Treat Python plugins as trusted in-process code, not isolation boundaries.
  Host-mediated tool calls still run through policy, but direct script runtime
  filesystem, network, process, and native-extension APIs can bypass SmolClaw
  policy in v1.

### Hardware

- Match devices by identity and explicit user configuration.
- Require capability checks before GPIO, serial, firmware, or board-control
  actions.
- Support emergency stop and safe shutdown.
- Validate all framing, checksums, lengths, and command IDs.
- Avoid automatic destructive actions on newly discovered devices.

## Dependency Security

Third-party libraries may be used only behind wrappers. Before adoption, record:

- License and compatibility decision.
- Source URL, commit or release, and hash.
- Build flags and enabled features.
- Known parser, network, threading, and allocator assumptions.
- Fuzz, sanitizer, or component tests that exercise the wrapper.

The dependency inventory tracks adopted and evaluated third-party libraries.
None should leak raw dependency types into public SmolClaw headers.

## Build Hardening

Default hardened builds should enable the strongest supported subset of:

- Warnings as errors.
- Stack protector.
- Fortify support where available.
- PIE and RELRO/now for executables.
- Hidden symbols by default for shared libraries.
- ASan, UBSan, and leak sanitizer in debug CI.
- Thread sanitizer for selected async and registry tests where practical.
- `clang-tidy`, `cppcheck`, and compiler static analysis.
- Public header and C++ consumer compile tests.
- ABI and exported-symbol checks for shared builds and plugins.

Security-sensitive release builds should document compiler, linker, dependency,
and platform hardening differences.

## Validation Gates

Before a C23 release, the security gate requires:

- Core unit tests and component tests pass.
- Sanitizer runs are clean.
- Fuzz smoke targets run in CI.
- No unresolved high-severity static-analysis findings.
- Public ABI checks pass.
- Dependency inventory is complete.
- Generated config, CLI, provider, channel, and tool docs are current.
- Secret scanning covers docs, fixtures, generated files, and examples.
- Manual credential-gated provider tests are documented separately from CI.

## Reporting

Security reports should include:

- Affected module and public API, if known.
- Minimal reproduction steps or input.
- Build type, compiler, target OS, and enabled CMake options.
- Sanitizer, static-analysis, or fuzz output when available.
- Whether the issue crosses a tool, gateway, channel, plugin, provider, memory,
  or hardware trust boundary.
- Any observed secret exposure, filesystem mutation, network call, or hardware
  side effect.

Reports must not include real secrets, personal data, private prompts, private
channel messages, or customer documents. Use neutral placeholders following the
repository privacy guidance.

## Policy Before Side Effect

Every side-effecting operation follows this order:

1. Parse and validate arguments.
2. Build a policy request.
3. Ask `sc_security_policy`.
4. Emit denial receipt if denied.
5. Execute only if allowed.
6. Emit success or failure receipt.

Never perform filesystem, shell, network, SaaS, plugin, or hardware effects
before the policy decision.

## Tool Denial Pattern

Tool code should be structured like:

```c
status = sc_tool_validate_args(tool, args_json, &args);
if (!sc_status_is_ok(status)) {
    goto cleanup;
}

status = sc_security_check_tool(policy, tool_name, &args, &decision);
if (!sc_status_is_ok(status)) {
    goto cleanup;
}

if (!decision.allowed) {
    status = sc_receipts_tool_denied(receipts, tool_name, &decision);
    if (sc_status_is_ok(status)) {
        status = sc_status_security_denied(decision.error_key);
    }
    goto cleanup;
}

status = tool_execute(&args, out);
```

Denial is a successful policy outcome but a failed tool execution.

## Workspace Path Pattern

For file operations:

1. Reject empty paths and embedded null bytes.
2. Resolve the configured workspace root.
3. Resolve the target path using race-aware platform helpers.
4. Verify the resolved path remains inside the workspace.
5. For writes, create or open using flags that avoid symlink surprises where
   supported.
6. Re-check identity when replacing files.
7. Emit receipt with redacted path if needed.

Never trust string prefix checks alone.

## SSRF Pattern

For outbound URLs:

1. Parse with the shared URL wrapper.
2. Require an allowed scheme.
3. Normalize host and port.
4. Apply configured allow or deny lists.
5. Resolve DNS through policy-aware helpers.
6. Reject private, loopback, link-local, multicast, metadata, and otherwise
   denied address ranges unless explicitly allowed.
7. Apply redirect limits and repeat checks after redirects.
8. Apply request and response size limits.
9. Apply timeout and cancellation.

Providers and HTTP tools share the same URL and network policy layer.

## Secret Redaction Pattern

Any log, receipt, metric, or error message that may include user or config data
must pass through redaction helpers.

Redact:

- API keys.
- Bearer tokens.
- Basic auth values.
- Cookies.
- Gateway pairing secrets.
- OTP material.
- Private prompts and channel text unless explicitly allowed.
- File contents.

Metrics labels must never contain secrets or high-cardinality private data.

## Gateway Authentication Pattern

Gateway routes should declare:

- Auth mode.
- Required capability.
- Request size limit.
- Timeout.
- Rate-limit bucket.
- Input schema.
- Output schema.

Handler order:

1. Apply method and path match.
2. Apply size limit.
3. Authenticate or pair.
4. Apply rate limit.
5. Parse and validate input.
6. Call runtime or service.
7. Redact and serialize response.

## Channel Signature Pattern

For platform webhooks:

1. Read bounded raw body.
2. Validate signature or token before parsing if platform requires raw body.
3. Reject replay when timestamp or nonce exists.
4. Parse body.
5. Normalize platform event.
6. Drop unsupported event types.
7. Dispatch to channel orchestrator.

Do not log raw webhook bodies.

## Provider Stream Pattern

Provider streaming parsers must:

- Accept arbitrary chunk boundaries.
- Bound accumulated partial data.
- Validate each event type.
- Reject malformed tool-call JSON.
- Preserve cancellation checks.
- Avoid logging raw chunks.
- Clear partial state on failure.

Provider-generated tool calls are untrusted until runtime validates them against
registered tool schemas.

## Plugin Dispatch Pattern

Plugin loading:

1. Load metadata.
2. Check ABI version.
3. Check descriptor size.
4. Check required symbols.
5. Route dynamic, WASM, or Python artifacts by manifest capability and
   extension.
6. Register requested capabilities as denied by default.
7. Enable capabilities only through config or policy.

Plugin dispatch:

1. Validate host-owned inputs.
2. Check capability.
3. Call plugin through ABI wrapper.
4. Validate plugin output.
5. Clear plugin-owned memory through plugin-provided release callbacks.

Python plugins are v1 tool-only dispatch targets. They may register text-output
tools through `host.register_tool`, and their tool calls are still checked by
the same tool policy wrapper used for C ABI and WASM tools.

## Hardware Command Pattern

Hardware commands require:

1. Device identity match.
2. Capability check.
3. Emergency-stop check.
4. Frame validation.
5. Timeout and cancellation.
6. Receipt or audit event.

Discovery must be passive unless the device class requires a safe non-mutating
probe.

## Audit Event Pattern

Audit events should include:

- Stable event key.
- Subsystem.
- Actor or session identifier when available.
- Capability or policy name.
- Redacted target.
- Outcome.
- Error key when failed.

Audit events should not include raw prompts, raw provider payloads, private file
contents, secrets, or unbounded user text.

## Security Review Checklist

- Side effect happens only after policy allow.
- Denial path is tested.
- Receipt or audit event exists.
- Inputs are size-limited.
- Secrets are redacted.
- Cancellation and timeout work.
- Dependency wrapper enforces limits.
- Fuzz tests cover untrusted parser inputs.
