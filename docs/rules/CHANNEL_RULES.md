# SmolClaw Channel Rules

This file guides implementation of `src/channels/` and channel adapters.
Channels connect external communication platforms to the runtime.

## Channel Boundary

Channels own:

- Platform authentication and webhook validation.
- Platform-specific message parsing.
- Message normalization.
- Outbound message formatting.
- Media handoff.
- Rate-limit and retry behavior for platform APIs.

Channels do not own:

- Tool execution.
- Runtime policy decisions.
- Provider selection.
- Memory storage policy.
- Gateway pairing.

## Channel Descriptor

Each channel registry entry should include:

- Stable name.
- Description localization key.
- Config schema reference.
- Required secret keys.
- Supported inbound event types.
- Supported outbound capabilities.
- Media capabilities.
- Constructor and destroy callbacks.

## Inbound Flow

Inbound events follow:

1. Read bounded request or platform event.
2. Validate signature, token, or source where supported.
3. Reject replay if timestamp or nonce exists.
4. Parse platform payload.
5. Normalize into `sc_channel_event`.
6. Apply channel-level rate limits.
7. Dispatch to runtime adapter.
8. Send outbound response if the platform requires it.

Raw platform payloads must not enter the runtime.

## Normalized Events

Normalized events should include:

- Channel name.
- Platform account or workspace identifier.
- Conversation identifier.
- Sender identifier.
- Message identifier.
- Text content as `sc_str` or `sc_string`.
- Attachment metadata.
- Reply context.
- Timestamp if trusted.

Do not put platform SDK types into public runtime contracts.

## Outbound Flow

Outbound messages must:

- Respect platform length limits.
- Handle rate limits and retryable errors.
- Redact private runtime metadata.
- Avoid sending internal error details to public chats.
- Preserve thread or reply context when available.

## Media Rules

Media is untrusted binary input.

Rules:

- Limit size before download.
- Validate declared and detected media type.
- Store temporary media in controlled locations.
- Scan or decode with sandboxing where practical.
- Avoid logging filenames or metadata that may contain private data.
- Clear temporary files after use.

## Channel Tests

Use fake platform payloads and fake runtime adapters.

Required cases:

- Valid inbound message.
- Invalid signature or token.
- Replay rejection where supported.
- Malformed payload.
- Oversized payload.
- Unsupported event type.
- Outbound formatting.
- Rate-limit handling.
- Runtime failure response.
- Media limit rejection.
