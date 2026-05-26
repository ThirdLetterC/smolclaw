# SmolClaw Tool Rules

This file guides implementation of `src/tools/` and the public tool contract.
Tools are security-sensitive because they can mutate files, execute commands,
call networks, interact with browsers, use SaaS APIs, modify memory, and control
hardware.

## Tool Boundary

Tools own:

- Tool schema.
- Argument validation.
- Local execution behavior.
- Output normalization.
- Receipt details for their own effects.

Tools do not own:

- Provider tool-call trust.
- Runtime loop control.
- Gateway or channel authentication.
- Policy definition.
- Secrets loading outside approved helpers.

## Tool Descriptor

Each tool registry entry should include:

- Stable name.
- Description localization key.
- Input schema.
- Output schema where useful.
- Capability category.
- Side-effect category.
- Default autonomy level.
- Constructor or static execute callback.
- Generated catalog metadata.

Registry names must be unique and stable.

## Execution Order

Tool execution follows:

1. Validate tool name exists.
2. Validate argument JSON against schema.
3. Normalize arguments into typed C data.
4. Build security policy request.
5. Deny or approve before side effects.
6. Execute with timeout and cancellation.
7. Bound and redact output.
8. Emit receipt.
9. Return structured result.

Argument validation happens before policy so policy receives typed and bounded
data.

## Tool Categories

High-risk tools:

- Shell and process.
- File write and edit.
- HTTP request and web fetch.
- Browser automation.
- SaaS actions.
- MCP tools.
- Memory import/export.
- Hardware control.

Low-risk tools can still become high-risk when they read private data or expose
network-reachable behavior.

## File Tools

File tools must:

- Use workspace boundary helpers.
- Reject embedded null bytes and invalid encodings as required.
- Avoid string-prefix-only path checks.
- Handle symlinks and replacement races.
- Limit read and write sizes.
- Produce receipts for mutations.
- Preserve original file data when edits fail partway through.

## Shell Tools

Shell tools must:

- Be denied by default.
- Require explicit autonomy or approval.
- Use sandbox configuration where available.
- Bound stdout, stderr, runtime, and environment.
- Redact environment secrets.
- Avoid shell invocation for non-shell tools.
- Record command, working directory, exit status, and redacted output summary.

## HTTP And Browser Tools

Network tools must:

- Use shared URL parsing and SSRF policy.
- Apply timeout and body limits.
- Respect proxy policy.
- Redact headers and cookies.
- Restrict redirects.
- Avoid writing downloaded data without file policy checks.

Browser tools inherit both network and filesystem concerns.

## Tool Tests

Required cases:

- Unknown tool.
- Invalid JSON.
- Schema mismatch.
- Policy denial.
- Timeout.
- Cancellation.
- Output truncation.
- Receipt redaction.
- Workspace escape attempt.
- SSRF attempt for network tools.
- Cleanup after execution failure.

Every side-effecting tool should test denial before success.
