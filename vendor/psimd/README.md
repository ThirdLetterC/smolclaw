### psimd

A portable, header-only SIMD library for C23. A single include, no dependencies beyond the C standard library and the platform's intrinsic headers. Provides a stable, ISA-agnostic API that maps to native vector instructions on x86 (SSE2, SSE4.1, AVX/AVX2+FMA), ARM (NEON/AArch64) and WebAssembly SIMD128, with a scalar fallback for everything else.

### Features

- Explicit types/widths where every type name encodes its element type and lane count. `psimd_f32x4` is always four 32-bit floats regardless of the platform. There are no abstract "native width" types that silently change size between targets.
- All types are byte-array structs aligned to their natural register size. Backends reinterpret the storage internally. No ISA-specific types leak into user code.
- C has no operator overloading. Every operation is a named function: `psimd_add_f32x4`, `psimd_cmplt_f32x4`, `psimd_select_f32x4`. The naming scheme is `psimd_<op>_<type><lanes>`.
- Comparison operations return `psimd_mask32x4` (or `x8`) where all bits set means true and zero means false. This matches SSE, AVX, NEON and WASM semantics directly and allows mask results to feed `psimd_select_*` without any conversion.

### Installation

Include the [`psimd.h`](/psimd.h) header.

The backend is chosen by passing the appropriate architecture flag to the compiler. No source changes required.
Use `-std=c2x` with compilers that implement C23 features but have not yet accepted `-std=c23`.

#### x86

```sh
# Force scalar fallback
gcc -O2 -DPSIMD_FORCE_SCALAR -std=c23 your_file.c -lm

# SSE2
gcc -O2 -msse2 -std=c23 your_file.c -lm

# SSE4.1 (adds blendv, floor/ceil/round, faster integer min/max/mul)
gcc -O2 -msse4.1 -std=c23 your_file.c -lm

# AVX + FMA (256-bit f32x8, native FMA, full AVX2 integer ops)
gcc -O2 -mavx2 -mfma -std=c23 your_file.c -lm
```

Clang accepts the same flags. MSVC users would pass `/arch:AVX2` for the AVX backend.

#### ARM

```sh
# ARMv7 NEON (cross-compile)
arm-linux-gnueabihf-gcc -O2 -mfpu=neon -std=c23 your_file.c -lm

# AArch64 (NEON is always present, no flag needed)
aarch64-linux-gnu-gcc -O2 -std=c23 your_file.c -lm
```

On Apple Silicon with Clang, NEON is detected automatically, no extra flags needed.

#### WebAssembly

```sh
emcc -O2 -msimd128 -std=c23 your_file.c -lm -o output.js
```

#### Checking which backend is active

```c
psimd_backend b = psimd_get_backend();
// psimd_backend_scalar
// psimd_backend_sse2
// psimd_backend_sse41
// psimd_backend_avx
// psimd_backend_neon
// psimd_backend_wasm
```

### Running the tests

```sh
# Scalar
gcc -O2 -DPSIMD_FORCE_SCALAR -std=c23 -Wall -Wextra -Wpedantic -Werror test.c -lm -o test && ./test

# SSE4.1
gcc -O2 -msse4.1 -std=c23 -Wall -Wextra -Wpedantic -Werror test.c -lm -o test && ./test

# AVX2 + FMA
gcc -O2 -mavx2 -mfma -std=c23 -Wall -Wextra -Wpedantic -Werror test.c -lm -o test && ./test
```

The suite runs 114 checks covering construction, arithmetic, comparisons, masks, select, reductions, type conversions, shuffles and several practical kernels.

### Types

#### 128-bit (4-lane)

| Type             | Description                                    |
| ---------------- | ---------------------------------------------- |
| `psimd_f32x4`    | 4 x 32-bit float                               |
| `psimd_i32x4`    | 4 x 32-bit signed integer                      |
| `psimd_u32x4`    | 4 x 32-bit unsigned integer                    |
| `psimd_mask32x4` | 4-lane bitmask (all-ones = true, zero = false) |

#### 256-bit (8-lane)

| Type             | Description               |
| ---------------- | ------------------------- |
| `psimd_f32x8`    | 8 x 32-bit float          |
| `psimd_i32x8`    | 8 x 32-bit signed integer |
| `psimd_mask32x8` | 8-lane bitmask            |

`x8` types use native 256-bit registers on AVX and are emulated via two `x4` operations everywhere else. The API is identical in both cases.

### API reference

#### f32x4 construction

```c
psimd_f32x4 psimd_set1_f32x4(float x);
psimd_f32x4 psimd_set_f32x4(float x0, float x1, float x2, float x3);
psimd_f32x4 psimd_zero_f32x4();
psimd_f32x4 psimd_loadu_f32x4(const float *p);   // unaligned load
psimd_f32x4 psimd_loada_f32x4(const float *p);   // 16-byte aligned load
void        psimd_storeu_f32x4(float *p, psimd_f32x4 v);
void        psimd_storea_f32x4(float *p, psimd_f32x4 v);
float       psimd_get_lane_f32x4(psimd_f32x4 v, int i);
psimd_f32x4 psimd_set_lane_f32x4(psimd_f32x4 v, int i, float x);
```

`psimd_set_lane_f32x4` returns a new vector with lane `i` replaced. The input `v` is not modified.

#### f32x4 arithmetic

```c
psimd_f32x4 psimd_add_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_f32x4 psimd_sub_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_f32x4 psimd_mul_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_f32x4 psimd_div_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_f32x4 psimd_neg_f32x4(psimd_f32x4 a);
psimd_f32x4 psimd_abs_f32x4(psimd_f32x4 a);
psimd_f32x4 psimd_sqrt_f32x4(psimd_f32x4 a);
psimd_f32x4 psimd_rsqrt_f32x4(psimd_f32x4 a);    // approximate
psimd_f32x4 psimd_min_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_f32x4 psimd_max_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_f32x4 psimd_fma_f32x4(psimd_f32x4 a, psimd_f32x4 b, psimd_f32x4 c);   // a*b+c
psimd_f32x4 psimd_fnma_f32x4(psimd_f32x4 a, psimd_f32x4 b, psimd_f32x4 c);  // -a*b+c
```

`psimd_rsqrt_f32x4` uses a hardware estimate with one Newton-Raphson refinement on NEON. On SSE it maps to `_mm_rsqrt_ps` directly (~12-bit precision).

#### f32x4 math

```c
psimd_f32x4 psimd_floor_f32x4(psimd_f32x4 a);
psimd_f32x4 psimd_ceil_f32x4(psimd_f32x4 a);
psimd_f32x4 psimd_round_f32x4(psimd_f32x4 a);   // nearest, ties to even
```

On SSE2 (without SSE4.1), `floor` and `ceil` fall back to scalar `floorf`/`ceilf`. `round` preserves nearest-even tie handling across backends.

#### f32x4 comparisons

All comparisons return a `psimd_mask32x4`.

```c
psimd_mask32x4 psimd_cmpeq_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_mask32x4 psimd_cmpne_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_mask32x4 psimd_cmplt_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_mask32x4 psimd_cmple_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_mask32x4 psimd_cmpgt_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_mask32x4 psimd_cmpge_f32x4(psimd_f32x4 a, psimd_f32x4 b);
```

#### f32x4 select and reduce

```c
psimd_f32x4 psimd_select_f32x4(psimd_mask32x4 m, psimd_f32x4 a, psimd_f32x4 b);

float psimd_reduce_add_f32x4(psimd_f32x4 v);
float psimd_reduce_mul_f32x4(psimd_f32x4 v);
float psimd_reduce_max_f32x4(psimd_f32x4 v);
float psimd_reduce_min_f32x4(psimd_f32x4 v);
```

`psimd_select_f32x4(m, a, b)` returns `a` where `m` is true, `b` where `m` is false. Equivalent to a branchless ternary per lane.

#### f32x4 shuffle

```c
psimd_f32x4 psimd_zip_lo_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_f32x4 psimd_zip_hi_f32x4(psimd_f32x4 a, psimd_f32x4 b);
psimd_f32x4 psimd_hadd_f32x4(psimd_f32x4 a, psimd_f32x4 b);
float       psimd_dot4_f32x4(psimd_f32x4 a, psimd_f32x4 b);
```

#### mask32x4 operations

```c
psimd_mask32x4 psimd_and_mask32x4(psimd_mask32x4 a, psimd_mask32x4 b);
psimd_mask32x4 psimd_or_mask32x4(psimd_mask32x4 a, psimd_mask32x4 b);
psimd_mask32x4 psimd_xor_mask32x4(psimd_mask32x4 a, psimd_mask32x4 b);
psimd_mask32x4 psimd_not_mask32x4(psimd_mask32x4 a);
bool           psimd_any_mask32x4(psimd_mask32x4 m);
bool           psimd_all_mask32x4(psimd_mask32x4 m);
psimd_mask32x4 psimd_true_mask32x4();
psimd_mask32x4 psimd_false_mask32x4();
```

#### i32x4

```c
psimd_i32x4 psimd_set1_i32x4(int32_t x);
psimd_i32x4 psimd_set_i32x4(int32_t x0, int32_t x1, int32_t x2, int32_t x3);
psimd_i32x4 psimd_zero_i32x4();
psimd_i32x4 psimd_loadu_i32x4(const int32_t *p);
psimd_i32x4 psimd_loada_i32x4(const int32_t *p);
void        psimd_storeu_i32x4(int32_t *p, psimd_i32x4 v);
void        psimd_storea_i32x4(int32_t *p, psimd_i32x4 v);

psimd_i32x4 psimd_add_i32x4(psimd_i32x4 a, psimd_i32x4 b);
psimd_i32x4 psimd_sub_i32x4(psimd_i32x4 a, psimd_i32x4 b);
psimd_i32x4 psimd_mul_i32x4(psimd_i32x4 a, psimd_i32x4 b);
psimd_i32x4 psimd_neg_i32x4(psimd_i32x4 a);
psimd_i32x4 psimd_abs_i32x4(psimd_i32x4 a);
psimd_i32x4 psimd_min_i32x4(psimd_i32x4 a, psimd_i32x4 b);
psimd_i32x4 psimd_max_i32x4(psimd_i32x4 a, psimd_i32x4 b);

psimd_i32x4 psimd_and_i32x4(psimd_i32x4 a, psimd_i32x4 b);
psimd_i32x4 psimd_or_i32x4(psimd_i32x4 a, psimd_i32x4 b);
psimd_i32x4 psimd_xor_i32x4(psimd_i32x4 a, psimd_i32x4 b);
psimd_i32x4 psimd_not_i32x4(psimd_i32x4 a);
psimd_i32x4 psimd_shl_i32x4(psimd_i32x4 a, int n);    // logical left shift
psimd_i32x4 psimd_shr_i32x4(psimd_i32x4 a, int n);    // arithmetic right shift (sign-extending)
psimd_i32x4 psimd_shru_i32x4(psimd_i32x4 a, int n);   // logical right shift (zero-filling)

psimd_mask32x4 psimd_cmpeq_i32x4(psimd_i32x4 a, psimd_i32x4 b);
psimd_mask32x4 psimd_cmplt_i32x4(psimd_i32x4 a, psimd_i32x4 b);
psimd_mask32x4 psimd_cmpgt_i32x4(psimd_i32x4 a, psimd_i32x4 b);
psimd_i32x4    psimd_select_i32x4(psimd_mask32x4 m, psimd_i32x4 a, psimd_i32x4 b);

int32_t psimd_reduce_add_i32x4(psimd_i32x4 v);
int32_t psimd_reduce_max_i32x4(psimd_i32x4 v);
int32_t psimd_reduce_min_i32x4(psimd_i32x4 v);
```

#### Type conversions

```c
psimd_i32x4 psimd_cvt_f32x4_i32x4(psimd_f32x4 a);     // truncate toward zero
psimd_f32x4 psimd_cvt_i32x4_f32x4(psimd_i32x4 a);

psimd_f32x4    psimd_reinterpret_i32x4_f32x4(psimd_i32x4 a);
psimd_i32x4    psimd_reinterpret_f32x4_i32x4(psimd_f32x4 a);
psimd_mask32x4 psimd_reinterpret_i32x4_mask32x4(psimd_i32x4 a);
psimd_i32x4    psimd_reinterpret_mask32x4_i32x4(psimd_mask32x4 a);
```

#### f32x8 and i32x8

The x8 API mirrors x4. Key functions:

```c
psimd_f32x8 psimd_set1_f32x8(float x);
psimd_f32x8 psimd_loadu_f32x8(const float *p);
psimd_f32x8 psimd_loada_f32x8(const float *p);
void        psimd_storeu_f32x8(float *p, psimd_f32x8 v);
void        psimd_storea_f32x8(float *p, psimd_f32x8 v);

psimd_f32x8 psimd_add_f32x8(psimd_f32x8 a, psimd_f32x8 b);
psimd_f32x8 psimd_sub_f32x8(psimd_f32x8 a, psimd_f32x8 b);
psimd_f32x8 psimd_mul_f32x8(psimd_f32x8 a, psimd_f32x8 b);
psimd_f32x8 psimd_div_f32x8(psimd_f32x8 a, psimd_f32x8 b);
psimd_f32x8 psimd_fma_f32x8(psimd_f32x8 a, psimd_f32x8 b, psimd_f32x8 c);
psimd_f32x8 psimd_min_f32x8(psimd_f32x8 a, psimd_f32x8 b);
psimd_f32x8 psimd_max_f32x8(psimd_f32x8 a, psimd_f32x8 b);

psimd_mask32x8 psimd_cmplt_f32x8(psimd_f32x8 a, psimd_f32x8 b);
psimd_f32x8    psimd_select_f32x8(psimd_mask32x8 m, psimd_f32x8 a, psimd_f32x8 b);

float psimd_reduce_add_f32x8(psimd_f32x8 v);
float psimd_reduce_max_f32x8(psimd_f32x8 v);
float psimd_reduce_min_f32x8(psimd_f32x8 v);

psimd_f32x4 psimd_lo_f32x8(psimd_f32x8 v);
psimd_f32x4 psimd_hi_f32x8(psimd_f32x8 v);
psimd_f32x8 psimd_combine_f32x8(psimd_f32x4 lo, psimd_f32x4 hi);
```

### Usage examples

#### Elementwise array addition

```c
void add_arrays(const float *a, const float *b, float *out, int n) {
  int i = 0;

  for (; i <= n - 4; i += 4) {
    psimd_f32x4 va = psimd_loadu_f32x4(a + i);
    psimd_f32x4 vb = psimd_loadu_f32x4(b + i);
    psimd_storeu_f32x4(out + i, psimd_add_f32x4(va, vb));
  }

  for (; i < n; i++) out[i] = a[i] + b[i];
}
```

#### ReLU activation

```c
void relu(float *data, int n) {
  psimd_f32x4 zero = psimd_zero_f32x4();
  int i = 0;

  for (; i <= n - 4; i += 4) {
    psimd_f32x4 v = psimd_loadu_f32x4(data + i);
    psimd_storeu_f32x4(data + i, psimd_max_f32x4(v, zero));
  }

  for (; i < n; i++) if (data[i] < 0.0f) data[i] = 0.0f;
}
```

#### Dot product via FMA

```c
float dot(const float *a, const float *b, int n) {
  psimd_f32x4 acc = psimd_zero_f32x4();
  int i = 0;
  for (; i <= n - 4; i += 4)
    acc = psimd_fma_f32x4(psimd_loadu_f32x4(a + i), psimd_loadu_f32x4(b + i), acc);
  float result = psimd_reduce_add_f32x4(acc);
  for (; i < n; i++) result += a[i] * b[i];
  return result;
}
```

#### Branchless clamp

```c
void clamp(float *data, float lo, float hi, int n) {
  psimd_f32x4 vlo = psimd_set1_f32x4(lo);
  psimd_f32x4 vhi = psimd_set1_f32x4(hi);
  int i = 0;

  for (; i <= n - 4; i += 4) {
    psimd_f32x4 v = psimd_loadu_f32x4(data + i);
    psimd_storeu_f32x4(data + i, psimd_min_f32x4(psimd_max_f32x4(v, vlo), vhi));
  }

  for (; i < n; i++) {
    if (data[i] < lo) data[i] = lo;
    else if (data[i] > hi) data[i] = hi;
  }
}
```

#### Conditional select without branching

```c
psimd_f32x4 threshold = psimd_set1_f32x4(0.5f);
psimd_f32x4 values    = psimd_loadu_f32x4(ptr);
psimd_f32x4 fallback  = psimd_set1_f32x4(0.0f);

psimd_mask32x4 above = psimd_cmpgt_f32x4(values, threshold);
psimd_f32x4 result   = psimd_select_f32x4(above, values, fallback);
```

### Backend notes

#### SSE2 vs SSE4.1

SSE2 lacks `_mm_blendv_ps`, `_mm_mullo_epi32`, `_mm_min_epi32`, `_mm_max_epi32` and `_mm_floor/ceil/round_ps`. All of these are emulated on SSE2 using available instructions. The emulation is correct but slightly less efficient. Compile with `-msse4.1` or `-mavx2` to use native paths.

#### AVX without AVX2

The `PSIMD_AVX` path is activated by either `-mavx` or `-mavx2`. Float operations (`f32x8`) always use 256-bit AVX instructions. Integer `i32x8` operations use AVX2 when available and fall back to two `i32x4` operations on plain AVX (which lacks 256-bit integer ops). For integer-heavy workloads, prefer `-mavx2`.

#### NEON on ARMv7 vs AArch64

On ARMv7 NEON, `vdivq_f32` and `vsqrtq_f32` do not exist. `psimd_div_f32x4` uses a reciprocal estimate with two Newton-Raphson refinement steps (~23-bit accuracy). `psimd_sqrt_f32x4` uses `vrsqrteq_f32` multiplied back. On AArch64 these use the native hardware instructions.

#### WASM SIMD128

All operations map directly to `wasm_simd128.h` intrinsics. The target requires Emscripten with `-msimd128`. Reductions fall through to scalar loops since WASM does not expose horizontal reduction instructions.

### Alignment

`psimd_loada_*` and `psimd_storea_*` require the pointer to be aligned to the type's natural alignment (16 bytes for `x4`, 32 bytes for `x8`). Passing an unaligned pointer to an aligned load is undefined behavior and will fault on most platforms. Use `psimd_loadu_*` / `psimd_storeu_*` when alignment cannot be guaranteed.

To declare an aligned buffer:

```c
alignas(16) float buf[64];
```

### Limitations

Integer division has no SIMD equivalent on any supported ISA and is not provided. Divide by converting to float, dividing and converting back or restructure the algorithm to avoid it.

The `x8` types on non-AVX platforms are implemented as two sequential `x4` operations. They are provided for source portability, not for emulated performance.

There is no scatter/gather, no masked load/store and no 64-bit float (`f64x2`) arithmetic beyond the type stub. These may be added in future revisions.

### License

Apache v2.0 License
