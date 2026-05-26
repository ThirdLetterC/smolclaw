# Security Model

This repository is an in-process JSON parser and serializer library written in
C23. It parses attacker-controlled JSON text into a tree of `JSON_Value`,
`JSON_Object`, and `JSON_Array` nodes, provides mutation and lookup helpers,
and serializes that tree back to JSON text.

Parson is not a sandbox, not a policy engine, and not a full JSON Schema
implementation. Its job is to parse, build, validate, and serialize JSON in the
caller's process. Callers remain responsible for trust decisions, semantic
validation, resource limits, and filesystem policy.

## Trust Boundaries

- All input passed to `json_parse_string()`,
  `json_parse_string_with_comments()`, `json_parse_file()`, and
  `json_parse_file_with_comments()` is untrusted.
- File paths passed to `json_parse_file()`, `json_parse_file_with_comments()`,
  `json_serialize_to_file()`, and `json_serialize_to_file_pretty()` are
  untrusted caller input unless the application constrains them.
- Object keys and dotted paths passed to `json_object_get_*()`,
  `json_object_dotget_*()`, `json_object_set_*()`, and
  `json_object_dotset_*()` are untrusted application input.
- `JSON_Value *` trees supplied to mutation, validation, comparison, and
  serialization APIs are untrusted unless the caller created and controlled
  them.
- Allocator hooks, float formats, and custom number serialization callbacks set
  through `json_set_allocation_functions()`,
  `json_set_float_serialization_format()`, and
  `json_set_number_serialization_function()` are part of the trusted computing
  base.
- The library runs inside the caller's address space and inherits the caller's
  memory, file, and thread-safety constraints.

## Protected Assets

- Process memory safety while parsing, building, validating, and serializing
  hostile JSON.
- Structural integrity of the in-memory JSON tree.
- Deterministic rejection of malformed JSON, invalid Unicode escape sequences,
  invalid UTF-8 passed through string-building APIs, and invalid numeric input.
- Bounded writes during serialization when using the size-query APIs before
  `json_serialize_to_buffer()` or `json_serialize_to_buffer_pretty()`.

## Defensive Posture In This Codebase

- The parser enforces a maximum nesting depth of `2048`.
- File parsing reads the file into a heap buffer with overflow checks and a
  terminating NUL byte before reusing the same string parser path.
- `json_parse_string()` accepts and skips a UTF-8 BOM at the beginning of the
  input.
- Parsed JSON strings are decoded through escape-processing logic that rejects
  malformed escapes and invalid surrogate-pair handling.
- Programmatically created strings go through UTF-8 validation in
  `json_value_init_string_with_len()`.
- Programmatically created numbers reject NaN and infinity.
- Serialization computes the required output size up front; buffer serializers
  fail if the caller-provided buffer is too small.
- Allocation failures are checked and reported through `nullptr` or
  `JSONFailure`.
- The repository builds with `-std=c23 -Wall -Wextra -Wpedantic -Werror`.
- The repository includes a collision-focused test configuration via
  `PARSON_FORCE_HASH_COLLISIONS`.

## Security-Relevant Limits And Defaults

- Maximum parser nesting depth is `2048`.
- Default object/array growth starts from a capacity of `16`.
- Default number serialization format is `%1.17g`.
- Internal temporary number buffer size is `64` bytes.
- Slash escaping is enabled by default during serialization.
- `json_parse_file()` and `json_parse_file_with_comments()` read the entire file
  into memory before parsing.
- `json_parse_string()` and `json_parse_file()` parse the first JSON value; they
  do not enforce full-input consumption after that value.
- Comment-tolerant parsing duplicates the input and strips `/* ... */` and
  `// ...` comments before parsing.

These are implementation properties, not isolation boundaries. Hostile inputs
can still consume CPU and memory up to the process limits available to the
caller.

## Limitations And Caller Responsibilities

- All `json_set_*()` configuration functions mutate process-global state.
  `json_set_escape_slashes()` is explicitly documented as not thread-safe, and
  the same practical restriction applies to allocator and serialization-format
  setters.
- This library is not a resource governor. Callers should impose file-size,
  input-size, time, and memory limits around untrusted data.
- File parsing APIs are not suitable for unbounded attacker-controlled files
  because the full file is loaded into memory first.
- If your application requires strict whole-document validation, add an
  application-side wrapper that rejects trailing data. The public parse APIs
  intentionally stop after the first JSON value.
- `json_parse_string_with_comments()` and `json_parse_file_with_comments()`
  accept a JSON superset. Use the strict parsers when comments must be rejected.
- `json_validate()` is a structural compatibility helper, not JSON Schema. It
  permits extra object fields, and for arrays it validates every element against
  only the first schema element.
- Dot-notation helpers are convenience APIs. Keys containing literal `.` bytes
  can be ambiguous or inaccessible through `dotget` and `dotset` paths.
- The library does not zeroize freed memory. Parsed secrets remain in process
  memory until normal allocator reuse.
- Custom allocators and custom number serialization callbacks must obey the
  contracts expected by the library. Incorrect hooks can reintroduce memory
  safety bugs outside Parson's control.

## Examples And Development Artifacts

- `examples/tests.c` is the in-tree test harness, not a hardened application
  frontend.
- `examples/security_tests.c` is the dedicated in-tree security regression
  target for hostile-input behavior and contract checks.
- `tests/*.txt` are parser and serialization fixtures.
- There is currently no dedicated fuzzing harness.

## Verification

Current validation that exists in this repository:

- `zig build`
- `zig build test`
- `zig build test-collisions`
- `zig build test-security`
- `just build`
- `just test`
- `just test-collisions`
- `just test-security`

Additional validation that is reasonable for security-sensitive embedding:

- Run debug builds or pass `zig build -Dsanitize=full` so Zig's C
  sanitization stays enabled.
- Add fuzzing coverage for `json_parse_string()`,
  `json_parse_string_with_comments()`, `json_parse_file()`, and the serializer
  entry points.
- Expand the security regression suite with adversarial custom allocator hooks
  and any application-specific whole-document parsing wrappers you add on top
  of the library.

## Deployment Guidance

- Treat every parsed value as untrusted until your application validates its
  semantics.
- Prefer string-based parsing on already-bounded buffers when input size limits
  matter.
- Apply external path controls before using file-based parse or serialize APIs.
- Avoid mutating global serializer or allocator settings concurrently with other
  library calls.
- Free every owned `JSON_Value *` with `json_value_free()` exactly once.
- Free strings returned by `json_serialize_to_string()` and
  `json_serialize_to_string_pretty()` with
  `json_free_serialized_string()`.
