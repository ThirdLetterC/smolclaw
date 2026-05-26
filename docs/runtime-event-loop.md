# Runtime-Wide Event Loop Contract

This contract defines the C runtime scheduler target. The current backend is
libuv-backed and lives in `src/runtime/loop.c`; it schedules channel polling,
cron, and heartbeat tasks while each agent turn executes synchronously inside a
scheduled runtime task.

## Ownership Boundary

The runtime-wide event loop owns coordination, not subsystem behavior.

It owns:

- Lifecycle of long-running runtime tasks.
- Timer registration and deadline tracking.
- Monotonic timer handles exposed as opaque `sc_timer_handle` values.
- Ordered session/thread agent-turn queues.
- Turn cancellation tokens and shutdown tokens.
- Scheduling of provider, channel, gateway, cron, heartbeat, and delivery work.
- Observer events for task start, completion, cancellation, timeout, and
  shutdown.

It does not own:

- Provider-specific HTTP request construction or parsing.
- Channel protocol details such as Telegram polling URLs or outbound formatting.
- Gateway route parsing, auth, or response serialization.
- Tool side effects, memory backend I/O, hardware framing, or plugin callbacks.

Subsystems stay behind their public contracts. Event-loop code may drive
`sc_agent`, `sc_channel_orchestrator`, `sc_gateway_server`, and autonomy APIs,
but those subsystems must not depend on event-loop implementation types.

## Core Objects

Current public objects:

```c
typedef struct sc_runtime_loop sc_runtime_loop;
typedef struct sc_cancel_token sc_cancel_token;
typedef struct sc_timer_handle sc_timer_handle;
```

Current entry points:

```c
sc_status sc_runtime_loop_new(sc_allocator *alloc,
                              const sc_runtime_loop_options *options,
                              sc_runtime_loop **out);

sc_status sc_runtime_loop_run(sc_runtime_loop *loop);
sc_status sc_runtime_loop_request_shutdown(sc_runtime_loop *loop,
                                           sc_shutdown_reason reason);
sc_status sc_runtime_loop_shutdown(sc_runtime_loop *loop,
                                   const sc_runtime_shutdown_options *options);
sc_status sc_runtime_loop_add_task(sc_runtime_loop *loop,
                                   const sc_runtime_loop_task_options *options);
sc_status sc_runtime_timer_start(sc_runtime_loop *loop,
                                 const sc_timer_options *options,
                                 sc_timer_handle **out);
sc_status sc_runtime_turn_queue_enqueue(sc_runtime_turn_queue *queue,
                                        const sc_runtime_agent_job *job);
void sc_runtime_loop_destroy(sc_runtime_loop *loop);
```

The current concrete backend is libuv. Builds require an `SC::libuv` target
from either system `pkg-config libuv` or the pinned GitHub dependency path.
Backend-specific types must not appear in public headers.

## Task Classes

The loop schedules these task classes:

| Class | Examples | Scheduling rule |
|---|---|---|
| Gateway | HTTP request, WebSocket chat, SSE stream | Driven by gateway backend events. |
| Channel input | Telegram polling, webhook dispatch, mail/RabbitMQ receives, future platform receives | One active receive per channel unless adapter documents more. |
| Channel output | Replies, draft edits, approvals, cron delivery | Serialized per channel target when ordering matters. |
| Agent turn | User message, cron prompt, gateway chat | Serialized per session/thread by default. |
| Provider request | Completion or stream | Bounded by deadline and cancellation token. |
| Tool execution | Tool calls from provider output | Sequential until parallel policy is explicitly enabled. |
| Cron/heartbeat | Due jobs and periodic health ticks | Timer-driven; missed ticks coalesce. |
| Shutdown cleanup | Flush receipts, final observer events, close channels | Runs after new work is refused. |

## Cancellation

Cancellation is cooperative and token based.

- Every scheduled agent turn receives a turn token.
- Gateway abort, channel `cancel_previous`, shutdown, and timeout all request
  cancellation through the same token path.
- Provider, channel, gateway, memory, tool, hardware, and plugin boundaries must
  receive a cancellation signal before the loop waits for them to finish.
- Cancellation does not erase receipts. If a side effect occurred before the
  cancellation request was observed, the receipt chain records it.
- A cancelled task emits a terminal observer event with stable `error_key`
  context.

Hard interruption is reserved for future isolated plugin/process work. Native C
callbacks are trusted code and cannot be force-killed safely in-process.

## Timers And Deadlines

Timers use monotonic time for intervals and deadlines. Wall time is only for
cron schedule interpretation and user-facing timestamps.

- Provider requests use provider timeout settings.
- Gateway requests use route/session deadlines.
- Channel long polls use adapter-specific poll timeouts plus a shutdown wakeup.
- Approval waits use the channel approval timeout.
- Cron and heartbeat timers coalesce missed ticks instead of running an
  unbounded backlog.
- Deadline expiry is reported as timeout, not generic cancellation.

## Scheduling Rules

- Session/thread ordering is preserved unless a subsystem explicitly opts into
  concurrency.
- One turn per session/thread is active by default.
- New channel messages with `cancel_previous` request cancellation of the active
  turn for that channel/thread before scheduling the replacement.
- Provider fallback attempts are part of the same turn task and share its
  deadline budget unless config later opts into per-provider budgets.
- Gateway chat, WebSocket chat, channel messages, and cron agent jobs all enter
  the same source-tagged `sc_runtime_agent_job` queue before calling
  `sc_agent_process_message`.
- Backpressure is explicit: when a queue is full, the caller receives a
  retryable status and the loop emits a dropped-or-rejected observer event.

## Shutdown

Shutdown has ordered phases:

1. Mark the loop as stopping; reject new gateway, channel, cron, and plugin
   work.
2. Stop scheduling new timers except shutdown deadlines.
3. Signal shutdown tokens to gateway sessions, channel receives, active turns,
   provider requests, tools, memory operations, hardware operations, and plugin
   callbacks.
4. Drain bounded cleanup: receipts, memory persistence, observer terminal
   events, channel finalization, gateway disconnect notices.
5. Force-close transport handles after the configured shutdown deadline.
6. Destroy subsystem handles in reverse construction order.

Shutdown must be idempotent. Repeated shutdown requests may update the reason
or shorten the deadline, but must not restart cleanup.

Runtime-owning CLI commands shut down gracefully by default. `--hard-shutdown`
still requests cooperative cancellation and closes owned transports, but skips
bounded graceful drain/flush callbacks. It does not force-kill native C
callbacks in-process.

## Observer Events

Required stable event names:

- `runtime.loop.start`
- `runtime.loop.stop_requested`
- `runtime.task.start`
- `runtime.task.complete`
- `runtime.task.cancelled`
- `runtime.task.timeout`
- `runtime.task.rejected`
- `runtime.timer.fire`
- `runtime.shutdown.drain`
- `runtime.loop.stopped`

Fields must include task class, session/thread when known, channel/gateway
route when known, timeout/deadline when relevant, and redacted error keys.

## Tests

The C test suite covers the concrete loop backend with deterministic zero-delay
timer handles, ordered shutdown callbacks, and session/thread
`cancel_previous` queue behavior. Remaining deeper integration tests should
cover:

- Gateway request timeout.
- Telegram poll cancellation during shutdown.
- Cron and heartbeat timer coalescing.
- Provider timeout and retry under a shared turn deadline.
- Idempotent shutdown.
- Observer terminal events for success, timeout, cancellation, rejection, and
  shutdown.
