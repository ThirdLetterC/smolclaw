# SmolClaw Observability Rules

This file guides implementation of `src/observability/`, observer contracts,
metrics, logs, traces, audit events, and receipts.

## Observability Goals

- Explain runtime behavior without exposing private data.
- Make security decisions auditable.
- Make failures diagnosable with stable keys.
- Support local logs and optional external telemetry.
- Keep observability dependencies replaceable.

## Event Types

SmolClaw should distinguish:

- Diagnostic logs.
- Runtime observer events.
- Metrics.
- Traces or spans.
- Audit events.
- Tool receipts.

Do not use one event path for all purposes if it weakens privacy or security.

## Observer Contract

Observer callbacks should document:

- Event ownership.
- Callback thread or event-loop context.
- Whether callback failure can affect the runtime.
- Redaction guarantees.
- Backpressure or drop behavior.

Observers must not receive raw secrets.

## Stable Fields

Events should include:

- Event key.
- Subsystem.
- Operation.
- Status code or outcome.
- Error key when failed.
- Redacted target when useful.
- Duration where useful.
- Correlation or turn ID.

Avoid high-cardinality private data in metric labels.

## Redaction Rules

Always redact:

- Provider API keys.
- Authorization headers.
- Cookies.
- Gateway pairing secrets.
- OTP material.
- Raw prompts.
- Raw channel messages.
- Private file contents.
- Full provider payloads.

Receipts may include summaries, hashes, or redacted previews when useful.

## Receipts

Receipts should exist for side-effecting tools:

- Shell/process.
- File write/edit/delete.
- HTTP requests.
- Browser automation.
- SaaS mutations.
- Memory import/export.
- Plugin side effects.
- Hardware commands.

Receipts should include policy decision, redacted inputs, outcome, and failure
reason when applicable.

## Metrics

Metrics should cover:

- Runtime turn count and duration.
- Provider request count, latency, and failures.
- Tool calls by category and outcome.
- Security denials.
- Gateway request counts and status.
- Channel inbound and outbound events.
- Memory retrieval counts and latency.

Metric names and labels must be stable and low cardinality.

## Observability Tests

Required cases:

- Observer receives expected events on runtime success.
- Observer receives terminal event on failure.
- Security denial emits audit event.
- Tool receipt redacts secrets.
- Provider error does not log raw payload.
- Metrics labels do not include private data.
- Observer callback failure does not corrupt runtime state.
