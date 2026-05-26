# Security Model

Trust boundaries:
- All network bytes read from peer sockets are untrusted.
- Incoming HTTP upgrade requests and all WebSocket frames are untrusted until validated by the protocol parser.
- Transport is plain TCP in this project (no TLS termination in-process).

Attacker model:
- An attacker can send malformed upgrade requests, malformed WebSocket frames, unexpected opcodes, and connection churn.
- An attacker can send unmasked client frames, non-zero RSV bits, fragmented control/data frames, and oversized payload lengths.

Protected assets:
- Server process memory safety and control flow integrity.
- Connection state consistency and deterministic cleanup behavior across handshake/open/closing states.
- Service availability under malformed input and parser abuse attempts.

Defensive posture:
- Handshake parsing validates `GET ... HTTP/1.1`, `Upgrade: websocket`, `Connection: Upgrade`, `Sec-WebSocket-Version: 13`, and presence of `Sec-WebSocket-Key`.
- Handshake headers are bounded (`8'192` bytes); oversized or invalid requests get `400 Bad Request` and the connection is closed.
- Frame parsing validates opcodes, lengths, FIN/control rules, continuation ordering, and bounded writes.
- Frame parsing rejects unmasked client frames, non-zero RSV bits, fragmented control frames, unsupported continuation/data fragmentation, and invalid close payload lengths.
- Frame parsing fails closed: protocol violations or malformed reads immediately drop connection state.
- Inbound frame payloads are capped (`1'048'576` bytes) and control payloads are capped (`125` bytes) to limit resource exhaustion.
- Size arithmetic and buffer growth paths include explicit overflow checks before allocation and frame assembly.
- Allocation failures are treated as hard protocol/runtime errors and close with server error semantics.
- Build defaults enforce `-std=c23 -Wall -Wextra -Wpedantic -Werror`; debug builds support `-fsanitize=address,undefined,leak`.
