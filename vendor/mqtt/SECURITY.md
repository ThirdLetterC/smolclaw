# Security Model

This repository is a client-side MQTT 3.1.1 library implemented in C23. It is
not a broker, not a sandbox, not a TLS stack, and not an authorization layer.
Its job is to serialize MQTT requests, parse MQTT broker responses, and manage
client-side protocol state inside the caller's process.

Everything that comes from the network, from the broker, or from caller-supplied
topics and payloads must be treated as untrusted. The library enforces protocol
framing and some internal consistency checks, but application policy remains the
caller's responsibility.

## Trust Boundaries

- Inbound broker traffic processed by `__mqtt_recv()`, `mqtt_sync()`, and the
  unpacking helpers is untrusted.
- Topic names, payloads, credentials, keep-alive values, and all other inputs
  passed into the public API are untrusted until the application validates
  them.
- The core library performs socket I/O through the PAL in
  `include/mqtt/mqtt_pal.h` and `src/mqtt_pal.c`. The PAL, socket handle type,
  and any custom transport implementation are part of the trusted computing
  base.
- TLS configuration and certificate validation are outside the core MQTT
  library. The OpenSSL, mbedTLS, WolfSSL, and BearSSL paths live in the PAL and
  examples; their correctness depends on the caller's transport setup.
- User callbacks (`publish_response_callback`, `inspector_callback`,
  `reconnect_callback`) execute with the caller's privileges and are trusted.

## Protected Assets

- Memory safety while packing and unpacking MQTT control packets.
- Integrity of the client's internal message queue, packet ID tracking, and
  retransmission state.
- Correct handling of caller-owned send and receive buffers.
- Rejection of malformed fixed headers, invalid flags, invalid remaining
  lengths, malformed responses, and unexpected acknowledgements.

## Defensive Posture In This Codebase

- The parser validates MQTT control packet types and required flag patterns.
- Remaining length handling rejects values beyond the MQTT 3.1.1 limit of
  `268435455`.
- The client reports `MQTT_ERROR_RECV_BUFFER_TOO_SMALL` when the configured
  receive buffer can never fit the next complete packet.
- The send path reports `MQTT_ERROR_SEND_BUFFER_IS_FULL` when the configured
  egress queue cannot stage another message.
- The default PAL treats peer closure and hard socket/TLS errors as
  `MQTT_ERROR_SOCKET_ERROR` so callers can reconnect.
- Per-client mutexes protect internal state transitions during normal API use.
- Debug builds are wired for `-fsanitize=address,undefined,leak` in `justfile`
  and `build.zig`.

## Security-Relevant Limits And Defaults

- The library is MQTT 3.1.1, not MQTT 5.
- The core library does not provide encryption by itself. Plain TCP is the
  default unless the caller selects a TLS-capable PAL/socket handle.
- Default examples and integration tests target `test.mosquitto.org` on ports
  `1883` or `8883`.
- Send and receive storage is fully caller-owned. Buffer sizes directly affect
  reliability and denial-of-service tolerance.
- `mqtt_init()` expects a non-blocking connected socket.
- `sendbuf` should be suitably aligned for `struct mqtt_queued_message`
  accesses. Unaligned transmit buffers can fault on strict-alignment targets.

## Limitations And Caller Responsibilities

- Treat broker-provided topic names and payloads as attacker-controlled data.
  The library does not sanitize application payload contents.
- The core library does not implement hostname verification, certificate
  pinning, trust store management, secret storage, ACLs, or authorization
  checks. Those belong in the application and transport layer.
- `publish_response_callback` is invoked directly from the receive path while
  the client mutex is held. Keep it non-blocking and avoid re-entering the same
  client from that callback.
- If `publish_response_callback` is null, inbound `PUBLISH` packets are still
  parsed and protocol acknowledgements are generated, but the decoded publish
  is dropped after protocol handling.
- `inspector_callback` and `reconnect_callback` are also trusted code paths.
  Bugs in those callbacks can deadlock the client, corrupt application state,
  or weaken reconnect logic.
- `mqtt_sync()` and reconnect handling rely on specific lock sequencing. Sharing
  one `mqtt_client` across threads is supported only if callers respect the
  documented API model and do not bypass it with unsynchronized direct field
  access.
- The library does not persist session state across process restarts and does
  not rate-limit broker traffic. Hostile peers can still consume CPU, socket
  bandwidth, and caller-provided buffer space.
- Example programs are demonstrations, not hardened production deployments.
  They print received data, use public brokers, and inherit the transport and
  callback risks above.

## Verification

Validation confirmed in this checkout:

- `just test` builds `bin/tests` successfully. In this environment, the local
  serialization and utility tests passed, and the network-backed broker tests
  depend on outbound DNS/network access to `test.mosquitto.org`.
- `zig build tests` completes successfully as a build-only check for the test
  executable.

Additional validation paths present in the repository:

- `zig build test` builds and runs the test executable.
- `just build-examples` and `zig build examples` build the example clients.
- Setting `DEBUG=1` for `just` or `-Doptimize=Debug -Dsanitize=true` for Zig
  enables sanitizer-backed builds on supported platforms.

## Deployment Guidance

- Use TLS and explicit certificate verification in production. Do not treat the
  plain TCP examples as a secure default.
- Size `sendbuf` and `recvbuf` for the largest publish, subscription, and
  acknowledgement patterns your broker can produce.
- Provide a real `publish_response_callback` for any subscriber or any client
  that might receive publishes.
- Keep callbacks small, non-blocking, and free of recursive calls into the same
  client unless you have audited the locking implications.
- Reconnect logic should close and replace compromised sockets, rebuild
  subscriptions, and reapply authentication state explicitly.
- If the library will process hostile traffic, keep sanitizer coverage and
  protocol tests in CI and consider adding dedicated fuzzing for
  `mqtt_unpack_response()` and related parsers.

## Reporting

This repository does not currently publish a dedicated private security contact
or disclosure workflow in-tree. If you need coordinated disclosure, use an
existing maintainer contact channel if you have one; do not post exploit details
publicly before the maintainer has had a chance to assess them.
