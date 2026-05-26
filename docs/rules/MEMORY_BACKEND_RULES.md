# SmolClaw Memory Backend Rules

This file guides implementation of `src/memory/`. Memory backends store and
retrieve context for the agent runtime.

## Memory Boundary

Memory backends own:

- Persistence.
- Retrieval.
- Deletion and redaction.
- Import and export.
- Snapshotting.
- Backend-specific migrations.
- Embedding and vector metadata where enabled.

Memory backends do not own:

- Prompt policy.
- Provider selection.
- Tool execution.
- Channel identity policy.
- Secret storage outside approved secret helpers.

## Backend Descriptor

Each memory backend registry entry should include:

- Stable name.
- Config schema reference.
- Capabilities: store, retrieve, delete, redact, embeddings, snapshots.
- Persistence type: none, file, SQLite, remote DB, vector store.
- Constructor and destroy callbacks.
- Migration support.

## Record Rules

Memory records should include:

- Stable record ID.
- Source.
- Timestamp.
- Content or content reference.
- Metadata.
- Redaction state.
- Retention policy.

Rules:

- Bound content and metadata sizes.
- Validate UTF-8 where required.
- Avoid storing secrets by default.
- Preserve deletion and redaction semantics across backends.
- Keep provider-private prompt data out of memory unless configured.

## Retrieval Rules

Retrieval must:

- Bound query size.
- Bound result count.
- Bound total result bytes.
- Avoid returning deleted or redacted records.
- Avoid leaking backend-specific metadata into prompts.
- Track retrieval events with redacted observer fields.

## Embedding And Vector Rules

If embeddings are enabled:

- Validate vector dimensions.
- Validate model identity.
- Keep embedding model changes explicit.
- Bound batch sizes.
- Avoid embedding secrets unless explicitly configured.
- Keep vector index dependencies behind wrappers.

## Import And Export

Import/export must:

- Validate source format.
- Limit file size and record count.
- Redact secrets in exported fixtures unless user explicitly opts out.
- Preserve deletion markers if the format supports them.
- Avoid path traversal in archives.

## Memory Tests

Required cases:

- Store and retrieve.
- Delete and redaction.
- Reopen persistence.
- Corrupt store handling.
- Migration.
- Query limits.
- Import malformed data.
- Export redaction.
- Embedding dimension mismatch.
- Remote backend error translation if applicable.
