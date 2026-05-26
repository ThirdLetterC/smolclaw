# SmolClaw Provider Rules

This file guides implementation of `src/providers/` and the public provider
contract. Providers connect the runtime to LLMs, embedding services, local model
servers, and OpenAI-compatible APIs.

## Provider Boundary

Providers own:

- Provider-specific config interpretation.
- Credential lookup through secrets helpers.
- Request JSON construction.
- HTTP or local server calls through network wrappers.
- Streaming response parsing.
- Provider error translation.
- Capability metadata.

Providers do not own:

- Tool execution.
- Security approval for tool calls.
- Prompt policy.
- Channel or gateway state.
- Memory persistence.

## Provider Descriptor

Each provider registry entry should include:

- Stable name.
- Human-readable description key.
- Stability status.
- Supported modes: chat, stream, embeddings, tool calls, vision, audio.
- Config schema reference.
- Secret keys required.
- Constructor and destroy callbacks.
- Default timeout policy.

## Request Construction

Provider request construction must:

- Use bounded JSON helpers.
- Avoid raw string concatenation.
- Preserve role and content boundaries.
- Enforce model name length and character policy.
- Apply capability checks before adding optional fields.
- Avoid logging raw prompts or credentials.

Provider request objects should be request-scoped unless the API documents
longer ownership.

## Credentials

- Load credentials only through config and secrets helpers.
- Never store API keys in provider descriptors.
- Never log API keys, authorization headers, cookies, or signed URLs.
- Redact credentials in errors and receipts.
- Support missing credentials as explicit configuration errors.

## Network Policy

Remote providers must use shared network wrappers for:

- TLS validation.
- Proxy policy.
- Connect, read, write, and total timeouts.
- Redirect policy.
- Response body limits.
- Cancellation.
- Retry policy.
- Redacted logs.

OpenAI-compatible providers still use the same URL and SSRF policy as network
tools.

## Streaming Rules

Streaming parsers must:

- Tolerate arbitrary chunk boundaries.
- Bound partial buffer size.
- Validate event type and JSON types.
- Reject malformed tool calls.
- Preserve incremental text ordering.
- Clear partial state on failure.
- Handle cancellation between chunks.
- Emit observer events with redacted content.

Streaming parser tests should include truncated, split, duplicated, malformed,
oversized, and unknown-event inputs.

## Tool Calls

Providers may report tool calls, but runtime validates them.

Provider code should:

- Preserve tool-call IDs when present.
- Preserve raw argument JSON only as bounded data.
- Avoid invoking tools.
- Avoid assuming provider arguments match schema.
- Convert provider-specific tool-call formats into `sc_tool_call`.

## Retry And Fallback

Retry policy must distinguish:

- Transient network failures.
- Provider rate limits.
- Provider server errors.
- Invalid credentials.
- Security denials.
- Invalid requests.
- Cancellation.

Do not retry security denials, invalid credentials, invalid requests, or
cancellation as transient failures.

## Provider Tests

Use fixture responses and fake HTTP clients.

Required cases:

- Request JSON shape.
- Missing credentials.
- Provider HTTP error.
- Timeout.
- Cancellation.
- Streaming success.
- Streaming malformed chunk.
- Tool-call conversion.
- Redaction in logs.
- Retry classification.
- Capability-disabled feature rejection.
