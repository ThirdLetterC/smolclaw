# Security Model

`psimd` is a portable, header-only SIMD library. It exposes fixed-width vector
types and inline operations over caller-owned scalar buffers, selecting scalar,
x86, ARM NEON, or WebAssembly SIMD code paths at compile time.

This project does not parse files, allocate memory, spawn threads, perform I/O
beyond the test binary, or maintain global mutable library state.

## Trust Boundaries

- Public API calls in `psimd.h` are the primary trust boundary. Function
  arguments are trusted only to the extent required by C and the selected
  platform intrinsic.
- Load, store, lane, shift, and prefetch functions consume caller-provided
  pointers or indexes. The library does not perform runtime validation before
  issuing scalar accesses or SIMD intrinsics.
- Vector values returned by `psimd_*` functions are plain value types. Callers
  own all source and destination storage passed to load and store functions.
- Backend selection is compile-time only. The active backend depends on compiler
  target macros such as `__SSE2__`, `__AVX__`, `__AVX2__`, `__ARM_NEON`, and
  `__wasm_simd128__`, or on `PSIMD_FORCE_SCALAR`.
- Platform intrinsic headers, compiler code generation, target CPU feature
  flags, and floating-point environment behavior are part of the trusted
  computing base.

## Attacker Model

An attacker may influence:

- Data loaded from caller buffers and stored through caller buffers.
- Pointer values, buffer lengths, and alignment in applications that expose
  `psimd` operations to untrusted inputs.
- Lane indexes passed to functions such as `psimd_get_lane_f32x4` and
  `psimd_set_lane_f32x4`.
- Shift counts passed to integer shift helpers such as `psimd_shl_i32x4`,
  `psimd_shr_i32x4`, `psimd_shru_i32x4`, `psimd_shl_u32x4`, and
  `psimd_shr_u32x4`.
- Floating-point values including NaNs, infinities, signed zeroes, subnormals,
  and divide-by-zero operands.
- Build flags that select scalar, SSE2, SSE4.1, AVX/AVX2+FMA, NEON, or WASM
  SIMD implementations.

## Protected Assets

- Process memory safety while reading from and writing to caller-owned buffers.
- Consistent vector storage layout and alignment across supported backends.
- Deterministic backend selection for a given compiler target.
- Absence of hidden dynamic allocation, hidden ownership transfer, and internal
  persistent state.
- Correctness of arithmetic, comparison, mask, select, reduction, conversion,
  and shuffle operations for supported inputs.

## Defensive Posture

- `psimd` is header-only and allocation-free. There are no library-owned heap
  objects, destructor APIs, or cleanup paths.
- Vector types store bytes in explicitly sized structs with `PSIMD_ALIGN`.
  Public type names encode element type and lane count, avoiding native-width
  vector types whose size changes by target.
- ISA-specific intrinsic types do not appear in the public API. Backend casts
  and intrinsic calls are contained inside `psimd.h`.
- Unaligned load and store APIs (`psimd_loadu_*`, `psimd_storeu_*`) are provided
  for buffers whose alignment is not guaranteed.
- Aligned load and store APIs (`psimd_loada_*`, `psimd_storea_*`) are separate
  from unaligned APIs, making the caller's alignment contract explicit.
- `x8` floating-point operations use native AVX registers when available and
  fall back to two `x4` operations otherwise. Plain AVX uses two `x4` paths for
  256-bit integer operations that require AVX2.
- SSE2 fallbacks cover operations that need newer instructions on SSE4.1 or
  AVX, including selected integer operations and rounding helpers.
- Reinterpret helpers use `memcpy` or value-preserving storage copies where the
  public API needs bit reinterpretation.
- `psimd_get_backend` exposes the active backend so callers and tests can verify
  the compiled code path.
- The local test suite exercises scalar, SSE4.1, and AVX2+FMA builds through the
  `just` targets. AVX2 tests are skipped automatically by `test-all` when CPU
  support is not detected.

## Limitations And Caller Responsibilities

- Caller pointers must be valid for the full vector width being loaded or
  stored. For example, `psimd_loadu_f32x4` reads four `float` values and
  `psimd_storeu_f32x8` writes eight `float` values.
- `psimd_loada_*` and `psimd_storea_*` require natural alignment: 16 bytes for
  `x4` values and 32 bytes for `x8` values. Passing unaligned storage to these
  functions is undefined behavior and may fault.
- Use unaligned load and store functions when alignment cannot be proven.
- Lane indexes must be in range for the vector width. Passing a negative index
  or an index greater than or equal to the lane count is out of scope for the
  library and may read or write outside a temporary lane array.
- Shift counts must be valid for the element width. Keep 32-bit integer shift
  counts in the range `0..31`.
- Floating-point operations follow the selected hardware or compiler backend.
  NaN propagation, signed-zero behavior, subnormal handling, exception flags,
  and approximate reciprocal-square-root precision may vary by target.
- `psimd` does not authenticate data, clamp input ranges, sanitize NaNs, or
  enforce application-level numeric policy.
- `psimd_prefetch_r` and `psimd_prefetch_w` are hints only. Callers should still
  pass pointers that are meaningful for the embedding program and avoid using
  prefetch as a validation or bounds mechanism.
- The library is reentrant because it has no mutable internal state. Thread
  safety of buffers passed to `psimd` remains the caller's responsibility.
- The current repository is C23-oriented in its examples and `justfile`
  defaults. Downstream users should compile their own code with the same warning
  policy while verifying compiler support for the intrinsic headers they target.

## Build Hardening

- Current local defaults are defined in `justfile`:
  `-O2 -std=c2x -Wall -Wextra -Wpedantic -Werror`.
- `just test` runs `test-all`, which builds and runs scalar, SSE4.1, and
  AVX2+FMA tests when supported by the host CPU.
- `just test-scalar` compiles with `-DPSIMD_FORCE_SCALAR`.
- `just test-sse41` compiles with `-msse4.1`.
- `just test-avx2` compiles with `-mavx2 -mfma`.
- `just format-check` checks `psimd.h` and `test.c` with `clang-format`.
- `just test-sanitize` runs the scalar test binary under AddressSanitizer,
  UndefinedBehaviorSanitizer, and LeakSanitizer.
- Security-sensitive downstream builds should add sanitizers and hardening flags
  appropriate for their platform, such as:

```sh
CFLAGS="-O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer \
  -Wall -Wextra -Wpedantic -Werror"
```

- Production embedders should combine `psimd` with their normal platform
  hardening, such as stack protection, fortify support where available, PIE, and
  RELRO/now linking. Header-only code inherits these settings from the
  translation unit that includes `psimd.h`.

## Reporting

When reporting a security issue, include:

- A minimal reproducer that includes `psimd.h`.
- Compiler name and version.
- Target architecture, operating system, and CPU feature flags.
- The active backend from `psimd_get_backend`.
- Full compile command and relevant `CFLAGS`.
- Whether the issue reproduces with `PSIMD_FORCE_SCALAR`.
- Whether the issue reproduces under sanitizers or only with a specific ISA
  backend.
