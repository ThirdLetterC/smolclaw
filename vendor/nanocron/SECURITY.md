# Security Model

`nanocron` is an in-process C23 cron scheduler library. It does not open
network sockets, parse files, or provide authentication, authorization, or
transport security. Its attack surface is the public API and any callback code
registered by the caller.

## Trust Boundaries

- All caller-provided inputs are untrusted:
  - schedule strings passed to `cron_add`
  - `struct timespec` values passed to `cron_execute_due`,
    `cron_execute_between`, and `cron_get_next_trigger`
  - timezone offsets passed to `cron_set_timezone_offset_minutes`
  - callback `user_data` pointers
- Registered callbacks are part of the trusted computing base.
- The process clock, libc time conversion routines, allocator, and toolchain are
  trusted dependencies.

## Attacker Model

An attacker controlling library inputs may attempt to:

- supply malformed cron expressions
- supply invalid nanosecond values in `struct timespec`
- trigger high callback volume by installing dense schedules
- force repeated schedule scans with `cron_get_next_trigger` or
  `cron_execute_between`
- misuse API lifetime rules, including removal or destruction during callback
  execution

This model assumes the attacker does not already have arbitrary code execution
inside trusted callbacks. If they do, the library cannot contain that code.

## Protected Assets

- process memory safety while parsing schedules and walking job lists
- scheduler state integrity for `cron_ctx_t`, job registration, and deferred
  destruction/removal
- predictable schedule evaluation semantics across UTC time and fixed timezone
  offsets
- bounded resource use during parsing and next-trigger search

## Defensive Posture

- Schedule parsing is strict:
  - exactly 7 whitespace-separated fields are required
  - maximum schedule length is `512` bytes excluding the trailing NUL
  - each field accepts at most `12` atoms
  - numeric ranges are validated against field-specific bounds
- Invalid inputs fail closed:
  - `cron_add` returns `nullptr` on parse failure, invalid arguments, or
    allocation failure
  - `cron_set_timezone_offset_minutes` returns `false` outside
    `[-1440, 1440]`
  - execution/query functions reject invalid `tv_nsec` values outside
    `[0, 999999999]`
- Integer-sensitive operations use checked arithmetic from `<stdckdint.h>` for:
  - timezone offset application
  - temporary schedule parse-buffer allocation sizing
  - next-trigger scan arithmetic
- Allocation behavior is simple and explicit:
  - contexts, jobs, and temporary schedule parse buffers are allocated with
    `calloc`
  - allocation failures are propagated as `nullptr` or `false`
- Callback reentrancy is intentionally guarded:
  - `cron_destroy()` during callback execution is deferred until the outermost
    execution scope unwinds
  - `cron_remove()` during callback execution marks jobs for deferred removal
  - last-fired timestamps are updated before invoking the callback to prevent
    reentrant double-fire for the same instant
- Time handling is explicit:
  - schedule matching is based on UTC plus a fixed per-context timezone offset
  - local timezone databases and DST rules are not interpreted by the library
- Search work is bounded:
  - `cron_get_next_trigger()` searches at most `366` days ahead
  - `cron_execute_between()` advances by repeated next-trigger discovery and
    stops when no future match exists, the upper bound is exceeded, or deferred
    destroy is requested

## Limitations And Caller Responsibilities

- The library is not thread-safe.
  - `cron_ctx_t` has no internal synchronization.
  - Concurrent access to the same context must be serialized externally.
- Callback code is fully trusted and can still introduce:
  - memory safety bugs
  - blocking behavior
  - reentrancy hazards outside the documented destroy/remove semantics
  - races if the application shares state across threads
- No availability controls are built in.
  - There is no rate limiting, admission control, or quota on the number of
    jobs in a context.
  - Very dense schedules or very large replay windows can cause CPU-heavy
    callback bursts.
- Timezone handling is fixed-offset only.
  - DST transitions, leap-second policy, and zone database lookups are caller
    concerns.
- Object lifetime rules remain strict.
  - A `cron_job_t *` becomes invalid after successful removal.
  - A `cron_ctx_t *` becomes invalid after `cron_destroy()` completes or, if
    destruction was requested from inside a callback, immediately after the
    outermost execution call returns.

## Hardening And Verification

The repository currently enforces:

- `-std=c23`
- `-Wall -Wextra -Wpedantic -Werror`
- `-fstack-protector-strong`
- `-D_FORTIFY_SOURCE=3`
- `-D_POSIX_C_SOURCE=200809L`
- `-fPIE`
- linker hardening on Linux: `-Wl,-z,relro,-z,now -pie`

Debug test builds add:

- `-fsanitize=address,undefined,leak`
- `-fno-omit-frame-pointer`

The codebase includes unit coverage for:

- invalid schedule rejection
- schedule length limit enforcement
- nanosecond precision and strict lower-bound behavior
- DOM/DOW matching semantics
- timezone offset validation and matching
- deferred job removal and deferred context destruction
- API null/invalid-input handling
- 366-day next-trigger lookahead bound behavior
- bounded catch-up execution with `cron_execute_between`

## Out Of Scope

`nanocron` does not attempt to provide:

- sandboxing for callbacks
- secret storage or zeroization
- privilege separation
- authentication or authorization
- persistence, audit logging, or secure remote control interfaces
