# Security Model

This repository is a minimal RabbitMQ C23 client library for AMQP 0-9-1. It
opens TCP or TLS connections to a broker, negotiates AMQP connection
parameters, authenticates with SASL, parses inbound frames and tables, and
provides sample programs plus CLI tools for common broker operations.

It is not a complete security boundary by itself. The library can help with
memory-safe parsing within its implemented checks, but transport policy,
broker authorization, credential handling, and deployment isolation remain the
caller's responsibility.

## Trust Boundaries

- All data read from a broker is untrusted, including protocol headers, AMQP
  frames, field tables, message properties, and message bodies.
- URL strings passed to `amqp_parse_url()` are untrusted input.
- Hostnames, ports, vhosts, usernames, passwords, CA paths, certificate paths,
  and private key paths passed into the library or tools are untrusted local
  input until validated by the embedding application.
- Files read by the CLI tools, especially `--authfile`, are untrusted input.
- OpenSSL and the operating system socket stack are part of the trusted
  computing base when TLS is enabled.

## Attacker Model

- A malicious or compromised broker can send malformed frames, oversized field
  tables, invalid method sequences, truncated bodies, or unexpected connection
  shutdowns.
- A network attacker can observe or tamper with traffic when plaintext AMQP is
  used, or when TLS is enabled without proper certificate validation.
- A local user can recover credentials from command-line arguments, auth files,
  logs, or process memory if the application handles secrets carelessly.
- A peer can attempt denial of service through slow or stalled I/O, repeated
  connection failures, oversized negotiated settings, or message streams that
  keep application threads blocked.

## Protected Assets

- Process memory safety while decoding AMQP frames, tables, arrays, message
  properties, and message bodies.
- Integrity of connection state, channel state, frame queues, and socket
  lifecycle.
- Confidentiality of broker credentials and message data, subject to the
  limitations below.
- Availability of callers using timeouts, heartbeats, and bounded parsing
  correctly.

## Defensive Posture In This Codebase

- The Zig build uses strict compilation flags:
  `-std=c23 -Wall -Wextra -Wpedantic -Werror`.
- Hardening is enabled by default in `build.zig`:
  `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3` outside debug builds,
  `RELRO`, eager symbol binding, and `PIE` for Linux executables/tests.
- Optional sanitizer instrumentation is available with
  `zig build -Dsanitizers=true`.
- Allocation-heavy paths generally use `calloc()` and check for `nullptr`
  before use.
- Integer overflow checks are used in parser and allocation sizing paths via
  `<stdckdint.h>` helpers such as `ckd_add()` and `ckd_mul()`.
- Field-table and field-array decoding rejects nesting deeper than `100`
  levels.
- URL parsing rejects malformed percent-encoding, malformed IPv6 host syntax,
  and invalid port values.
- TLS sockets default to peer verification enabled, hostname verification
  enabled, and TLS versions restricted to TLS 1.2 through TLS 1.3.
- Socket polling uses `poll()` or `select()` with timeout-aware helpers rather
  than unbounded busy loops.
- On platforms that support it, socket writes suppress `SIGPIPE` via
  `MSG_NOSIGNAL` or `SO_NOSIGPIPE`.
- The default login handshake timeout is `12` seconds.

## Security-Relevant Limits And Defaults

- Default requested AMQP frame size: `131'072` bytes (`AMQP_DEFAULT_FRAME_SIZE`)
- Default requested channel limit: `2'047` (`AMQP_DEFAULT_MAX_CHANNELS`)
- Default heartbeat interval: `0` seconds (`AMQP_DEFAULT_HEARTBEAT`)
- Initial inbound socket buffer: `131'072` bytes
- Initial frame-pool page size: `65'536` bytes
- URL defaults:
  - `amqp://` -> `guest` / `guest`, host `localhost`, port `5672`, vhost `/`
  - `amqps://` -> `guest` / `guest`, host `localhost`, port `5671`, vhost `/`

These are defaults, not hard security boundaries. Negotiated AMQP settings can
still affect memory use and throughput.

## Limitations And Caller Responsibilities

- Plain TCP is supported and is the default unless the caller explicitly uses
  TLS. Credentials and message data are exposed to the network on plaintext
  connections.
- TLS is optional at build time (`-Dssl=true`) and at runtime. Callers that
  need transport security must create an SSL socket and configure trust
  anchors before connecting.
- `amqp_ssl_socket_new()` enables peer and hostname verification by default,
  but it does not automatically load trust roots. Call
  `amqp_ssl_socket_enable_default_verify_paths()` or
  `amqp_ssl_socket_set_cacert()` before connecting.
- The sample program [examples/amqp_ssl_connect.c](examples/amqp_ssl_connect.c)
  now loads system trust roots by default and keeps peer and hostname
  verification enabled unless the user explicitly passes `noverifypeer` or
  `noverifyhostname` for debugging.
- Only the `PLAIN` and `EXTERNAL` SASL methods are implemented.
- Heartbeat support is partial. Per the public API docs, heartbeats are
  serviced during publish and frame-wait paths, not as a general background
  liveness mechanism.
- RPC timeouts default to infinite. If stalled brokers are unacceptable, set
  handshake and RPC timeouts explicitly and design higher-level watchdogs.
- Connection state is not generally thread-safe. The library does not protect
  `amqp_connection_state_t` with per-connection locks. Use external
  synchronization or keep a connection owned by one thread at a time.
- Secret zeroization is limited. Usernames, passwords, message payloads, and
  key material may remain in process memory until overwritten or released by
  the allocator.
- CLI tools accept credentials through argv and auth files. That is convenient
  but not secret-safe against local inspection of process arguments or poorly
  protected files.
- The CLI helper in [tools/common.c](tools/common.c)
  defaults to `guest` / `guest`. It now rejects auth files that are readable or
  writable by group or other users, and it scrubs auth-file credentials after
  connection setup, but credentials supplied via argv or embedded in URLs still
  remain exposed to local inspection.
- The library validates frame structure and table encoding, but it does not
  make authorization decisions about exchanges, queues, routing keys, or
  message content. Broker ACLs and application validation remain out of scope.

## Examples And Tools

- `examples/` and `tools/` are operational references, not hardened frontends.
- `tools/` builds require `popt` and expose broker operations such as publish,
  consume, get, declare-queue, and delete-queue.
- The tools can read credentials from `--authfile`, but they do not scrub those
  buffers after use if the credentials came from argv or AMQP URLs.

Review example and tool defaults before using them in automation or
production-like environments.

## Testing And Verification

- Unit and integration-oriented test binaries are built with `zig build tests`.
- Current tests cover URL parsing, AMQP table parsing, SASL mechanism matching,
  status enum behavior, capability merging, and basic broker round-trips.
- The repository includes [compose.yml](compose.yml)
  for a local RabbitMQ test broker on `localhost:5672` with `guest` / `guest`.
- Development validation should include:
  - `zig build`
  - `zig build tests`
  - `zig build -Dsanitizers=true`
  - `zig build -Dssl=true` when TLS support is required

## Deployment Guidance

- Prefer TLS for any network that is not fully trusted.
- Load system CA paths or pin an explicit CA bundle before enabling broker
  certificate verification.
- Keep example defaults such as `guest` / `guest` out of production.
- Set explicit handshake, RPC, and application-level timeouts for long-lived
  connections.
- Restrict broker permissions by vhost, exchange, and queue according to the
  principle of least privilege.
- Treat every inbound message body and AMQP field as untrusted application
  input even after the library successfully parses it.
