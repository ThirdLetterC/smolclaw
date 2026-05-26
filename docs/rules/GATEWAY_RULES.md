# SmolClaw Gateway Rules

This file guides implementation of `src/gateway/`. The gateway exposes local or
network control surfaces through HTTP, WebSocket, SSE, and static assets.

## Gateway Boundary

The gateway owns:

- HTTP routing.
- WebSocket sessions.
- SSE streams.
- Pairing and local authentication flow.
- Request and response serialization.
- Rate limits.
- Static asset serving.

The gateway does not own:

- Runtime business logic.
- Tool execution.
- Provider credentials.
- Channel platform logic.
- Plugin capability policy.

## Route Descriptor

Each route should declare:

- Method.
- Path.
- Auth mode.
- Required capability.
- Request size limit.
- Timeout.
- Rate-limit bucket.
- Input schema.
- Output schema.
- Handler.

Route metadata should support generated API docs.

## Handler Order

Gateway handlers follow:

1. Match method and path.
2. Enforce request size limit.
3. Authenticate or pair.
4. Apply rate limit.
5. Parse and validate input.
6. Call runtime or service adapter.
7. Redact response.
8. Serialize output.

Authentication must happen before privileged parsing side effects.

## Pairing

Pairing code must:

- Use secure random generation.
- Expire pairing tokens.
- Bind token to session or origin where applicable.
- Prevent replay.
- Log only redacted token metadata.
- Rate-limit attempts.

Pairing secrets must not appear in URLs if avoidable.

## WebSocket And SSE

Streaming connections must:

- Authenticate before upgrade or subscription.
- Bound queued messages.
- Support cancellation and disconnect cleanup.
- Avoid leaking events across sessions.
- Apply backpressure or drop policy explicitly.
- Redact event payloads.

Runtime cancellation should be triggered when a client disconnects from an
interactive turn if the route contract says so.

## Static Files

Static file serving must:

- Serve only from configured asset roots.
- Deny path traversal.
- Avoid following unsafe symlinks.
- Set conservative content types.
- Avoid exposing local config, logs, or workspace files.

## Gateway Tests

Use fake runtime adapters.

Required cases:

- Unknown route.
- Method not allowed.
- Unauthenticated privileged route.
- Pairing success and failure.
- Replayed pairing token.
- Oversized request.
- Malformed JSON.
- Runtime failure.
- WebSocket disconnect cleanup.
- SSE subscription isolation.
- Static traversal denial.
