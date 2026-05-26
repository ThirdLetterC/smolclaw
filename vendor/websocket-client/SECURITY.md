# Security Model

Trust boundaries:
- All network bytes read from peer sockets are untrusted.
- DNS resolution and remote TLS endpoints are untrusted until handshake/certificate validation succeeds.

Attacker model:
- An attacker can send malformed websocket frames, fragmented control/data frames, and oversized lengths.
- An attacker can force handshake failures, unexpected opcodes, and connection churn.

Protected assets:
- Process memory safety and client control flow integrity.
- Socket/TLS state consistency and deterministic cleanup behavior.

Defensive posture:
- Frame parsing validates opcodes, lengths, continuation ordering, and bounded writes.
- Frame parsing rejects masked server frames, non-zero RSV bits, fragmented control frames, and invalid close payload lengths.
- Frame parsing fails closed: protocol violations or malformed reads immediately drop connection state.
- Inbound frame payloads are capped (`16'777'216` bytes) to limit parser abuse and resource exhaustion.
- Overflow-prone size math uses checked integer APIs from `<stdckdint.h>`.
- Secure randomness is sourced from `/dev/urandom`; failures are treated as hard errors.
- Handshake request construction validates host/path characters to prevent request-line/header injection.
- Build defaults include sanitizer support in debug and hardening flags for production-style builds.
