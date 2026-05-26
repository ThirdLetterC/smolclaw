# Security Model

Trust boundaries:
- All inbound network bytes delivered via libuv (`on_uv_read` -> `jsonrpc_conn_feed`) are untrusted and attacker-controlled.
- JSON fields (`jsonrpc`, `id`, `method`, `params`) are untrusted until validated by protocol checks in `src/jsonrpc.c`.
- Application callbacks (`on_request`, `on_notification`, `on_open`, `on_close`) and transport hooks are part of the trusted computing base.
- Arena override hooks (`ARENA_MALLOC`, `ARENA_FREE`, `ARENA_MEMCPY`) and parser allocator hooks are trusted when overridden.

Attacker model:
- An attacker can open TCP connections and send arbitrary newline-delimited payloads, including malformed JSON and oversized messages.
- An attacker can send high-rate request streams to stress allocation paths, parse paths, and callback dispatch.
- An attacker can send JSON-RPC batches with mixed valid/invalid entries to exercise partial-success behavior.
- An attacker can trigger allocation-failure paths indirectly by causing memory pressure.

Protected assets:
- Process memory safety while parsing, buffering, serializing, and dispatching JSON-RPC messages.
- Connection state integrity (`closed`, inbound buffer length/capacity, transport lifecycle).
- Arena invariants (`region`, `index`, `size`) and per-message allocation cleanup behavior.
- Service availability under malformed or adversarial input within configured limits.

Defensive posture:
- Framing is strict newline-delimited JSON; blank lines are ignored.
- Request size limits are enforced:
  - Per-message limit: `MAX_MESSAGE_BYTES = 65'536` bytes after optional trailing `\r` trim.
  - Per-connection inbound buffer cap: `MAX_BUFFER_BYTES = 131'072` bytes.
- Oversized requests are fail-closed: server sends JSON-RPC `Invalid Request` (`-32600`) where possible and closes the connection.
- Input buffering is overflow-aware (`rpc_buffer_append` rejects growth beyond cap; reserve logic guards capacity growth).
- Request validation enforces JSON-RPC 2.0 shape:
  - `jsonrpc` must be exactly `"2.0"`.
  - `id` is accepted only as string, number, or null.
  - `params` is accepted only as array/object when present.
  - Empty batches are rejected as invalid request.
- Notification semantics are preserved: messages without `id` do not produce responses.
- Allocation and ownership behavior is explicit:
  - Connection objects and dynamic buffers use `calloc` and null checks.
  - Parson allocations are tagged (`JSONRPC_ALLOC_TAG`) and tracked as arena-backed vs heap-backed.
  - Heap-backed tagged allocations are released in `jsonrpc_arena_free`; arena-backed data is reclaimed on `arena_clear`.
- Arena lifecycle is bounded per processed message:
  - `jsonrpc_arena_scope_begin/end` restores prior global arena and clears connection arena after message handling.
  - Failed operations return explicit failure (`nullptr`/`false`) instead of partial-success semantics.
- Error handling avoids inconsistent partial state:
  - Failed send/build/parse paths clean up allocated JSON values.
  - Connection teardown frees protocol state, inbound buffers, and arena resources.
  - Connection teardown requested inside callbacks is deferred until callback unwind, preventing callback-triggered use-after-free during dispatch.
- TCP lifecycle guards:
  - Writes validate null/length/size bounds before allocation and `uv_write`.
  - Transport write/allocation failures are fail-closed and trigger connection close.
  - Read callbacks close the connection on libuv read errors.
  - `SIGPIPE` is ignored so peer disconnects do not terminate the process during writes.

Limitations and caller responsibilities:
- The library is not thread-safe:
  - Global allocator state (`g_current_arena`, `g_parson_allocator_initialized`) is process-global and unsynchronized.
  - Shared connection access must be externally synchronized.
- No transport security is provided:
  - TCP is plaintext; there is no TLS, authentication, authorization, or rate limiting.
- Availability protection is bounded:
  - Size limits reduce memory abuse but do not prevent CPU-level denial-of-service from many valid requests/connections.
- Callback safety is caller-owned:
  - Application callback logic can still introduce panics, memory misuse, blocking behavior, or data races.
- Arena behavior constraints:
  - `arena_init` creates non-owning arenas; `arena_create` creates owning arenas.
  - `arena_destroy` assumes ownership of `arena->region`; callers must not destroy non-owning arenas with external region memory.
  - `arena_copy` uses `ARENA_MEMCPY`; overlap handling depends on hook semantics.
- Secret-zeroization is not enabled by default; memory wiping for sensitive data is a caller concern.

Build hardening and verification posture:
- C compilation enforces strict warnings/errors and C23 mode:
  - `-std=c23 -Wall -Wextra -Wpedantic -Werror` (see `build.zig`).
- Debug sanitizer support is available via Zig build option:
  - `zig build -Dsanitizers=true` enables AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer for Debug builds.
- Valgrind workflow is provided (`zig build valgrind`, `just check-leaks`) for memory-leak and lifetime checks.
- Hardened non-Debug builds enable stack protector and fortified libc checks:
  - Compile-time flags include `-fstack-protector-strong`, `_FORTIFY_SOURCE=3`, and `-fPIE`.
- Hardened non-Debug linking enables PIE and RELRO behavior (`pie`, `link_z_relro`, non-lazy relocations).
