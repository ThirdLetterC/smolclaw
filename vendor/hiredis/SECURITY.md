# Security Model

Trust boundaries:
- All bytes read from Redis peer sockets are untrusted.
- Incoming RESP2/RESP3 type bytes, lengths, and payloads are untrusted until validated by the protocol parser.
- Transport is plain TCP or Unix domain sockets by default; TLS is optional and must be explicitly enabled.

Attacker model:
- An attacker can operate a malicious Redis endpoint or tamper with plaintext traffic on the network path.
- An attacker can send malformed RESP type bytes, malformed line endings, malformed integers/doubles/bools, and invalid bignum/simple string content.
- An attacker can send oversized bulk lengths, oversized aggregate lengths, deeply nested replies, partial frames, and connection churn.
- An attacker can trigger timeout/EOF/error paths to stress connection and parser state transitions.

Protected assets:
- Client process memory safety and control flow integrity.
- Parser and connection state consistency, including deterministic cleanup behavior after protocol/runtime errors.
- Service availability under malformed input, parser abuse attempts, and allocation pressure.

Defensive posture:
- The reader fails closed: protocol or allocation errors set error state, free partial reply objects, clear buffered input, and reset parser task stack.
- Reply parsing accepts only known RESP markers and rejects unknown type bytes immediately with protocol error semantics.
- Integer and size conversions use strict parsing and overflow checks before use (`string2ll`, `longLongToSize`).
- Bulk string parsing rejects out-of-range lengths and validates framed size arithmetic against `SIZE_MAX` before payload access/allocation.
- Aggregate parsing rejects out-of-range lengths, enforces bounded element counts (`REDIS_READER_MAX_ARRAY_ELEMENTS` defaults to `(1LL << 32) - 1`), and checks map/attribute element expansion for overflow.
- Nested parser task-stack growth validates `int`/`size_t` bounds before reallocating task vectors.
- Reader buffering is bounded by default (`REDIS_READER_MAX_BUF` = `16'384` bytes of unused buffer), and consumed data is compacted to prevent unbounded growth.
- SDS and allocation helpers include explicit overflow checks before allocation/reallocation; allocation failures are treated as hard errors.
- Allocator override setup tolerates partial/null override fields by retaining known-good defaults for missing function pointers.
- Command formatting paths check cumulative command sizing against `SIZE_MAX` before appending protocol frames.
- Command formatting rejects null format/argument pointers on `%s`/`%b` paths and validates argv pointers before `strlen`/copy operations.
- Socket I/O paths surface explicit error codes for timeout/EOF/I/O failures and transition contexts into error/closed states.
- Timeout conversion rejects negative `timeval` fields and checks poll deadline arithmetic for overflow.
- Errno string formatting clamps prefix lengths before appending error text to fixed-size buffers.
- TLS support is opt-in (`-Dssl=true`); TLS helpers default to peer verification (`REDIS_SSL_VERIFY_PEER`) unless callers explicitly relax verification mode.
- TLS read/write wrappers clamp I/O lengths to API-supported integer ranges and guard legacy lock-index bounds checks.

Build hardening:
- Build defaults enforce `-std=c23 -Wall -Wextra -Wpedantic -Werror` (plus strict prototype/string diagnostics).
- Debug and CI hardening should include `-fsanitize=address,undefined,leak`.
