# SmolClaw Runtime Rules

This file guides AI coding assistants implementing `src/runtime/` in SmolClaw's
C23 codebase. The runtime is the agent kernel. It coordinates providers, tools,
memory, security, observability, cancellation, and history.

## Runtime Responsibilities

The runtime owns:

- Agent turn lifecycle.
- Prompt assembly and history trimming.
- Provider request construction.
- Provider streaming and non-streaming response handling.
- Tool-call validation and dispatch.
- Memory load, write-back, and retrieval hooks.
- Receipts for tool effects.
- Cancellation and timeout propagation.
- Runtime-wide event-loop coordination for long-running gateway, channel,
  provider, cron, heartbeat, and shutdown tasks.
- Observer events for turn progress.

The runtime does not own:

- Provider-specific HTTP behavior.
- Tool side effects.
- Channel protocol details.
- Gateway transport details.
- Database driver details.
- Hardware command framing.

## Core Entry Points

Current public entry points:

```c
sc_status sc_runtime_create(sc_allocator *alloc,
                            const sc_runtime_config *config,
                            sc_runtime **out);

sc_status sc_runtime_process_message(sc_runtime *runtime,
                                     const sc_runtime_message *message,
                                     sc_allocator *alloc,
                                     sc_runtime_response *out);

sc_status sc_runtime_cancel(sc_runtime *runtime, sc_runtime_turn_id turn_id);

void sc_runtime_destroy(sc_runtime *runtime);
```

All entry points must document ownership, cancellation behavior, and observer
events.

## Turn Lifecycle

Each turn should follow this order:

1. Validate message and runtime state.
2. Create turn arena and cancellation token.
3. Load config, active policy, memory context, and tool registry snapshot.
4. Build prompt with bounded history.
5. Call provider.
6. Validate provider output.
7. Dispatch tool calls only after policy allows them.
8. Append tool results and continue provider loop if needed.
9. Persist memory updates if configured.
10. Emit final observer event and response.
11. Clear turn arena.

Failures must still clear temporary state and emit terminal observer events.

## Tool Loop Rules

- Tool names must match registered tool descriptors.
- Tool arguments must validate against the registered schema.
- Tool call count and recursion depth must be bounded.
- Parallel tool execution is optional and must not be introduced without an
  explicit scheduling decision.
- Tool output size must be bounded before it is added to history or prompts.
- Tool results must distinguish success, denial, timeout, cancellation, and
  execution failure.

Provider-returned tool calls are untrusted data.

## Prompt And History Rules

- Prompt assembly must be deterministic for the same inputs.
- Token and byte budgets must be enforced.
- Private memory and channel content must be redacted when logged.
- Tool receipts should not be pasted into prompts unless the prompt contract
  requires them.
- System and developer instructions must remain separate from user content.
- Memory retrieval should be attributed internally without leaking storage
  metadata to providers unnecessarily.

## Cancellation Rules

Cancellation must propagate to:

- Provider request or stream.
- Tool execution.
- Gateway and channel waits.
- Memory retrieval or embedding.
- Hardware commands.
- Timers and scheduled routines.

After cancellation, cleanup must still run and side-effect receipts must still
represent any operation that already occurred.

## Event Loop Rules

The shared event-loop contract lives in
[runtime-event-loop.md](../runtime-event-loop.md). Implementations must keep
backend types private and preserve these boundaries:

- Gateway, channel, provider, cron, heartbeat, and agent work enter one
  scheduler instead of creating independent agent loops.
- Timers and deadlines use monotonic time, except cron schedule interpretation.
- Shutdown first rejects new work, then signals cancellation, drains bounded
  cleanup, closes transports, and destroys subsystem handles.
- Session/thread ordering is preserved by default.
- Blocking native C callbacks are cooperative; force-kill behavior belongs only
  to future process or WASM isolation.

## Observer Events

Emit events for:

- Turn started.
- Prompt built.
- Provider request started.
- Provider stream delta if enabled.
- Tool call requested.
- Tool call denied.
- Tool call completed.
- Memory read or write summary.
- Turn completed, failed, timed out, or cancelled.

Events must use stable keys and redacted fields.

## Runtime Tests

Use fake providers, fake tools, fake memory, fake policy, and fake observers.

Required cases:

- Simple message response.
- Provider error.
- Provider streaming malformed chunk.
- Unknown tool call.
- Tool schema validation failure.
- Tool policy denial.
- Tool success with receipt.
- Tool recursion limit.
- Cancellation before provider.
- Cancellation during provider stream.
- Cancellation during tool execution.
- Memory retrieval failure.
- Observer events on success and failure.

No runtime test should require real provider credentials.
