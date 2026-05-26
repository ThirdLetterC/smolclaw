# C23 Style Guide

This guide defines the coding rules for the repository root. It complements the
agent-facing style notes in the repository root.

## Goals

- Make ownership visible at every API boundary.
- Keep public ABI stable and dependency-free.
- Make error paths testable.
- Keep security-sensitive behavior easy to audit.
- Prefer clear C over clever macros.

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

Public headers live in `include/sc/`. Private headers are owned by the
subsystem source that uses them; in the current tree many live under
`include/<subsystem>/`, while local-only headers may live beside source files.
Public headers may include standard C headers and other `sc/*.h` headers, but
must not include third-party dependency headers.

## Naming

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

## Ownership Tags

Use these public ownership shapes:

| Type or pattern | Meaning |
|---|---|
| `sc_str` | Borrowed immutable string view, not necessarily null-terminated. |
| `sc_string` | Owned heap string allocated by a `sc_allocator`. |
| `sc_buf` | Borrowed byte span. |
| `sc_bytes` | Owned byte buffer allocated by a `sc_allocator`. |
| `const sc_* *` | Borrowed read-only object. |
| `sc_* *out` | Caller provides storage; callee initializes on success. |
| `sc_* **out` | Callee allocates and transfers ownership to caller. |

Every owning result needs a matching `sc_*_clear`, `sc_*_free`, or
`sc_*_destroy` function. `destroy` frees the object; `clear` releases contents
but not the containing storage.

## Function Shape

Prefer this shape for fallible functions:

```c
sc_status sc_module_do_work(sc_module *self,
                            const sc_request *request,
                            sc_response *out)
{
    sc_status status = sc_status_ok();
    sc_response tmp = {0};

    if (self == nullptr || request == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.module.invalid_argument");
    }

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

- Validate arguments before allocation or side effects.
- Initialize temporary owning values to zero.
- Use one cleanup path for complex functions.
- Transfer ownership to `out` only after all fallible work succeeds.
- Leave outputs untouched on failure unless the API documents otherwise.

## Return Status And Errors

Public fallible APIs return `sc_status` unless a narrower documented status enum
is more appropriate.

`sc_status` must distinguish success, invalid argument, no memory, I/O failure,
parse failure, security denial, unsupported feature, timeout, and cancellation.
Security denial must not be collapsed into generic I/O or invalid input.

Error keys are stable English identifiers such as
`sc.security.policy_denied`. Do not include dynamic data or secrets in keys.

## Nullability And Const

Pointer nullability must be documented in public contracts. Use `const` for
borrowed read-only input, vtables, descriptors, and immutable byte or string
views.

Out parameters are non-null unless explicitly documented otherwise. Optional
callbacks and optional vtable methods must be checked before dispatch.

## Thread-Safety Notes

Public headers and module docs must state whether an object is:

- Thread-safe.
- Reentrant but externally synchronized.
- Event-loop confined.
- Single-threaded.
- Valid only during a request or turn.

Do not add global mutable state without an explicit owner, initialization path,
and teardown path.

## Logging

Logs and tracing events stay in English and use stable keys. User-facing CLI,
gateway, or channel messages go through localization once the i18n layer
exists.

Never log API keys, authorization headers, unredacted config secrets, raw
private prompts, raw channel messages, full provider payloads, or private file
contents.

## Formatting

Use the project `clang-format` configuration once it exists. Until then:

- 4 spaces, no tabs.
- Function braces on the next line.
- Control-block braces on the same line.
- One declaration per line where ownership matters.
- No variable length arrays.
- No K&R declarations.
- No unsafe unbounded string functions.

## Tests

Code changes should include the smallest tests that protect the behavior:

- Unit tests for pure helpers and cleanup paths.
- Component tests for subsystem contracts.
- Fuzz targets for parsers that consume untrusted input.
- Sanitizer coverage for allocation, parser, and cleanup-heavy code.
- Denial-path tests before success-path tests for security code.

Tests must not require real credentials, private data, or real hardware unless
explicitly marked manual.
