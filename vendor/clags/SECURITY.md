# Security Model

This repository is an in-process command-line argument parser written in C23.
It parses attacker-controlled `argv` strings into caller-owned variables,
supports typed conversion, list accumulation, choice matching, and recursive
subcommands, and prints usage/help text from caller-supplied configuration
metadata.

`clags` is not a sandbox, not an authorization engine, and not a replacement
for application-level validation. Its job is to parse syntax and enforce basic
type constraints inside the caller's process. Callers remain responsible for
trust decisions, semantic validation, privilege separation, resource limits,
and logging policy.

## Trust Boundaries

- `argc` and `argv` passed to `clags_parse()` are untrusted.
- Values consumed by the built-in verifiers (`bool`, integer, floating-point,
  size, time, choice, path, file, dir, and subcommand inputs) remain untrusted
  until the embedding application validates their semantics.
- Filesystem paths accepted through `Clags_Path`, `Clags_File`, and `Clags_Dir`
  are untrusted caller input. `clags` only performs `stat()`-based
  existence/type checks.
- `clags_config_t` definitions, custom verifiers, callback flags, log handlers,
  and allocator macro overrides (`CLAGS_CALLOC`, `CLAGS_REALLOC`, `CLAGS_FREE`)
  are part of the trusted computing base.
- Names and descriptions used by `clags_usage()` are trusted application
  metadata and are printed verbatim.

## Protected Assets

- Process memory safety during parsing, list growth, string duplication, and
  usage formatting.
- Integrity of caller-owned output variables and `clags_list_t` storage.
- Deterministic rejection of malformed numeric values, unsupported units,
  unknown flags/options, invalid choice selections, invalid subcommand layouts,
  and failed path/file/dir checks.
- Overflow-safe size calculations for internal dynamic allocations.

## Defensive Posture In This Codebase

- Allocation and buffer growth math uses checked addition/multiplication via
  `<stdckdint.h>` when available, compiler overflow builtins otherwise, and a
  manual fallback when neither exists.
- Recursive subcommand parsing is capped at `CLAGS_MAX_PARSE_DEPTH` (default
  `64`), and parent/child config cycles are rejected before descent.
- Unsigned parsers reject sign-prefixed inputs; integer, double, and time
  parsers reject empty values, trailing junk, `ERANGE`, NaN, and infinity.
- Size and time parsers validate supported suffixes and reject values that
  would overflow `uint64_t`-backed storage.
- List parsing validates `clags_list_t.item_size` against the declared
  `value_type` before writing.
- Optional string duplication is tracked per-config and can be released through
  `clags_config_free_allocs()` or `clags_config_free()`.
- The library performs no file writes, network I/O, shell execution, or
  environment-variable parsing.
- Build definitions enable `-std=c23` or `-std=c2x`, `-Wall -Wextra
  -Wpedantic -Werror`, stack protector, `FORTIFY_SOURCE`, and PIE. Debug test
  targets add ASan, UBSan, and LSan flags.

## Security-Relevant Limits And Defaults

- Maximum recursive subcommand depth is `64` unless `CLAGS_MAX_PARSE_DEPTH` is
  overridden.
- Initial dynamic growth capacity is `8` unless `CLAGS_LIST_INIT_CAPACITY` is
  overridden.
- Usage/help alignment defaults to column `36` unless `CLAGS_USAGE_ALIGNMENT`
  is overridden.
- `duplicate_strings` defaults to `false`, so string-like outputs borrow
  pointers into `argv` unless explicitly enabled.
- `--` always disables option/flag parsing for the rest of the parse unless
  `allow_option_parsing_toggle` is enabled, in which case a later `--` can
  re-enable it.
- `ignore_prefix` and `list_terminator` are optional syntax extensions defined
  entirely by the caller.
- `Clags_Path`, `Clags_File`, and `Clags_Dir` rely on `stat()` at parse time
  only.

These are implementation properties, not isolation guarantees. Hostile inputs
can still consume CPU, stack depth, and heap memory up to the limits available
to the embedding process.

## Limitations And Caller Responsibilities

- `clags_parse()` mutates `clags_config_t` (`name`, `parent`, `invalid`,
  `allocs`, and `error`). Do not parse concurrently against the same config or
  config tree without external synchronization.
- When `duplicate_strings` is disabled, parsed `string`, `path`, `file`, and
  `dir` outputs are borrowed `argv` pointers. They become invalid once the
  caller releases or mutates the underlying argument storage.
- `Clags_Path`, `Clags_File`, and `Clags_Dir` do not canonicalize paths and are
  subject to TOCTOU races between parse-time `stat()` and later open/use by the
  caller.
- Custom verifiers, callback flags, log handlers, and allocator overrides
  execute with the caller's privileges and can reintroduce memory-safety or
  logic bugs outside `clags`'s control.
- The default log handler prints raw argument values and configuration errors to
  `stdout`/`stderr`. Do not rely on it when arguments may contain secrets or
  sensitive file paths.
- The library validates syntax and basic type constraints only. Semantic policy
  remains the caller's responsibility: allowed ranges, path policy,
  authorization, cross-argument constraints, and business rules all belong in
  the application.
- Internal allocation failures and invariant violations use `clags_assert()`
  and abort the process. This favors fail-fast behavior over silent corruption,
  but hostile inputs can still cause denial of service through resource
  exhaustion.
- `clags_config_free()` frees list buffers and duplicated strings for one
  config only; it does not recurse into child configs automatically.
- An in-tree libFuzzer harness is available at `testing/fuzz_parse.c` and can
  be built with `just fuzz-build` when a libFuzzer-capable compiler (for
  example `clang`) is available.

## Verification

Current validation confirmed in this checkout:

- `just test`
- `zig build test`

Additional validation paths present in the repository:

- `just tests-debug` builds the test binary with ASan, UBSan, and LSan enabled.
- `just fuzz-build` builds the in-tree libFuzzer harness with ASan and UBSan
  when `FUZZ_CC` points to a compatible compiler.
- `just test-debug` exists, but it does not complete in this environment
  because LeakSanitizer cannot run under the current ptrace restrictions.

## Deployment Guidance

- Treat every parsed value as untrusted until your application validates
  semantics and authorization.
- Enable `duplicate_strings` when parsed values must outlive `argv` storage or
  when you want explicit ownership boundaries.
- Re-validate paths at open/use time and apply least-privilege filesystem
  policy around any `Clags_Path`, `Clags_File`, or `Clags_Dir` inputs.
- Use a custom log handler or raise `min_log_level` when argument values may
  contain secrets.
- Free duplicated strings and list storage with `clags_config_free()` or the
  narrower free helpers on every parse path that enables allocations.
- Add fuzzing and sanitizer-backed CI if the parser will handle hostile or
  high-volume command-line input. The repository ships a starter harness, but
  continuous fuzzing coverage is still a caller/integrator responsibility.
