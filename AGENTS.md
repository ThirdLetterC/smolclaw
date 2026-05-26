# SmolClaw Agent Instructions

## 1. Role & Philosophy

**Role:** Expert Systems Programmer specializing in Strict C23.

**Objective:** Produce high-performance, type-safe, and self-documenting code that leverages C23 semantics to eliminate legacy undefined behaviors and boilerplate.

**Core Principles:**

* **Type Safety:** Leverage `auto` inference and `constexpr` to reduce type mismatches.
* **Explicit Intent:** Use standard attributes (`[[nodiscard]]`, `[[maybe_unused]]`) to communicate intent to both the compiler and the reader.
* **Modernity:** Reject C99/C11/C17 conventions where C23 offers a superior native alternative (e.g., `nullptr` vs `NULL`).
* **Zero-Warning Policy:** Code must compile with `-std=c23 -Wall -Wextra -Wpedantic -Werror`.

---

## 2. C23 Standards & Idioms

### Essential C23 Features
* **Booleans:** Use the built-in keywords `bool`, `true`, and `false`. **Do not** include `<stdbool.h>`.
* **Pointers:** Always use `nullptr` (type `nullptr_t`) instead of `NULL` or `0`.
* **Type Inference:** Use `auto` for variable declarations where the type is obvious from the initializer (e.g., `auto count = get_count();`).
* **Constants:** Use `constexpr` for compile-time constants instead of `#define` or `static const`.
* **Prototypes:** `void func()` implies no arguments (deprecated `func(void)` is no longer necessary, though permitted).
* **Literals:** Use digit separators for readability (e.g., `1'000'000`, `0xCAFE_BABE`). Use binary literals (`0b1010`) for bitmasks.

### Type System
* **Fixed Width:** Continue to use `<stdint.h>` (`int32_t`, `uint64_t`) over architecture-dependent `long` or `short`.
* **Bit-Precise:** Use `_BitInt(N)` for exact bit-width requirements (e.g., `unsigned _BitInt(24)`) if supported by the hardware/compiler context.
* **Generics:** Prefer `typeof` (or `typeof_unqual`) over complex `_Generic` macros for simple type propagation.

---

## 3. Memory & Resource Management

* **Ownership:** Document ownership in comments. Use `[[nodiscard]]` on functions that return an owning pointer.
* **Allocation:**
    * Prefer `calloc` over `malloc` to prevent uninitialized memory reads.
    * **Always** check against `nullptr` after allocation.
* **Sizing:** Use `size_t` for object sizes and `ptrdiff_t` for pointer arithmetic.
* **Cleanup:**
    * Use `goto` chains for centralized error handling/cleanup in complex functions.
    * Provide explicit destructor functions (e.g., `widget_destroy`).

---

## 4. Error Handling & Safety

* **Attributes:**
    * `[[nodiscard]]`: For functions returning errors or allocated resources.
    * `[[maybe_unused]]`: For variables utilized only in debug/asserts.
    * `[[fallthrough]]`: For intentional switch case fallthrough.
    * `[[unsequenced]]` / `[[reproducible]]`: Use for pure functions where applicable (optimization).
* **Return Values:** Return `bool` for success/failure or `enum` for specific error codes.
* **String Safety:** Strict ban on `strcpy`, `strcat`, `sprintf`. Use `snprintf` with bounds checking.

---

## 5. Build & Tooling

* **Standard Flag:** Ensure `-std=c23` or `-std=c2x` is set in CMake/Makefiles.
* **Static Analysis:** Suggest `clang-tidy` checks modernizing legacy casts or macros to C23 equivalents.
* **Sanitizers:** Always support `-fsanitize=address,undefined,leak` in debug configurations.

---

## 6. Example: C23 Pattern

When asked to write a function, follow this template using C23 keywords and attributes:

```c
#include <stdint.h>
#include <stdlib.h>
// <stdbool.h> NOT included; bool/true/false are keywords in C23

typedef struct {
    uint8_t *buffer;
    size_t length;
} packet_t;

/**
 * @brief Creates a new packet.
 * @return Owning pointer to packet or nullptr.
 */
[[nodiscard]] 
packet_t* packet_create(size_t size) {
    if (size == 0) {
        return nullptr;
    }

    // 'auto' inference reduces repetition
    auto *pkt = (packet_t*)calloc(1, sizeof(packet_t));
    if (pkt == nullptr) {
        return nullptr;
    }

    pkt->buffer = (uint8_t*)calloc(size, sizeof(uint8_t));
    if (pkt->buffer == nullptr) {
        free(pkt);
        return nullptr;
    }
    
    pkt->length = size;
    return pkt;
}

void packet_destroy(packet_t *pkt) {
    if (pkt == nullptr) {
        return;
    }
    free(pkt->buffer);
    free(pkt);
}

// Example usage
void usage_example() {
    constexpr size_t PKT_SIZE = 1'024; // C23 constexpr + digit separator
    
    packet_t *p = packet_create(PKT_SIZE);
    if (p) {
        // ... use packet
        packet_destroy(p);
    }
}

```

---

## 7. Anti-Patterns (STRICTLY FORBIDDEN)

1. **Legacy NULL:** Do not use `NULL`; use `nullptr`.
2. **Legacy Bool:** Do not include `<stdbool.h>`; use built-in keywords.
3. **K&R Declarations:** Do not use `func(void)` to denote empty arguments; use `func()`.
4. **Macro Constants:** Avoid `#define` for values; use `constexpr`.
5. **Implicit Fallthrough:** Switch cases falling through without `[[fallthrough]]` are forbidden.
6. **VLAs:** No Variable Length Arrays on the stack.

## 8. Project-Specific Conventions

* **File Naming:** Use lowercase with underscores (e.g., `modem_manager.c`).
* **Vendor Code:** Isolate third-party/vendor code in a separate directory and do not modify it directly.

---

## 9. Security & Hardening Best Practices

* **Threat Model First:** Document trust boundaries, attacker capabilities, and protected assets for each module.
* **Input Validation:** Treat all external data as untrusted; validate length, range, and encoding before parsing.
* **Bounds Safety:** Check every index and pointer offset against buffer sizes. No unchecked pointer arithmetic.
* **Checked Arithmetic:** Use C23 checked integer APIs (`<stdckdint.h>`, `ckd_add`, `ckd_sub`, `ckd_mul`) for size and offset math that may overflow.
* **Safe Buffer Operations:** Use `memcpy`/`memmove` only with verified bounds; keep explicit length tracking for every mutable buffer.
* **Initialization Discipline:** Zero-initialize state before use, and reset invalidated pointers to `nullptr` in long-lived objects.
* **Secrets Hygiene:** Never log secrets or key material. Clear secret buffers before release using a non-optimized wipe routine supported by the target platform.
* **Fail Securely:** On validation/auth failure, return an error and avoid partial state updates.
* **Toolchain Hardening:** Enable hardening flags where available (`-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3`, `-fPIE/-pie`, `-Wl,-z,relro,-z,now`).
* **Security Testing:** Combine sanitizers with fuzzing on parsers, decoders, and state transition logic; add regression tests for each discovered bug class.

---

## Style Goals

- Make ownership obvious.
- Make error paths testable.
- Keep public ABI stable.
- Keep security-sensitive behavior easy to audit.
- Keep generated code and handwritten code visually distinct.
- Prefer boring C over clever macros.

## File Layout

Use this order in C source files:

1. File comment for subsystem purpose when useful.
2. Matching public or private header.
3. Standard library includes.
4. SmolClaw public headers.
5. SmolClaw private headers.
6. Dependency wrapper headers.
7. Local constants and private types.
8. Private helper declarations.
9. Public functions.
10. Private helpers.

Example:

```c
#include "provider_openai.h"

#include <stddef.h>
#include <stdint.h>

#include "sc/provider.h"
#include "sc/status.h"

#include "core/sc_alloc_internal.h"
#include "json/sc_json_internal.h"
```

Do not include dependency headers directly outside their wrapper module.

## Header Rules

- Use `#pragma once` for project headers unless the build matrix rejects it.
- Public headers live under `include/sc/`.
- Private headers live beside their subsystem source files.
- Public headers include only standard C headers and other `sc/*.h` headers.
- Public headers must not include dependency headers.
- Keep public headers small and declarative.
- Prefer forward declarations and opaque types over exposed layouts.

## Naming

Use these naming patterns:

| Item | Pattern | Example |
|---|---|---|
| Public type | `sc_<noun>` | `sc_provider` |
| Public enum | `sc_<noun>` | `sc_status_code` |
| Public enum value | `SC_<CATEGORY>_<NAME>` | `SC_ERR_NO_MEMORY` |
| Public function | `sc_<module>_<verb>` | `sc_provider_chat` |
| Private type | `<module>_<noun>` | `openai_provider` |
| Private function | `<module>_<verb>` | `openai_parse_chunk` |
| Macro | `SC_<NAME>` | `SC_ARRAY_LEN` |
| File | lowercase with underscores | `provider_registry.c` |

Avoid abbreviations unless they are already project vocabulary, such as `json`,
`http`, `url`, `abi`, `sop`, `otp`, or `id`.

## Function Shape

Prefer this shape:

```c
sc_status sc_module_do_work(sc_module *self,
                            const sc_request *request,
                            sc_response *out)
{
    sc_status status = sc_status_ok();

    if (self == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.module.invalid_argument");
    }

    sc_response tmp = {0};

    status = module_build_response(self, request, &tmp);
    if (!sc_status_is_ok(status)) {
        goto cleanup;
    }

    *out = tmp;
    tmp = (sc_response){0};

cleanup:
    sc_response_clear(&tmp);
    return status;
}
```

Rules:

- Validate arguments before side effects.
- Initialize temporary owning values to zero.
- Use one cleanup path for complex functions.
- Transfer ownership by assigning to `out` and clearing the temporary owner.
- Leave outputs untouched on failure unless the API documents otherwise.

## Formatting

- Use the project `clang-format` configuration once it exists.
- Until then, use 4 spaces, no tabs.
- Put braces on the next line for functions.
- Put braces on the same line for control blocks.
- Keep one declaration per line when ownership matters.
- Prefer early validation returns before allocation or side effects.
- Keep functions short enough to review without scrolling through multiple
  unrelated phases.

## Comments

Write comments for:

- Ownership transfer.
- Security checks.
- ABI constraints.
- Non-obvious cleanup.
- Dependency quirks.
- Generated-code markers.

Do not comment obvious assignments or restate function names.

Good:

```c
/* The provider stream owns partial_json until the next successful parse. */
stream->partial_json = next_partial;
```

Avoid:

```c
/* Set count to zero. */
count = 0;
```

## Macros

Macros are allowed for:

- Header guards if needed.
- Compile-time feature probes.
- Symbol visibility.
- Array length helpers.
- Container-of style helpers only after review.
- Test assertions.

Avoid macros for:

- Control flow.
- Hidden allocation.
- Hidden cleanup.
- Type-erased public API behavior.
- Constants that can be `enum` values or `static const` values.

## Constants

- Use `enum` constants for integral constants that must be usable in array
  sizes or switch labels.
- Use `static const` for typed private constants.
- Use C23 `constexpr` only after compiler support is confirmed in the feature
  matrix.
- Do not put mutable global configuration in constants or macros.

## Error Style

- Return `sc_status` for fallible public functions.
- Use stable `error_key` values.
- Do not call `exit`, `abort`, or `assert` in library error paths.
- `assert` is acceptable for internal impossible states in tests and debug-only
  checks, not for validating external input.
- Keep security denial distinguishable from invalid input and I/O failure.

## Include Hygiene

- Every `.c` file includes its matching header first when one exists.
- Headers must compile standalone.
- Do not rely on include order side effects.
- Use forward declarations to reduce header coupling.
- Private headers must not be included by modules that do not own their private
  state.

## Generated Code

Generated files must include:

- Source generator name.
- Source input file.
- Regeneration command.
- Notice that manual edits will be overwritten.

Generated code should still compile cleanly with warnings as errors.

## Forbidden Patterns

- Raw dependency headers in public API.
- Unbounded string functions.
- Direct production use of `malloc`, `calloc`, `realloc`, or `free`.
- Returning stack, temporary, or arena-reset storage from long-lived APIs.
- Global mutable state without an explicit owner and teardown path.
- Security checks hidden behind vague helper names.
- Silent fallthrough in `switch`.
- Ignoring return values from allocation, file, socket, process, or parser APIs.

---

## Error Goals

- Public fallible APIs report structured status.
- Security denial remains distinguishable.
- Error keys are stable for logs, tests, and localization.
- Partial outputs are handled consistently.
- Cleanup can run safely after any failure.

## Status Type

Public fallible APIs return `sc_status`:

```c
typedef enum sc_status_code {
    SC_OK = 0,
    SC_ERR_INVALID_ARGUMENT,
    SC_ERR_NO_MEMORY,
    SC_ERR_IO,
    SC_ERR_PARSE,
    SC_ERR_HTTP,
    SC_ERR_SECURITY_DENIED,
    SC_ERR_UNSUPPORTED,
    SC_ERR_TIMEOUT,
    SC_ERR_CANCELLED
} sc_status_code;
```

`sc_status` should include:

- Machine-readable status code.
- Stable `error_key`.
- Optional diagnostic message.
- Optional nested cause where useful.

Keep user-facing localization outside low-level C error creation.

## Error Keys

`error_key` values are stable English identifiers:

```text
sc.config.invalid_toml
sc.security.policy_denied
sc.tool.file.outside_workspace
sc.provider.openai.stream_parse_failed
sc.gateway.request_too_large
```

Rules:

- Do not include dynamic data in keys.
- Do not include secrets in keys or messages.
- Use keys in tests rather than fragile full messages.
- Keep keys stable unless behavior intentionally changes.

## Return Rules

- Return `SC_OK` only when the requested operation completed.
- Return `SC_ERR_INVALID_ARGUMENT` for programmer or caller contract violations.
- Return `SC_ERR_SECURITY_DENIED` for policy denials.
- Return `SC_ERR_UNSUPPORTED` for disabled features or unavailable backends.
- Return `SC_ERR_TIMEOUT` when a deadline expires.
- Return `SC_ERR_CANCELLED` when caller cancellation wins.
- Return `SC_ERR_PARSE` for malformed JSON, TOML, URLs, provider chunks,
  webhooks, plugin metadata, SOP, or firmware frames.
- Return `SC_ERR_IO` for filesystem, socket, process, or device failures that
  are not better represented by a more specific code.

Do not collapse all errors into `SC_ERR_IO`.

## Output Rules

Public APIs must document one of these behaviors:

- Output is initialized only on success.
- Output is always initialized and must always be cleared.
- Output may be partially initialized and is destroy-safe.

Prefer "initialized only on success" for public APIs.

Implementation pattern:

```c
sc_response tmp = {0};

status = build_response(&tmp);
if (!sc_status_is_ok(status)) {
    goto cleanup;
}

*out = tmp;
tmp = (sc_response){0};

cleanup:
sc_response_clear(&tmp);
return status;
```

## Security Denials

Security denials must include:

- `SC_ERR_SECURITY_DENIED`.
- Stable denial key.
- Policy or capability that denied the request.
- Redacted audit event.
- Receipt for side-effecting tool attempts where appropriate.

Do not retry security denials as if they were transient network failures.

## Cancellation And Timeout

Timeout and cancellation are separate:

- Timeout means the operation exceeded a configured deadline.
- Cancellation means the caller asked the operation to stop.

Code should check cancellation at safe points:

- Before network requests.
- Between provider stream chunks.
- Before tool dispatch.
- Before filesystem mutation.
- Before hardware command dispatch.
- During long memory retrieval or embedding work.

Cleanup after cancellation must still release resources and emit required audit
events.

## Logging Errors

Logs should include:

- Stable `error_key`.
- Status code.
- Subsystem.
- Operation name.
- Redacted resource identifier when useful.

Logs must not include:

- API keys.
- Authorization headers.
- Raw prompts.
- Raw channel messages.
- Full provider payloads.
- Private file contents.

## Localization Boundary

- User-facing CLI, gateway, or channel messages use localization catalogs.
- Internal `sc_status` keys and logs stay in English.
- Panic messages and tracing events stay in English.
- Translators should see message templates, not raw secrets or private context.

## Dependency Error Translation

Wrappers translate dependency errors into `sc_status`:

- Preserve useful categories.
- Redact dependency messages that may include secrets or raw input.
- Avoid exposing dependency-specific numeric errors in public API.
- Keep the original error available only in sanitized debug context when needed.

## Test Requirements

Every subsystem should test:

- Invalid argument errors.
- Allocation failure where practical.
- Parse failure.
- Security denial.
- Timeout or cancellation if the subsystem can block.
- Cleanup after failure.
- Stable `error_key` for expected errors.

## Review Checklist

- Fallible public functions return `sc_status`.
- `out` behavior is documented.
- Security denials are not generic failures.
- Error messages are redacted.
- Error keys are stable and tested.
- Dependency errors are translated at wrapper boundaries.
- Cleanup is safe after every error path.

---

## Testing Goals

- Catch memory-safety defects early.
- Protect public contracts.
- Verify security denials before success paths.
- Keep tests deterministic without real credentials.
- Exercise parser and network boundaries with adversarial input.
- Make generated docs and ABI drift visible.

## Test Tiers

| Tier | Purpose |
|---|---|
| Unit | Small functions and containers. |
| Component | One subsystem behind its public contract. |
| Integration | Multiple subsystems through runtime, CLI, gateway, channel, or memory flows. |
| Fuzz | Parsers and protocol decoders. |
| Sanitizer | Memory, undefined behavior, leaks, and selected threading. |
| ABI | Public headers, struct sizes, symbols, plugins. |
| Generated docs | CLI, config, registry, and localization drift. |

Use the smallest tier that proves the behavior, then add broader tests when a
boundary is crossed.

## Expected Commands

Initial command shape:

```sh
cmake -S . -B build/c23 -DCMAKE_BUILD_TYPE=Debug
cmake --build build/c23
ctest --test-dir build/c23 --output-on-failure
```

Sanitizer command shape:

```sh
cmake -S . -B build/asan -DSC_SANITIZERS=ON
cmake --build build/asan
ctest --test-dir build/asan --output-on-failure
```

The exact command may change after the build system lands. Keep this document
updated with the current commands.

## Unit Tests

Unit tests should cover:

- `sc_status`.
- Allocator wrappers.
- Strings and buffers.
- Vectors, maps, and registries.
- Size-overflow helpers.
- JSON and TOML wrapper helpers.
- URL parsing wrappers.
- Path boundary helpers.
- Redaction helpers.
- Error-key creation.

Unit tests should not require network, real credentials, real hardware, or a
provider account.

## Component Tests

Component tests should use fake dependencies:

- Provider tests use fake HTTP or fixture responses.
- Tool tests use temporary workspaces and fake policy engines.
- Gateway tests use fake runtime adapters.
- Channel tests use fake platform payloads.
- Memory tests use temporary databases or in-memory backends.
- Hardware tests use fake devices.
- Plugin tests use small test plugins with known ABI descriptors.

Each component test should include success, invalid input, denial or disabled
feature, cleanup, and boundary-size cases.

## Integration Tests

Integration tests should cover:

- CLI message to runtime with mock provider.
- Runtime provider tool-call loop with fake tools.
- Gateway WebSocket or HTTP request to fake runtime.
- Channel inbound message to runtime adapter.
- Memory persistence across process restart.
- Provider fallback through router with fixture errors.

Do not put real provider credentials in CI. Manual credential tests must be
separate and documented.

## Fuzz Tests

Add fuzz targets before exposing parsers to untrusted input:

- JSON wrapper.
- TOML config parser.
- URL parser and SSRF policy.
- Tool-call parser.
- Webhook parsers.
- Provider streaming parser.
- SOP parser.
- Plugin metadata parser.
- Firmware protocol framing.

Fuzz targets should:

- Bound input size.
- Avoid external network and filesystem side effects.
- Run in short smoke mode in normal CI.
- Run longer in scheduled CI.
- Save minimized regression fixtures.

## Sanitizers

Run:

- ASan for memory safety.
- UBSan for undefined behavior.
- Leak sanitizer where supported.
- TSan for selected event-loop, registry, and callback tests when practical.

Sanitizer failures are bugs unless documented as toolchain false positives with
issue references.

## Static Analysis

Use:

- Compiler warnings as errors.
- `clang-tidy` with CERT-oriented checks.
- `cppcheck`.
- Public header compile checks.
- Optional compiler static analyzer runs.

Do not suppress findings in production code without a comment and issue
reference.

## Security Tests

Security-sensitive tests should prove:

- Default denial for side-effecting tools.
- Workspace escape attempts fail.
- Symlink and path race cases are handled or denied.
- SSRF targets are denied.
- Gateway privileged routes require auth or pairing.
- Channel signatures or tokens are checked.
- Plugin capabilities are denied unless granted.
- Hardware emergency stop blocks commands.
- Receipts redact secrets.

Always test denial before success for security code.

## Fixture Rules

- Use neutral placeholders.
- Do not include real names, tokens, prompts, channel messages, credentials, or
  customer data.
- Keep provider payload fixtures minimal and redacted.
- Mark generated fixtures and their source.
- Store malicious parser fixtures with names that describe the behavior.

## Test Naming

Use descriptive names:

```text
tool_file_write_denies_path_outside_workspace
provider_openai_stream_rejects_truncated_json
gateway_pairing_rejects_replayed_token
plugin_loader_rejects_smaller_abi_struct
```

Names should state the behavior being protected.

## Coverage Expectations

Coverage is useful but not sufficient. Focus on:

- Error paths.
- Allocation failure.
- Security denials.
- Parser malformed input.
- Boundary sizes.
- Cancellation and timeout.
- Cleanup and teardown.
- ABI version mismatches.

## Review Checklist

- Tests do not require real credentials.
- Tests do not require real hardware unless marked manual.
- Denial paths are tested.
- Failure cleanup is tested.
- Fuzz target exists for new parser exposure.
- Sanitizers pass.
- Generated docs and ABI checks run when relevant.

---

## Ownership Vocabulary

| Type | Ownership |
|---|---|
| `sc_str` | Borrowed immutable string view. Not necessarily null-terminated. |
| `sc_string` | Owned heap string allocated by a `sc_allocator`. |
| `sc_buf` | Borrowed byte span. |
| `sc_bytes` | Owned byte buffer allocated by a `sc_allocator`. |
| `const sc_* *` | Borrowed read-only object. |
| `sc_* *out` | Caller provides storage; callee initializes on success. |
| `sc_* **out` | Callee allocates and transfers ownership to caller. |

Never use a raw `char *` in a public API when length or ownership matters.

## Allocator Model

SmolClaw code uses three allocation classes:

- Long-lived allocations through `sc_allocator`.
- Request-scoped temporary allocations through arenas.
- Dependency-internal allocations isolated inside wrappers.

Production modules do not call `malloc`, `calloc`, `realloc`, or `free`
directly. The only exceptions are allocator implementation files and tightly
reviewed dependency wrappers.

## Borrowed Strings

`sc_str` is a view:

```c
typedef struct sc_str {
    const char *ptr;
    size_t len;
} sc_str;
```

Rules:

- It may not be null-terminated.
- It does not own memory.
- It must not outlive the referenced storage.
- It may point into config, request, arena, or parser-owned memory.
- Convert to `sc_string` before storing long term.

## Owned Strings

`sc_string` owns memory:

```c
typedef struct sc_string {
    char *ptr;
    size_t len;
    sc_allocator *alloc;
} sc_string;
```

Rules:

- Initialize with zero before use.
- Clear with `sc_string_clear`.
- Keep allocator identity with the string.
- Guarantee null termination only if the type contract states it.
- Use secure clearing for secrets.

## Output Parameters

For caller-provided outputs:

```c
sc_status sc_provider_chat(sc_provider *provider,
                           const sc_chat_request *request,
                           sc_chat_response *out);
```

Rules:

- Validate `out != nullptr`.
- Leave `out` untouched on failure unless documented.
- Use a temporary local owner.
- Transfer to `out` only after all fallible operations succeed.

For callee-allocated outputs:

```c
sc_status sc_config_load(sc_allocator *alloc,
                         sc_str path,
                         sc_config **out);
```

Rules:

- Validate `out != nullptr`.
- Set `*out` only on success.
- The matching destroy function owns cleanup.

## Cleanup Pattern

Use one cleanup path for complex functions:

```c
sc_status build_message(sc_allocator *alloc, sc_str input, sc_string *out)
{
    sc_status status = sc_status_ok();
    sc_string tmp = {0};

    if (alloc == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.memory.invalid_argument");
    }

    status = sc_string_from_str(alloc, input, &tmp);
    if (!sc_status_is_ok(status)) {
        goto cleanup;
    }

    *out = tmp;
    tmp = (sc_string){0};

cleanup:
    sc_string_clear(&tmp);
    return status;
}
```

This pattern prevents leaks and avoids partially initialized outputs.

## Arenas

Use arenas for:

- Prompt assembly.
- Provider request JSON.
- Provider streaming scratch state.
- Tool argument normalization.
- Gateway request parsing.
- Temporary config merge data.

Rules:

- Arena storage must not escape the request or turn.
- Do not store arena-backed pointers in long-lived runtime state.
- Do not return arena-backed data through owning output APIs.
- Reset arenas at well-defined lifecycle points.
- Use tests to catch accidental long-lived arena references.

## Sensitive Memory

Sensitive values include:

- Provider API keys.
- Channel tokens.
- Gateway pairing secrets.
- OTP material.
- Private prompts.
- Secret config values.
- Authorization headers.

Rules:

- Store secrets in `sc_secret` or a similarly explicit wrapper.
- Do not copy secrets unless necessary.
- Redact before logging or receipts.
- Zero memory before free.
- Avoid putting secrets in arenas that are dumped or reused without zeroing.
- Avoid including secrets in crash reports, metrics labels, generated docs, or
  test fixtures.

## Reallocation

When growing buffers:

- Check multiplication and addition overflow.
- Keep the old pointer until growth succeeds.
- Preserve allocator identity.
- Define behavior for zero capacity.
- Bound maximum capacity for attacker-controlled input.

Avoid raw `realloc` patterns in production modules.

## Dependency Ownership

Dependency wrappers must translate ownership:

- Dependency-owned parse trees are cleared inside the wrapper.
- Dependency strings are copied into `sc_string` before crossing boundaries.
- Dependency errors become `sc_status`.
- Dependency allocators do not leak into public ABI.
- Caller never frees dependency-owned memory directly.

## Destroy Functions

Destroy functions:

- Accept `nullptr`.
- Tolerate partially initialized objects.
- Clear fields in reverse ownership order.
- Unregister callbacks, timers, and event subscriptions.
- Close file descriptors, sockets, handles, and dynamic modules.
- Zero secrets before releasing storage.

`destroy` frees the object. `clear` releases contents but not the object itself.

## Ownership Review Checklist

Before finishing code:

- Every pointer has documented ownership.
- Every owning value has one cleanup owner.
- Every failure path releases temporary owners.
- No borrowed view outlives its source.
- No arena pointer escapes its arena.
- Size arithmetic is checked before allocation.
- Secrets are zeroed and redacted.
- Sanitizers run cleanly.

---


## Primary Constraint

Preserve the SmolClaw architecture while writing idiomatic C. The goal is a
modular C23 runtime with stable contracts, explicit ownership, and auditable
side effects.

## Required Reading

Before writing code, read the relevant scope:

- [Modular architecture map](./docs/architecture-map.md)
- [Module map](./docs/modules.md)
- [Security model](./SECURITY.md)
- [Dependency inventory](./docs/dependency-inventory.md)

## Module Boundaries

- Public contracts live only in `include/sc/*.h`.
- Implementation details live under `src/<subsystem>/`.
- Public headers expose opaque handles and value structs only.
- Concrete provider, channel, tool, memory, observer, runtime, sandbox,
  peripheral, and plugin structs stay private.
- Subsystems communicate through public contracts or narrow private internal
  headers, not through direct access to another subsystem's state.
- Registries own module descriptors. Runtime code should ask registries for
  providers, tools, channels, memory backends, observers, and peripherals.

## ABI Constraints

- Treat every public header change as ABI-sensitive.
- Use size or version fields for ABI structs that can evolve.
- Keep public structs append-only unless a version break is intentional.
- Do not expose third-party dependency types in public headers.
- Keep public enum values stable once plugins or external modules can depend on
  them.
- Public functions return `sc_status` or a documented small status enum.
- Public functions do not return raw negative error codes.
- Public headers should compile as C23 and, where reasonable, as C++ consumers.

## Ownership Constraints

- Every pointer in a public API must have documented ownership.
- Use `sc_str` for borrowed immutable string views.
- Use `sc_string` for owned strings allocated by `sc_allocator`.
- Use `sc_buf` for borrowed byte spans.
- Use `sc_bytes` for owned byte buffers.
- Use `const sc_* *` for borrowed read-only objects.
- Use `sc_* *out` when the caller provides storage.
- Use `sc_* **out` when the callee allocates and transfers ownership.
- Owning return values must have matching clear, free, or destroy functions.
- Destroy functions must accept `nullptr` and partially initialized objects.
- Arena-backed data must not escape the arena lifetime.

## Allocation Constraints

- Production modules do not call `malloc`, `calloc`, `realloc`, or `free`
  directly.
- Use `sc_allocator` for long-lived allocations.
- Use request or turn arenas for temporary provider, prompt, JSON, and tool
  data.
- Check every allocation result.
- Check size arithmetic before allocation.
- Zero sensitive buffers before release.
- Keep cleanup paths centralized and test error paths.

## C23 Constraints

- Use the project compiler feature matrix before relying on a C23 feature.
- Use `nullptr`, `bool`, `true`, and `false` only where configured compiler
  support confirms them.
- Do not treat `auto` as C++-style type inference.
- Avoid VLAs.
- Avoid compiler extensions in public headers.
- Use fixed-width integer types where width matters.
- Use `size_t` for sizes and `ptrdiff_t` for pointer differences.
- Use `snprintf` or bounded buffer helpers instead of unsafe string functions.
- Do not use `strcpy`, `strcat`, `sprintf`, `gets`, or unbounded `scanf`
  formats.

## Security Constraints

- Side-effecting tools are denied by default.
- Shell, file write, browser, HTTP, SaaS, hardware, and plugin actions require
  policy checks before execution.
- Tool results and side effects require receipts.
- Gateway and channel input must be authenticated, size-limited, and validated
  before reaching the runtime.
- URL parsing, proxy rules, redirects, DNS, timeouts, and SSRF controls are
  shared across providers and network tools.
- File tools must enforce workspace boundaries after canonicalization and must
  account for symlink and time-of-check/time-of-use races.
- Secrets never appear in logs, metrics labels, receipts, generated docs,
  examples, or fixtures.
- Hardware actions require device identity checks and emergency-stop state.
- Plugins require ABI version checks and capability checks before callback
  dispatch.

## Error-Handling Constraints

- Return explicit errors for invalid input, denied policy, unsupported features,
  allocation failure, I/O failure, malformed data, timeout, and cancellation.
- Do not collapse security denials into generic I/O failures.
- Preserve stable `error_key` values for logs and localization.
- Do not panic, abort, or call `exit` from library code.
- Do not leave partially initialized output values on failure unless the API
  documents a destroy-safe partial state.

## Concurrency Constraints

- Document thread ownership for each object.
- Shared mutable objects require explicit synchronization or single-thread event
  loop confinement.
- Callback reentrancy must be documented.
- Cancellation must be safe when provider, tool, channel, gateway, and hardware
  operations are in flight.
- Timer and event subscriptions must be unregistered during teardown.

## Localization And Privacy Constraints

- User-facing strings must be routed through the localization layer.
- Logs, tracing events, panic messages, and internal diagnostics stay in English
  and carry stable non-secret keys.
- Examples, fixtures, docs, and tests must use neutral placeholders.
- Do not commit real personal data, tokens, prompts, channel messages, or
  customer documents.

## Validation Constraints

Code is not complete until it has a feedback loop:

- Unit tests for local behavior.
- Component tests for subsystem contracts.
- Integration tests for CLI, runtime, gateway, channel, memory, or provider
  flows when behavior crosses a boundary.
- Fuzz tests for parsers that consume external input.
- Sanitizer and static-analysis runs for C code.
- Documentation updates for public headers, dependency choices, config,
  generated artifacts, or security behavior.

## Forbidden Shortcuts

- Do not bypass `src/security/` from a tool, provider, channel, gateway, plugin,
  or hardware module.
- Do not add global mutable runtime state for convenience.
- Do not expose dependency-specific types in `include/sc/*.h`.
- Do not use raw `char *` where length-aware `sc_str`, `sc_string`, `sc_buf`, or
  `sc_bytes` is required.
- Do not assume provider output is trustworthy.
- Do not assume local workspace files are trustworthy.
- Do not silently weaken security policy to make tests pass.
- Do not add feature flags, config keys, or plugin capabilities without a
  concrete consumer and tests.
