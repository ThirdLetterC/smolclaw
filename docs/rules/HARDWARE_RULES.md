# SmolClaw Hardware Rules

This file guides implementation of `src/hardware/` and `include/sc/peripheral.h`.
Hardware support covers USB discovery, serial devices, GPIO, board protocols,
and peripheral tools.

## Hardware Boundary

Hardware modules own:

- Device discovery.
- Device identity matching.
- Serial, USB, GPIO, or board protocol adapters.
- Command framing.
- Peripheral registry entries.
- Safe shutdown.

Hardware modules do not own:

- Runtime tool policy.
- Provider decisions.
- Channel or gateway authentication.
- General tool dispatch.

## Device Descriptor

Each peripheral descriptor should include:

- Stable name.
- Device class.
- Vendor and product identity where applicable.
- Capability flags.
- Safe discovery behavior.
- Config schema reference.
- Constructor and destroy callbacks.
- Manual test requirements.

## Discovery Rules

Discovery must:

- Be passive by default.
- Avoid destructive probes.
- Validate vendor, product, serial, path, or protocol identity.
- Handle devices disappearing at any time.
- Avoid claiming devices without explicit config when unsafe.
- Emit redacted observer events.

## Command Rules

Hardware commands require:

1. Device identity verified.
2. Capability allowed.
3. Emergency-stop state checked.
4. Command arguments validated.
5. Frame length and checksum validated.
6. Timeout and cancellation configured.
7. Receipt or audit event emitted.

Commands must fail closed when device identity is uncertain.

## Emergency Stop

Emergency stop behavior must:

- Be globally visible to hardware command paths.
- Block new destructive commands.
- Attempt safe shutdown where supported.
- Produce an audit event.
- Be testable with fake devices.

## Protocol Rules

Protocol parsers must:

- Validate frame length.
- Validate checksum or CRC when present.
- Validate command IDs.
- Reject unknown or oversized frames.
- Handle partial reads and timeouts.
- Have fuzz targets before exposed to external devices.

## Hardware Tool Rules

Hardware tools:

- Register through tool registry.
- Require hardware capability policy.
- Use peripheral registry.
- Never access devices directly from runtime.
- Produce receipts for side effects.
- Include safe dry-run or fake-device mode where possible.

## Hardware Tests

Required automated tests:

- Fake discovery.
- Identity mismatch.
- Device disappearance.
- Capability denial.
- Emergency-stop denial.
- Command timeout.
- Frame parser malformed input.
- Safe shutdown.

Manual hardware tests should be documented separately and never required for
normal CI.
