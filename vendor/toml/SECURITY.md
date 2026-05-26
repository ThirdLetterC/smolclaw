# Security Model

This repository is an in-process TOML v1.1 parser library written in C23. It
parses attacker-controlled text into a tree of `toml_datum_t` nodes and exposes
lookup helpers such as `toml_get()` and `toml_seek()`.

It is not a sandbox and it is not a policy engine. The library's job is to
parse TOML input and reject malformed documents. Callers remain responsible for
how parsed values are trusted, validated, and used.

## Trust Boundaries

- All TOML source text passed to `toml_parse()`, `toml_parse_file()`, or
  `toml_parse_file_ex()` is untrusted input.
- File paths passed to `toml_parse_file_ex()` are untrusted local input.
- Keys passed to `toml_get()` and `toml_seek()` are untrusted local input.
- Allocator hooks supplied through `toml_set_option()` are part of the trusted
  computing base.
- The parser runs in the caller's process and shares its address space,
  allocator, and file permissions.

## Protected Assets

- Process memory safety while scanning and parsing TOML text.
- Structural integrity of the parsed tree returned in `toml_result_t::toptab`.
- Predictable parse failure on malformed TOML instead of silent acceptance.
- Basic diagnostic information through `toml_result_t::errmsg`.

## Defensive Posture In This Codebase

- The public API returns parse status explicitly through `toml_result_t.ok`.
- Parse failures produce a bounded error string in `errmsg[200]`.
- Public parse and lookup entry points reject null pointers and negative
  buffer lengths with deterministic failure instead of undefined behavior.
- `toml_parse()` verifies that `src[len]` is the required terminating NUL byte
  before parsing.
- File parsing reads the file into memory, appends a terminating NUL byte, then
  reuses the same parser path as `toml_parse()`.
- Memory allocation failures are checked and reported as parse errors.
- Null allocator hooks passed through `toml_set_option()` are replaced with the
  default `realloc` and `free` hooks.
- Duplicate keys, malformed literals, malformed timestamps, unterminated
  strings, and invalid structural forms are rejected.
- Parser nesting depth is bounded: maximum bracket nesting for arrays is `30`
  and maximum brace nesting for inline tables is `30`.
- `toml_seek()` rejects multipart keys longer than `127` bytes.
- Optional full-input UTF-8 validation exists through
  `toml_option_t.check_utf8`, which defaults to `false`.
- The Zig build compiles with strict warnings:
  `-std=c23 -Wall -Wextra -Wpedantic -Werror`.
- Debug Zig builds enable `address`, `undefined`, and `leak` sanitizers in
  `build.zig`.

## Security-Relevant Limits And Defaults

- `toml_seek()` supports multipart keys up to `127` bytes.
- The parser allows up to `10` dotted key parts in internal key-part parsing.
- `toml_result_t.errmsg` is limited to `200` bytes including the terminator.
- The parser allocates an internal memory pool sized roughly to `len + 10`
  bytes, with additional dynamic allocations for tables and arrays.
- `toml_parse_file()` reads the entire input file into memory before parsing.
- Default global options from `toml_default_option()` are
  `check_utf8 = false`, `mem_realloc = realloc`, and `mem_free = free`.

These are implementation limits and defaults, not isolation boundaries.
Hostile inputs can still consume CPU and memory up to the process limits
available to the caller.

## Limitations And Caller Responsibilities

- The parser is not thread-safe with respect to global options.
  `toml_set_option()` mutates process-global state used by future parse calls.
- The returned tree is read-only by convention but not protected from caller
  misuse. Modifying internal pointers or freeing internals directly is out of
  contract.
- `toml_parse_file()` and `toml_parse_file_ex()` are not appropriate for
  unbounded files; they load the full file into memory first.
- Enabling `check_utf8` improves validation coverage but adds a full pre-scan of
  the input buffer.
- Successful parsing does not make values safe for application use. Callers must
  still validate semantic constraints such as path safety, numeric ranges,
  command allowlists, and feature flags.
- Secrets embedded in TOML are stored in process memory until `toml_free()` is
  called and allocator behavior decides when memory is reused. The library does
  not zeroize parsed values before release.
- Custom allocator hooks must follow the semantics expected by the library.
  Incorrect hooks can introduce memory safety bugs outside the parser's control.

## Examples And Development Artifacts

- `examples/simple.c` demonstrates normal API usage and should be treated as a
  usage example, not a hardened frontend.
- `examples/repro.c` is a local reproduction helper that uses `tmpfile()`; it
  is not part of the installed library interface.
- `testing/security_regression.c` is a local regression executable that covers
  malformed UTF-8, invalid Unicode escapes, and bounded lookup-key handling.
- There is currently no dedicated `tests/` directory or fuzzing harness in this
  repository.

## Verification

Current validation that exists in-tree:

- `zig build`
- `zig build run`
- `zig build security`
- `just build`
- `just run`
- `just security`

Additional validation that is reasonable for security-sensitive embedding:

- Run debug builds so the sanitizer flags in `build.zig` stay enabled.
- Add corpus-based fuzzing for `toml_parse()` and `toml_parse_file()`.
- Add regression tests for deeply nested input, large arrays/tables, invalid
  UTF-8, and malformed multiline strings.
- Exercise custom allocator hooks under ASan and UBSan.

## Deployment Guidance

- Treat every parsed value as untrusted until your application validates it.
- Prefer `toml_parse()` on already-bounded buffers when input size limits matter.
- Impose external file size limits before calling `toml_parse_file()` or
  `toml_parse_file_ex()`.
- If malformed UTF-8 must be rejected, enable `check_utf8` explicitly before
  parsing.
- Do not call `toml_set_option()` concurrently with parsing on other threads.
- Always release parse results with `toml_free()` exactly once.
