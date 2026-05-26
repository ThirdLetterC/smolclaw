// Copyright 2026 Abdi Moalim
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef PSIMD_H
#define PSIMD_H

#include <stdint.h>
#include <string.h>
#include <math.h>

/*
 * Clang 18 accepts -std=c23 and reports __STDC_VERSION__ 202311L, but it does
 * not yet implement C23 constexpr in C mode. Keep the source intent explicit
 * while allowing the Clang-backed build to compile.
 */
#if defined(__clang__)
#define PSIMD_CONSTEXPR const
#else
#define PSIMD_CONSTEXPR constexpr
#endif

#if defined(PSIMD_FORCE_SCALAR)
#define PSIMD_SCALAR 1
#elif defined(__AVX2__) || defined(__AVX__)
#include <immintrin.h>
#define PSIMD_AVX 1
#define PSIMD_SSE2 1
#if defined(__AVX2__)
#define PSIMD_AVX2 1
#endif
#if defined(__SSE4_1__)
#define PSIMD_SSE41 1
#endif
#if defined(__FMA__)
#define PSIMD_FMA 1
#endif
#elif defined(__SSE2__)
#include <emmintrin.h>
#ifdef __SSE4_1__
#include <smmintrin.h>
#define PSIMD_SSE41 1
#endif
#ifdef __FMA__
#include <immintrin.h>
#define PSIMD_FMA 1
#endif
#define PSIMD_SSE2 1
#elif defined(__ARM_NEON) || defined(__ARM_NEON__)
#include <arm_neon.h>
#define PSIMD_NEON 1
#elif defined(__wasm_simd128__)
#include <wasm_simd128.h>
#define PSIMD_WASM 1
#else
#define PSIMD_SCALAR 1
#endif

static PSIMD_CONSTEXPR int PSIMD_VERSION_MAJOR = 1;
static PSIMD_CONSTEXPR int PSIMD_VERSION_MINOR = 0;
static PSIMD_CONSTEXPR int PSIMD_VERSION_PATCH = 0;

static PSIMD_CONSTEXPR uint32_t psimd__mask32_true_bits = UINT32_MAX;

#if defined(_MSC_VER)
#define PSIMD_INLINE __forceinline
#elif defined(__GNUC__) || defined(__clang__)
#define PSIMD_INLINE __attribute__((always_inline)) static inline
#else
#define PSIMD_INLINE static inline
#endif

PSIMD_INLINE int32_t psimd__i32_from_u32(uint32_t x) {
  int32_t r;
  memcpy(&r, &x, sizeof(r));
  return r;
}

PSIMD_INLINE float psimd__round_nearest_even_f32(float x) {
  float lo = floorf(x);
  float hi = ceilf(x);
  float dlo = x - lo;
  float dhi = hi - x;
  if (dlo < dhi)
    return lo;
  if (dhi < dlo)
    return hi;

  float half = 0.5f * lo;
  return floorf(half) == half ? lo : hi;
}

typedef struct {
  alignas(16) uint8_t _[16];
} psimd_f32x4;

typedef struct {
  alignas(16) uint8_t _[16];
} psimd_i32x4;

typedef struct {
  alignas(16) uint8_t _[16];
} psimd_u32x4;

typedef struct {
  alignas(16) uint8_t _[16];
} psimd_mask32x4;

typedef struct {
  alignas(32) uint8_t _[32];
} psimd_f32x8;

typedef struct {
  alignas(32) uint8_t _[32];
} psimd_i32x8;

typedef struct {
  alignas(32) uint8_t _[32];
} psimd_u32x8;

typedef struct {
  alignas(32) uint8_t _[32];
} psimd_mask32x8;

typedef struct {
  alignas(8) uint8_t _[8];
} psimd_f32x2;

typedef struct {
  alignas(8) uint8_t _[8];
} psimd_i32x2;

typedef struct {
  alignas(8) uint8_t _[8];
} psimd_mask32x2;

typedef struct {
  alignas(16) uint8_t _[16];
} psimd_f64x2;

typedef struct {
  alignas(16) uint8_t _[16];
} psimd_i64x2;

typedef struct {
  alignas(16) uint8_t _[16];
} psimd_i16x8;

typedef struct {
  alignas(16) uint8_t _[16];
} psimd_u16x8;

typedef struct {
  alignas(16) uint8_t _[16];
} psimd_i8x16;

typedef struct {
  alignas(16) uint8_t _[16];
} psimd_u8x16;

#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)

PSIMD_INLINE psimd_f32x4 psimd__from_m128(__m128 v) {
  psimd_f32x4 r = {{0}};
  _mm_store_ps((float*)r._, v);
  return r;
}

PSIMD_INLINE __m128 psimd__to_m128(psimd_f32x4 v) {
  return _mm_load_ps((const float*)v._);
}

PSIMD_INLINE psimd_i32x4 psimd__from_m128i(__m128i v) {
  psimd_i32x4 r = {{0}};
  _mm_store_si128((__m128i*)r._, v);
  return r;
}

PSIMD_INLINE __m128i psimd__to_m128i_f(psimd_f32x4 v) {
  return _mm_load_si128((const __m128i*)v._);
}

PSIMD_INLINE __m128i psimd__to_m128i(psimd_i32x4 v) {
  return _mm_load_si128((const __m128i*)v._);
}

PSIMD_INLINE psimd_mask32x4 psimd__from_m128i_mask(__m128i v) {
  psimd_mask32x4 r = {{0}};
  _mm_store_si128((__m128i*)r._, v);
  return r;
}

PSIMD_INLINE __m128i psimd__mask_to_m128i(psimd_mask32x4 v) {
  return _mm_load_si128((const __m128i*)v._);
}

#endif

#if defined(PSIMD_AVX)

PSIMD_INLINE psimd_f32x8 psimd__from_m256(__m256 v) {
  psimd_f32x8 r = {{0}};
  _mm256_store_ps((float*)r._, v);
  return r;
}

PSIMD_INLINE __m256 psimd__to_m256(psimd_f32x8 v) {
  return _mm256_load_ps((const float*)v._);
}

PSIMD_INLINE psimd_i32x8 psimd__from_m256i(__m256i v) {
  psimd_i32x8 r = {{0}};
  _mm256_store_si256((__m256i*)r._, v);
  return r;
}

PSIMD_INLINE __m256i psimd__to_m256i(psimd_i32x8 v) {
  return _mm256_load_si256((const __m256i*)v._);
}

PSIMD_INLINE psimd_mask32x8 psimd__from_m256i_mask(__m256i v) {
  psimd_mask32x8 r = {{0}};
  _mm256_store_si256((__m256i*)r._, v);
  return r;
}

PSIMD_INLINE __m256i psimd__mask_to_m256i(psimd_mask32x8 v) {
  return _mm256_load_si256((const __m256i*)v._);
}

#endif

/* ============================================================
   f32x4 — set / load / store
   ============================================================ */

PSIMD_INLINE psimd_f32x4 psimd_set1_f32x4(float x) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_set1_ps(x));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vdupq_n_f32(x));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t v = wasm_f32x4_splat(x);
  memcpy(r._, &v, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  float* p = (float*)r._;
  p[0] = p[1] = p[2] = p[3] = x;
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_set_f32x4(float x0, float x1, float x2,
                                         float x3) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_set_ps(x3, x2, x1, x0));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  float tmp[4] = {x0, x1, x2, x3};
  vst1q_f32((float*)r._, vld1q_f32(tmp));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t v = wasm_f32x4_make(x0, x1, x2, x3);
  memcpy(r._, &v, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  float* p = (float*)r._;
  p[0] = x0;
  p[1] = x1;
  p[2] = x2;
  p[3] = x3;
  return r;
#endif
}

[[nodiscard]] PSIMD_INLINE psimd_f32x4 psimd_zero_f32x4() {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_setzero_ps());
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vdupq_n_f32(0.0f));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t v = wasm_f32x4_splat(0.0f);
  memcpy(r._, &v, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  memset(r._, 0, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_loadu_f32x4(const float* p) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_loadu_ps(p));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vld1q_f32(p));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t v = wasm_v128_load(p);
  memcpy(r._, &v, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  memcpy(r._, p, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_loada_f32x4(const float* p) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_load_ps(p));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vld1q_f32(p));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t v = wasm_v128_load(p);
  memcpy(r._, &v, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  memcpy(r._, p, 16);
  return r;
#endif
}

PSIMD_INLINE void psimd_storeu_f32x4(float* p, psimd_f32x4 v) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  _mm_storeu_ps(p, psimd__to_m128(v));
#elif defined(PSIMD_NEON)
  vst1q_f32(p, vld1q_f32((const float*)v._));
#elif defined(PSIMD_WASM)
  v128_t tmp;
  memcpy(&tmp, v._, 16);
  wasm_v128_store(p, tmp);
#else
  memcpy(p, v._, 16);
#endif
}

PSIMD_INLINE void psimd_storea_f32x4(float* p, psimd_f32x4 v) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  _mm_store_ps(p, psimd__to_m128(v));
#elif defined(PSIMD_NEON)
  vst1q_f32(p, vld1q_f32((const float*)v._));
#elif defined(PSIMD_WASM)
  v128_t tmp;
  memcpy(&tmp, v._, 16);
  wasm_v128_store(p, tmp);
#else
  memcpy(p, v._, 16);
#endif
}

/* ============================================================
   f32x4 — arithmetic
   ============================================================ */

PSIMD_INLINE psimd_f32x4 psimd_add_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_add_ps(psimd__to_m128(a), psimd__to_m128(b)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vaddq_f32(vld1q_f32((const float*)a._),
                                   vld1q_f32((const float*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_add(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  float* pr = (float*)r._;
  pr[0] = pa[0] + pb[0];
  pr[1] = pa[1] + pb[1];
  pr[2] = pa[2] + pb[2];
  pr[3] = pa[3] + pb[3];
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_sub_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_sub_ps(psimd__to_m128(a), psimd__to_m128(b)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vsubq_f32(vld1q_f32((const float*)a._),
                                   vld1q_f32((const float*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_sub(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  float* pr = (float*)r._;
  pr[0] = pa[0] - pb[0];
  pr[1] = pa[1] - pb[1];
  pr[2] = pa[2] - pb[2];
  pr[3] = pa[3] - pb[3];
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_mul_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_mul_ps(psimd__to_m128(a), psimd__to_m128(b)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vmulq_f32(vld1q_f32((const float*)a._),
                                   vld1q_f32((const float*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_mul(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  float* pr = (float*)r._;
  pr[0] = pa[0] * pb[0];
  pr[1] = pa[1] * pb[1];
  pr[2] = pa[2] * pb[2];
  pr[3] = pa[3] * pb[3];
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_div_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_div_ps(psimd__to_m128(a), psimd__to_m128(b)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  float32x4_t va = vld1q_f32((const float*)a._);
  float32x4_t vb = vld1q_f32((const float*)b._);
#if defined(__aarch64__)
  vst1q_f32((float*)r._, vdivq_f32(va, vb));
#else
  float32x4_t inv = vrecpeq_f32(vb);
  inv = vmulq_f32(vrecpsq_f32(vb, inv), inv);
  inv = vmulq_f32(vrecpsq_f32(vb, inv), inv);
  vst1q_f32((float*)r._, vmulq_f32(va, inv));
#endif
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_div(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  float* pr = (float*)r._;
  pr[0] = pa[0] / pb[0];
  pr[1] = pa[1] / pb[1];
  pr[2] = pa[2] / pb[2];
  pr[3] = pa[3] / pb[3];
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_neg_f32x4(psimd_f32x4 a) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_xor_ps(psimd__to_m128(a), _mm_set1_ps(-0.0f)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vnegq_f32(vld1q_f32((const float*)a._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_f32x4_neg(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float* pa = (const float*)a._;
  float* pr = (float*)r._;
  pr[0] = -pa[0];
  pr[1] = -pa[1];
  pr[2] = -pa[2];
  pr[3] = -pa[3];
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_abs_f32x4(psimd_f32x4 a) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  __m128i mask = _mm_set1_epi32(0x7fffffff);
  return psimd__from_m128(
      _mm_and_ps(psimd__to_m128(a), _mm_castsi128_ps(mask)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vabsq_f32(vld1q_f32((const float*)a._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_f32x4_abs(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float* pa = (const float*)a._;
  float* pr = (float*)r._;
  pr[0] = fabsf(pa[0]);
  pr[1] = fabsf(pa[1]);
  pr[2] = fabsf(pa[2]);
  pr[3] = fabsf(pa[3]);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_sqrt_f32x4(psimd_f32x4 a) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_sqrt_ps(psimd__to_m128(a)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  float32x4_t va = vld1q_f32((const float*)a._);
#if defined(__aarch64__)
  vst1q_f32((float*)r._, vsqrtq_f32(va));
#else
  float32x4_t est = vrsqrteq_f32(va);
  est = vmulq_f32(vrsqrtsq_f32(vmulq_f32(va, est), est), est);
  est = vmulq_f32(vrsqrtsq_f32(vmulq_f32(va, est), est), est);
  vst1q_f32((float*)r._, vmulq_f32(va, est));
#endif
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_f32x4_sqrt(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float* pa = (const float*)a._;
  float* pr = (float*)r._;
  pr[0] = sqrtf(pa[0]);
  pr[1] = sqrtf(pa[1]);
  pr[2] = sqrtf(pa[2]);
  pr[3] = sqrtf(pa[3]);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_rsqrt_f32x4(psimd_f32x4 a) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_rsqrt_ps(psimd__to_m128(a)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  float32x4_t va = vld1q_f32((const float*)a._);
  float32x4_t est = vrsqrteq_f32(va);
  est = vmulq_f32(vrsqrtsq_f32(vmulq_f32(va, est), est), est);
  vst1q_f32((float*)r._, est);
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  const float* pa = (const float*)a._;
  float* pr = (float*)r._;
  pr[0] = 1.0f / sqrtf(pa[0]);
  pr[1] = 1.0f / sqrtf(pa[1]);
  pr[2] = 1.0f / sqrtf(pa[2]);
  pr[3] = 1.0f / sqrtf(pa[3]);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float* pa = (const float*)a._;
  float* pr = (float*)r._;
  pr[0] = 1.0f / sqrtf(pa[0]);
  pr[1] = 1.0f / sqrtf(pa[1]);
  pr[2] = 1.0f / sqrtf(pa[2]);
  pr[3] = 1.0f / sqrtf(pa[3]);
  return r;
#endif
}

/* ============================================================
   f32x4 — min / max
   ============================================================ */

PSIMD_INLINE psimd_f32x4 psimd_min_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_min_ps(psimd__to_m128(a), psimd__to_m128(b)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vminq_f32(vld1q_f32((const float*)a._),
                                   vld1q_f32((const float*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_min(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  float* pr = (float*)r._;
  pr[0] = pa[0] < pb[0] ? pa[0] : pb[0];
  pr[1] = pa[1] < pb[1] ? pa[1] : pb[1];
  pr[2] = pa[2] < pb[2] ? pa[2] : pb[2];
  pr[3] = pa[3] < pb[3] ? pa[3] : pb[3];
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_max_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_max_ps(psimd__to_m128(a), psimd__to_m128(b)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vmaxq_f32(vld1q_f32((const float*)a._),
                                   vld1q_f32((const float*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_max(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  float* pr = (float*)r._;
  pr[0] = pa[0] > pb[0] ? pa[0] : pb[0];
  pr[1] = pa[1] > pb[1] ? pa[1] : pb[1];
  pr[2] = pa[2] > pb[2] ? pa[2] : pb[2];
  pr[3] = pa[3] > pb[3] ? pa[3] : pb[3];
  return r;
#endif
}

/* ============================================================
   f32x4 — FMA
   ============================================================ */

PSIMD_INLINE psimd_f32x4 psimd_fma_f32x4(psimd_f32x4 a, psimd_f32x4 b,
                                         psimd_f32x4 c) {
#if defined(PSIMD_FMA)
  return psimd__from_m128(
      _mm_fmadd_ps(psimd__to_m128(a), psimd__to_m128(b), psimd__to_m128(c)));
#elif defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_add_ps(
      _mm_mul_ps(psimd__to_m128(a), psimd__to_m128(b)), psimd__to_m128(c)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  float32x4_t va = vld1q_f32((const float*)a._);
  float32x4_t vb = vld1q_f32((const float*)b._);
  float32x4_t vc = vld1q_f32((const float*)c._);
  vst1q_f32((float*)r._, vmlaq_f32(vc, va, vb));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va, vb, vc, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  memcpy(&vc, c._, 16);
  vr = wasm_f32x4_add(wasm_f32x4_mul(va, vb), vc);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._,
              *pc = (const float*)c._;
  float* pr = (float*)r._;
  pr[0] = pa[0] * pb[0] + pc[0];
  pr[1] = pa[1] * pb[1] + pc[1];
  pr[2] = pa[2] * pb[2] + pc[2];
  pr[3] = pa[3] * pb[3] + pc[3];
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_fnma_f32x4(psimd_f32x4 a, psimd_f32x4 b,
                                          psimd_f32x4 c) {
#if defined(PSIMD_FMA)
  return psimd__from_m128(
      _mm_fnmadd_ps(psimd__to_m128(a), psimd__to_m128(b), psimd__to_m128(c)));
#elif defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_sub_ps(
      psimd__to_m128(c), _mm_mul_ps(psimd__to_m128(a), psimd__to_m128(b))));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  float32x4_t va = vld1q_f32((const float*)a._);
  float32x4_t vb = vld1q_f32((const float*)b._);
  float32x4_t vc = vld1q_f32((const float*)c._);
  vst1q_f32((float*)r._, vmlsq_f32(vc, va, vb));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va, vb, vc, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  memcpy(&vc, c._, 16);
  vr = wasm_f32x4_sub(vc, wasm_f32x4_mul(va, vb));
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._,
              *pc = (const float*)c._;
  float* pr = (float*)r._;
  pr[0] = -(pa[0] * pb[0]) + pc[0];
  pr[1] = -(pa[1] * pb[1]) + pc[1];
  pr[2] = -(pa[2] * pb[2]) + pc[2];
  pr[3] = -(pa[3] * pb[3]) + pc[3];
  return r;
#endif
}

/* ============================================================
   f32x4 — comparisons
   ============================================================ */

PSIMD_INLINE psimd_mask32x4 psimd_cmpeq_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_castps_si128(_mm_cmpeq_ps(psimd__to_m128(a), psimd__to_m128(b))));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vceqq_f32(vld1q_f32((const float*)a._),
                                      vld1q_f32((const float*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_eq(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] == pb[0] ? psimd__mask32_true_bits : 0u;
  pr[1] = pa[1] == pb[1] ? psimd__mask32_true_bits : 0u;
  pr[2] = pa[2] == pb[2] ? psimd__mask32_true_bits : 0u;
  pr[3] = pa[3] == pb[3] ? psimd__mask32_true_bits : 0u;
  return r;
#endif
}

PSIMD_INLINE psimd_mask32x4 psimd_cmpne_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_castps_si128(_mm_cmpneq_ps(psimd__to_m128(a), psimd__to_m128(b))));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vmvnq_u32(vceqq_f32(vld1q_f32((const float*)a._),
                                                vld1q_f32((const float*)b._))));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_ne(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] != pb[0] ? psimd__mask32_true_bits : 0u;
  pr[1] = pa[1] != pb[1] ? psimd__mask32_true_bits : 0u;
  pr[2] = pa[2] != pb[2] ? psimd__mask32_true_bits : 0u;
  pr[3] = pa[3] != pb[3] ? psimd__mask32_true_bits : 0u;
  return r;
#endif
}

PSIMD_INLINE psimd_mask32x4 psimd_cmplt_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_castps_si128(_mm_cmplt_ps(psimd__to_m128(a), psimd__to_m128(b))));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vcltq_f32(vld1q_f32((const float*)a._),
                                      vld1q_f32((const float*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_lt(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] < pb[0] ? psimd__mask32_true_bits : 0u;
  pr[1] = pa[1] < pb[1] ? psimd__mask32_true_bits : 0u;
  pr[2] = pa[2] < pb[2] ? psimd__mask32_true_bits : 0u;
  pr[3] = pa[3] < pb[3] ? psimd__mask32_true_bits : 0u;
  return r;
#endif
}

PSIMD_INLINE psimd_mask32x4 psimd_cmple_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_castps_si128(_mm_cmple_ps(psimd__to_m128(a), psimd__to_m128(b))));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vcleq_f32(vld1q_f32((const float*)a._),
                                      vld1q_f32((const float*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_le(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] <= pb[0] ? psimd__mask32_true_bits : 0u;
  pr[1] = pa[1] <= pb[1] ? psimd__mask32_true_bits : 0u;
  pr[2] = pa[2] <= pb[2] ? psimd__mask32_true_bits : 0u;
  pr[3] = pa[3] <= pb[3] ? psimd__mask32_true_bits : 0u;
  return r;
#endif
}

PSIMD_INLINE psimd_mask32x4 psimd_cmpgt_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_castps_si128(_mm_cmpgt_ps(psimd__to_m128(a), psimd__to_m128(b))));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vcgtq_f32(vld1q_f32((const float*)a._),
                                      vld1q_f32((const float*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_gt(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] > pb[0] ? psimd__mask32_true_bits : 0u;
  pr[1] = pa[1] > pb[1] ? psimd__mask32_true_bits : 0u;
  pr[2] = pa[2] > pb[2] ? psimd__mask32_true_bits : 0u;
  pr[3] = pa[3] > pb[3] ? psimd__mask32_true_bits : 0u;
  return r;
#endif
}

PSIMD_INLINE psimd_mask32x4 psimd_cmpge_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_castps_si128(_mm_cmpge_ps(psimd__to_m128(a), psimd__to_m128(b))));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vcgeq_f32(vld1q_f32((const float*)a._),
                                      vld1q_f32((const float*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_f32x4_ge(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] >= pb[0] ? psimd__mask32_true_bits : 0u;
  pr[1] = pa[1] >= pb[1] ? psimd__mask32_true_bits : 0u;
  pr[2] = pa[2] >= pb[2] ? psimd__mask32_true_bits : 0u;
  pr[3] = pa[3] >= pb[3] ? psimd__mask32_true_bits : 0u;
  return r;
#endif
}

/* ============================================================
   f32x4 — select / blend
   ============================================================ */

PSIMD_INLINE psimd_f32x4 psimd_select_f32x4(psimd_mask32x4 m, psimd_f32x4 a,
                                            psimd_f32x4 b) {
#if defined(PSIMD_SSE41) || defined(PSIMD_AVX)
  __m128 mf = _mm_castsi128_ps(psimd__mask_to_m128i(m));
  return psimd__from_m128(
      _mm_blendv_ps(psimd__to_m128(b), psimd__to_m128(a), mf));
#elif defined(PSIMD_SSE2)
  __m128i mi = psimd__mask_to_m128i(m);
  __m128i ia = _mm_castps_si128(psimd__to_m128(a));
  __m128i ib = _mm_castps_si128(psimd__to_m128(b));
  __m128i r = _mm_or_si128(_mm_and_si128(mi, ia), _mm_andnot_si128(mi, ib));
  return psimd__from_m128(_mm_castsi128_ps(r));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  uint32x4_t vm = vld1q_u32((const uint32_t*)m._);
  float32x4_t va = vld1q_f32((const float*)a._);
  float32x4_t vb = vld1q_f32((const float*)b._);
  vst1q_f32((float*)r._, vbslq_f32(vm, va, vb));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t vm, va, vb, vr;
  memcpy(&vm, m._, 16);
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_v128_bitselect(va, vb, vm);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const uint32_t* pm = (const uint32_t*)m._;
  const uint32_t* pa = (const uint32_t*)a._;
  const uint32_t* pb = (const uint32_t*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = (pm[0] & pa[0]) | (~pm[0] & pb[0]);
  pr[1] = (pm[1] & pa[1]) | (~pm[1] & pb[1]);
  pr[2] = (pm[2] & pa[2]) | (~pm[2] & pb[2]);
  pr[3] = (pm[3] & pa[3]) | (~pm[3] & pb[3]);
  return r;
#endif
}

/* ============================================================
   f32x4 — reductions
   ============================================================ */

PSIMD_INLINE float psimd_reduce_add_f32x4(psimd_f32x4 v) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  __m128 t = psimd__to_m128(v);
  __m128 s = _mm_add_ps(t, _mm_movehl_ps(t, t));
  s = _mm_add_ss(s, _mm_shuffle_ps(s, s, 0x55));
  return _mm_cvtss_f32(s);
#elif defined(PSIMD_NEON)
  float32x4_t vv = vld1q_f32((const float*)v._);
  float32x2_t lo = vget_low_f32(vv);
  float32x2_t hi = vget_high_f32(vv);
  float32x2_t sum = vadd_f32(lo, hi);
  sum = vpadd_f32(sum, sum);
  return vget_lane_f32(sum, 0);
#elif defined(PSIMD_WASM)
  const float* p = (const float*)v._;
  return p[0] + p[1] + p[2] + p[3];
#else
  const float* p = (const float*)v._;
  return p[0] + p[1] + p[2] + p[3];
#endif
}

PSIMD_INLINE float psimd_reduce_mul_f32x4(psimd_f32x4 v) {
  const float* p = (const float*)v._;
  return p[0] * p[1] * p[2] * p[3];
}

PSIMD_INLINE float psimd_reduce_max_f32x4(psimd_f32x4 v) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  __m128 t = psimd__to_m128(v);
  __m128 s = _mm_max_ps(t, _mm_movehl_ps(t, t));
  s = _mm_max_ss(s, _mm_shuffle_ps(s, s, 0x55));
  return _mm_cvtss_f32(s);
#elif defined(PSIMD_NEON)
  float32x4_t vv = vld1q_f32((const float*)v._);
  float32x2_t lo = vget_low_f32(vv);
  float32x2_t hi = vget_high_f32(vv);
  float32x2_t mx = vpmax_f32(lo, hi);
  mx = vpmax_f32(mx, mx);
  return vget_lane_f32(mx, 0);
#else
  const float* p = (const float*)v._;
  float m = p[0];
  if (p[1] > m)
    m = p[1];
  if (p[2] > m)
    m = p[2];
  if (p[3] > m)
    m = p[3];
  return m;
#endif
}

PSIMD_INLINE float psimd_reduce_min_f32x4(psimd_f32x4 v) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  __m128 t = psimd__to_m128(v);
  __m128 s = _mm_min_ps(t, _mm_movehl_ps(t, t));
  s = _mm_min_ss(s, _mm_shuffle_ps(s, s, 0x55));
  return _mm_cvtss_f32(s);
#elif defined(PSIMD_NEON)
  float32x4_t vv = vld1q_f32((const float*)v._);
  float32x2_t lo = vget_low_f32(vv);
  float32x2_t hi = vget_high_f32(vv);
  float32x2_t mn = vpmin_f32(lo, hi);
  mn = vpmin_f32(mn, mn);
  return vget_lane_f32(mn, 0);
#else
  const float* p = (const float*)v._;
  float m = p[0];
  if (p[1] < m)
    m = p[1];
  if (p[2] < m)
    m = p[2];
  if (p[3] < m)
    m = p[3];
  return m;
#endif
}

/* ============================================================
   f32x4 — lane access
   ============================================================ */

PSIMD_INLINE float psimd_get_lane_f32x4(psimd_f32x4 v, int i) {
  return ((const float*)v._)[i];
}

PSIMD_INLINE psimd_f32x4 psimd_set_lane_f32x4(psimd_f32x4 v, int i, float x) {
  float tmp[4];
  psimd_storeu_f32x4(tmp, v);
  tmp[i] = x;
  return psimd_loadu_f32x4(tmp);
}

/* ============================================================
   mask32x4 — logical operations
   ============================================================ */

PSIMD_INLINE psimd_mask32x4 psimd_and_mask32x4(psimd_mask32x4 a,
                                               psimd_mask32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_and_si128(psimd__mask_to_m128i(a), psimd__mask_to_m128i(b)));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vandq_u32(vld1q_u32((const uint32_t*)a._),
                                      vld1q_u32((const uint32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_v128_and(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const uint32_t *pa = (const uint32_t*)a._, *pb = (const uint32_t*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] & pb[0];
  pr[1] = pa[1] & pb[1];
  pr[2] = pa[2] & pb[2];
  pr[3] = pa[3] & pb[3];
  return r;
#endif
}

PSIMD_INLINE psimd_mask32x4 psimd_or_mask32x4(psimd_mask32x4 a,
                                              psimd_mask32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_or_si128(psimd__mask_to_m128i(a), psimd__mask_to_m128i(b)));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vorrq_u32(vld1q_u32((const uint32_t*)a._),
                                      vld1q_u32((const uint32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_v128_or(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const uint32_t *pa = (const uint32_t*)a._, *pb = (const uint32_t*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] | pb[0];
  pr[1] = pa[1] | pb[1];
  pr[2] = pa[2] | pb[2];
  pr[3] = pa[3] | pb[3];
  return r;
#endif
}

PSIMD_INLINE psimd_mask32x4 psimd_not_mask32x4(psimd_mask32x4 a) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  __m128i ones = _mm_set1_epi32(-1);
  return psimd__from_m128i_mask(_mm_xor_si128(psimd__mask_to_m128i(a), ones));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vmvnq_u32(vld1q_u32((const uint32_t*)a._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_v128_not(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const uint32_t* pa = (const uint32_t*)a._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = ~pa[0];
  pr[1] = ~pa[1];
  pr[2] = ~pa[2];
  pr[3] = ~pa[3];
  return r;
#endif
}

PSIMD_INLINE psimd_mask32x4 psimd_xor_mask32x4(psimd_mask32x4 a,
                                               psimd_mask32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_xor_si128(psimd__mask_to_m128i(a), psimd__mask_to_m128i(b)));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, veorq_u32(vld1q_u32((const uint32_t*)a._),
                                      vld1q_u32((const uint32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_v128_xor(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const uint32_t *pa = (const uint32_t*)a._, *pb = (const uint32_t*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] ^ pb[0];
  pr[1] = pa[1] ^ pb[1];
  pr[2] = pa[2] ^ pb[2];
  pr[3] = pa[3] ^ pb[3];
  return r;
#endif
}

[[nodiscard]] PSIMD_INLINE bool psimd_any_mask32x4(psimd_mask32x4 m) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return _mm_movemask_ps(_mm_castsi128_ps(psimd__mask_to_m128i(m))) != 0;
#elif defined(PSIMD_NEON)
  uint32x4_t vm = vld1q_u32((const uint32_t*)m._);
  uint32x2_t r = vorr_u32(vget_low_u32(vm), vget_high_u32(vm));
  return vget_lane_u32(vpmax_u32(r, r), 0) != 0;
#else
  const uint32_t* pm = (const uint32_t*)m._;
  return (pm[0] | pm[1] | pm[2] | pm[3]) != 0;
#endif
}

[[nodiscard]] PSIMD_INLINE bool psimd_all_mask32x4(psimd_mask32x4 m) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return _mm_movemask_ps(_mm_castsi128_ps(psimd__mask_to_m128i(m))) == 0xf;
#elif defined(PSIMD_NEON)
  uint32x4_t vm = vld1q_u32((const uint32_t*)m._);
  uint32x2_t r = vand_u32(vget_low_u32(vm), vget_high_u32(vm));
  return vget_lane_u32(vpmin_u32(r, r), 0) == psimd__mask32_true_bits;
#else
  const uint32_t* pm = (const uint32_t*)m._;
  return (pm[0] & pm[1] & pm[2] & pm[3]) == psimd__mask32_true_bits;
#endif
}

[[nodiscard]] PSIMD_INLINE psimd_mask32x4 psimd_true_mask32x4() {
  psimd_mask32x4 r = {{0}};
  memset(r._, 0xff, 16);
  return r;
}

[[nodiscard]] PSIMD_INLINE psimd_mask32x4 psimd_false_mask32x4() {
  psimd_mask32x4 r = {{0}};
  memset(r._, 0, 16);
  return r;
}

/* ============================================================
   i32x4 — set / load / store
   ============================================================ */

PSIMD_INLINE psimd_i32x4 psimd_set1_i32x4(int32_t x) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(_mm_set1_epi32(x));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vdupq_n_s32(x));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t v = wasm_i32x4_splat(x);
  memcpy(r._, &v, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  int32_t* p = (int32_t*)r._;
  p[0] = p[1] = p[2] = p[3] = x;
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_set_i32x4(int32_t x0, int32_t x1, int32_t x2,
                                         int32_t x3) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(_mm_set_epi32(x3, x2, x1, x0));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  int32_t tmp[4] = {x0, x1, x2, x3};
  vst1q_s32((int32_t*)r._, vld1q_s32(tmp));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t v = wasm_i32x4_make(x0, x1, x2, x3);
  memcpy(r._, &v, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  int32_t* p = (int32_t*)r._;
  p[0] = x0;
  p[1] = x1;
  p[2] = x2;
  p[3] = x3;
  return r;
#endif
}

[[nodiscard]] PSIMD_INLINE psimd_i32x4 psimd_zero_i32x4() {
  psimd_i32x4 r = {{0}};
  memset(r._, 0, 16);
  return r;
}

PSIMD_INLINE psimd_i32x4 psimd_loadu_i32x4(const int32_t* p) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(_mm_loadu_si128((const __m128i*)p));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vld1q_s32(p));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t v = wasm_v128_load(p);
  memcpy(r._, &v, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  memcpy(r._, p, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_loada_i32x4(const int32_t* p) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(_mm_load_si128((const __m128i*)p));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vld1q_s32(p));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t v = wasm_v128_load(p);
  memcpy(r._, &v, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  memcpy(r._, p, 16);
  return r;
#endif
}

PSIMD_INLINE void psimd_storeu_i32x4(int32_t* p, psimd_i32x4 v) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  _mm_storeu_si128((__m128i*)p, psimd__to_m128i(v));
#elif defined(PSIMD_NEON)
  vst1q_s32(p, vld1q_s32((const int32_t*)v._));
#elif defined(PSIMD_WASM)
  v128_t tmp;
  memcpy(&tmp, v._, 16);
  wasm_v128_store(p, tmp);
#else
  memcpy(p, v._, 16);
#endif
}

PSIMD_INLINE void psimd_storea_i32x4(int32_t* p, psimd_i32x4 v) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  _mm_store_si128((__m128i*)p, psimd__to_m128i(v));
#elif defined(PSIMD_NEON)
  vst1q_s32(p, vld1q_s32((const int32_t*)v._));
#elif defined(PSIMD_WASM)
  v128_t tmp;
  memcpy(&tmp, v._, 16);
  wasm_v128_store(p, tmp);
#else
  memcpy(p, v._, 16);
#endif
}

/* ============================================================
   i32x4 — arithmetic
   ============================================================ */

PSIMD_INLINE psimd_i32x4 psimd_add_i32x4(psimd_i32x4 a, psimd_i32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(
      _mm_add_epi32(psimd__to_m128i(a), psimd__to_m128i(b)));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vaddq_s32(vld1q_s32((const int32_t*)a._),
                                     vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_i32x4_add(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  uint32_t pa[4], pb[4], pr[4];
  memcpy(pa, a._, 16);
  memcpy(pb, b._, 16);
  pr[0] = pa[0] + pb[0];
  pr[1] = pa[1] + pb[1];
  pr[2] = pa[2] + pb[2];
  pr[3] = pa[3] + pb[3];
  memcpy(r._, pr, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_sub_i32x4(psimd_i32x4 a, psimd_i32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(
      _mm_sub_epi32(psimd__to_m128i(a), psimd__to_m128i(b)));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vsubq_s32(vld1q_s32((const int32_t*)a._),
                                     vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_i32x4_sub(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  uint32_t pa[4], pb[4], pr[4];
  memcpy(pa, a._, 16);
  memcpy(pb, b._, 16);
  pr[0] = pa[0] - pb[0];
  pr[1] = pa[1] - pb[1];
  pr[2] = pa[2] - pb[2];
  pr[3] = pa[3] - pb[3];
  memcpy(r._, pr, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_mul_i32x4(psimd_i32x4 a, psimd_i32x4 b) {
#if defined(PSIMD_SSE41) || defined(PSIMD_AVX)
  return psimd__from_m128i(
      _mm_mullo_epi32(psimd__to_m128i(a), psimd__to_m128i(b)));
#elif defined(PSIMD_SSE2)
  __m128i va = psimd__to_m128i(a), vb = psimd__to_m128i(b);
  __m128i lo = _mm_mul_epu32(va, vb);
  __m128i hi = _mm_mul_epu32(_mm_srli_si128(va, 4), _mm_srli_si128(vb, 4));
  return psimd__from_m128i(
      _mm_unpacklo_epi32(_mm_shuffle_epi32(lo, _MM_SHUFFLE(0, 0, 2, 0)),
                         _mm_shuffle_epi32(hi, _MM_SHUFFLE(0, 0, 2, 0))));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vmulq_s32(vld1q_s32((const int32_t*)a._),
                                     vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_i32x4_mul(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  uint32_t pa[4], pb[4], pr[4];
  memcpy(pa, a._, 16);
  memcpy(pb, b._, 16);
  pr[0] = pa[0] * pb[0];
  pr[1] = pa[1] * pb[1];
  pr[2] = pa[2] * pb[2];
  pr[3] = pa[3] * pb[3];
  memcpy(r._, pr, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_neg_i32x4(psimd_i32x4 a) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(
      _mm_sub_epi32(_mm_setzero_si128(), psimd__to_m128i(a)));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vnegq_s32(vld1q_s32((const int32_t*)a._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_i32x4_neg(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  uint32_t pa[4], pr[4];
  memcpy(pa, a._, 16);
  pr[0] = 0u - pa[0];
  pr[1] = 0u - pa[1];
  pr[2] = 0u - pa[2];
  pr[3] = 0u - pa[3];
  memcpy(r._, pr, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_abs_i32x4(psimd_i32x4 a) {
#if defined(PSIMD_SSE41) || defined(PSIMD_AVX)
  return psimd__from_m128i(_mm_abs_epi32(psimd__to_m128i(a)));
#elif defined(PSIMD_SSE2)
  __m128i v = psimd__to_m128i(a);
  __m128i neg = _mm_sub_epi32(_mm_setzero_si128(), v);
  __m128i mask = _mm_cmplt_epi32(v, _mm_setzero_si128());
  return psimd__from_m128i(
      _mm_or_si128(_mm_and_si128(mask, neg), _mm_andnot_si128(mask, v)));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vabsq_s32(vld1q_s32((const int32_t*)a._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_i32x4_abs(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  int32_t pa[4];
  uint32_t pu[4], pr[4];
  memcpy(pa, a._, 16);
  memcpy(pu, a._, 16);
  pr[0] = pa[0] < 0 ? 0u - pu[0] : pu[0];
  pr[1] = pa[1] < 0 ? 0u - pu[1] : pu[1];
  pr[2] = pa[2] < 0 ? 0u - pu[2] : pu[2];
  pr[3] = pa[3] < 0 ? 0u - pu[3] : pu[3];
  memcpy(r._, pr, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_min_i32x4(psimd_i32x4 a, psimd_i32x4 b) {
#if defined(PSIMD_SSE41) || defined(PSIMD_AVX)
  return psimd__from_m128i(
      _mm_min_epi32(psimd__to_m128i(a), psimd__to_m128i(b)));
#elif defined(PSIMD_SSE2)
  __m128i va = psimd__to_m128i(a), vb = psimd__to_m128i(b);
  __m128i mask = _mm_cmplt_epi32(va, vb);
  return psimd__from_m128i(
      _mm_or_si128(_mm_and_si128(mask, va), _mm_andnot_si128(mask, vb)));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vminq_s32(vld1q_s32((const int32_t*)a._),
                                     vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_i32x4_min(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  const int32_t *pa = (const int32_t*)a._, *pb = (const int32_t*)b._;
  int32_t* pr = (int32_t*)r._;
  pr[0] = pa[0] < pb[0] ? pa[0] : pb[0];
  pr[1] = pa[1] < pb[1] ? pa[1] : pb[1];
  pr[2] = pa[2] < pb[2] ? pa[2] : pb[2];
  pr[3] = pa[3] < pb[3] ? pa[3] : pb[3];
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_max_i32x4(psimd_i32x4 a, psimd_i32x4 b) {
#if defined(PSIMD_SSE41) || defined(PSIMD_AVX)
  return psimd__from_m128i(
      _mm_max_epi32(psimd__to_m128i(a), psimd__to_m128i(b)));
#elif defined(PSIMD_SSE2)
  __m128i va = psimd__to_m128i(a), vb = psimd__to_m128i(b);
  __m128i mask = _mm_cmpgt_epi32(va, vb);
  return psimd__from_m128i(
      _mm_or_si128(_mm_and_si128(mask, va), _mm_andnot_si128(mask, vb)));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vmaxq_s32(vld1q_s32((const int32_t*)a._),
                                     vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_i32x4_max(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  const int32_t *pa = (const int32_t*)a._, *pb = (const int32_t*)b._;
  int32_t* pr = (int32_t*)r._;
  pr[0] = pa[0] > pb[0] ? pa[0] : pb[0];
  pr[1] = pa[1] > pb[1] ? pa[1] : pb[1];
  pr[2] = pa[2] > pb[2] ? pa[2] : pb[2];
  pr[3] = pa[3] > pb[3] ? pa[3] : pb[3];
  return r;
#endif
}

/* ============================================================
   i32x4 — bitwise
   ============================================================ */

PSIMD_INLINE psimd_i32x4 psimd_and_i32x4(psimd_i32x4 a, psimd_i32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(
      _mm_and_si128(psimd__to_m128i(a), psimd__to_m128i(b)));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vandq_s32(vld1q_s32((const int32_t*)a._),
                                     vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_v128_and(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  const uint32_t *pa = (const uint32_t*)a._, *pb = (const uint32_t*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] & pb[0];
  pr[1] = pa[1] & pb[1];
  pr[2] = pa[2] & pb[2];
  pr[3] = pa[3] & pb[3];
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_or_i32x4(psimd_i32x4 a, psimd_i32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(
      _mm_or_si128(psimd__to_m128i(a), psimd__to_m128i(b)));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vorrq_s32(vld1q_s32((const int32_t*)a._),
                                     vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_v128_or(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  const uint32_t *pa = (const uint32_t*)a._, *pb = (const uint32_t*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] | pb[0];
  pr[1] = pa[1] | pb[1];
  pr[2] = pa[2] | pb[2];
  pr[3] = pa[3] | pb[3];
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_xor_i32x4(psimd_i32x4 a, psimd_i32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(
      _mm_xor_si128(psimd__to_m128i(a), psimd__to_m128i(b)));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, veorq_s32(vld1q_s32((const int32_t*)a._),
                                     vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_v128_xor(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  const uint32_t *pa = (const uint32_t*)a._, *pb = (const uint32_t*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] ^ pb[0];
  pr[1] = pa[1] ^ pb[1];
  pr[2] = pa[2] ^ pb[2];
  pr[3] = pa[3] ^ pb[3];
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_not_i32x4(psimd_i32x4 a) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(
      _mm_xor_si128(psimd__to_m128i(a), _mm_set1_epi32(-1)));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vmvnq_s32(vld1q_s32((const int32_t*)a._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_v128_not(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  const uint32_t* pa = (const uint32_t*)a._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = ~pa[0];
  pr[1] = ~pa[1];
  pr[2] = ~pa[2];
  pr[3] = ~pa[3];
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_shl_i32x4(psimd_i32x4 a, int n) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(_mm_slli_epi32(psimd__to_m128i(a), n));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._,
            vshlq_s32(vld1q_s32((const int32_t*)a._), vdupq_n_s32(n)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_i32x4_shl(va, n);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  const uint32_t* pa = (const uint32_t*)a._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] << n;
  pr[1] = pa[1] << n;
  pr[2] = pa[2] << n;
  pr[3] = pa[3] << n;
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_shr_i32x4(psimd_i32x4 a, int n) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(_mm_srai_epi32(psimd__to_m128i(a), n));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._,
            vshlq_s32(vld1q_s32((const int32_t*)a._), vdupq_n_s32(-n)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_i32x4_shr(va, n);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  const int32_t* pa = (const int32_t*)a._;
  int32_t* pr = (int32_t*)r._;
  pr[0] = pa[0] >> n;
  pr[1] = pa[1] >> n;
  pr[2] = pa[2] >> n;
  pr[3] = pa[3] >> n;
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_shru_i32x4(psimd_i32x4 a, int n) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(_mm_srli_epi32(psimd__to_m128i(a), n));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._,
            vshlq_u32(vld1q_u32((const uint32_t*)a._), vdupq_n_s32(-n)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_u32x4_shr(va, n);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  const uint32_t* pa = (const uint32_t*)a._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] >> n;
  pr[1] = pa[1] >> n;
  pr[2] = pa[2] >> n;
  pr[3] = pa[3] >> n;
  return r;
#endif
}

/* ============================================================
   i32x4 — comparisons
   ============================================================ */

PSIMD_INLINE psimd_mask32x4 psimd_cmpeq_i32x4(psimd_i32x4 a, psimd_i32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_cmpeq_epi32(psimd__to_m128i(a), psimd__to_m128i(b)));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vceqq_s32(vld1q_s32((const int32_t*)a._),
                                      vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_i32x4_eq(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const int32_t *pa = (const int32_t*)a._, *pb = (const int32_t*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] == pb[0] ? psimd__mask32_true_bits : 0u;
  pr[1] = pa[1] == pb[1] ? psimd__mask32_true_bits : 0u;
  pr[2] = pa[2] == pb[2] ? psimd__mask32_true_bits : 0u;
  pr[3] = pa[3] == pb[3] ? psimd__mask32_true_bits : 0u;
  return r;
#endif
}

PSIMD_INLINE psimd_mask32x4 psimd_cmplt_i32x4(psimd_i32x4 a, psimd_i32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_cmplt_epi32(psimd__to_m128i(a), psimd__to_m128i(b)));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vcltq_s32(vld1q_s32((const int32_t*)a._),
                                      vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_i32x4_lt(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const int32_t *pa = (const int32_t*)a._, *pb = (const int32_t*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] < pb[0] ? psimd__mask32_true_bits : 0u;
  pr[1] = pa[1] < pb[1] ? psimd__mask32_true_bits : 0u;
  pr[2] = pa[2] < pb[2] ? psimd__mask32_true_bits : 0u;
  pr[3] = pa[3] < pb[3] ? psimd__mask32_true_bits : 0u;
  return r;
#endif
}

PSIMD_INLINE psimd_mask32x4 psimd_cmpgt_i32x4(psimd_i32x4 a, psimd_i32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i_mask(
      _mm_cmpgt_epi32(psimd__to_m128i(a), psimd__to_m128i(b)));
#elif defined(PSIMD_NEON)
  psimd_mask32x4 r = {{0}};
  vst1q_u32((uint32_t*)r._, vcgtq_s32(vld1q_s32((const int32_t*)a._),
                                      vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_mask32x4 r = {{0}};
  v128_t va, vb, vr;
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_i32x4_gt(va, vb);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_mask32x4 r = {{0}};
  const int32_t *pa = (const int32_t*)a._, *pb = (const int32_t*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = pa[0] > pb[0] ? psimd__mask32_true_bits : 0u;
  pr[1] = pa[1] > pb[1] ? psimd__mask32_true_bits : 0u;
  pr[2] = pa[2] > pb[2] ? psimd__mask32_true_bits : 0u;
  pr[3] = pa[3] > pb[3] ? psimd__mask32_true_bits : 0u;
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_select_i32x4(psimd_mask32x4 m, psimd_i32x4 a,
                                            psimd_i32x4 b) {
#if defined(PSIMD_SSE41) || defined(PSIMD_AVX)
  return psimd__from_m128i(_mm_blendv_epi8(
      psimd__to_m128i(b), psimd__to_m128i(a), psimd__mask_to_m128i(m)));
#elif defined(PSIMD_SSE2)
  __m128i mi = psimd__mask_to_m128i(m);
  return psimd__from_m128i(
      _mm_or_si128(_mm_and_si128(mi, psimd__to_m128i(a)),
                   _mm_andnot_si128(mi, psimd__to_m128i(b))));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vbslq_s32(vld1q_u32((const uint32_t*)m._),
                                     vld1q_s32((const int32_t*)a._),
                                     vld1q_s32((const int32_t*)b._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t vm, va, vb, vr;
  memcpy(&vm, m._, 16);
  memcpy(&va, a._, 16);
  memcpy(&vb, b._, 16);
  vr = wasm_v128_bitselect(va, vb, vm);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  const uint32_t *pm = (const uint32_t*)m._, *pa = (const uint32_t*)a._,
                 *pb = (const uint32_t*)b._;
  uint32_t* pr = (uint32_t*)r._;
  pr[0] = (pm[0] & pa[0]) | (~pm[0] & pb[0]);
  pr[1] = (pm[1] & pa[1]) | (~pm[1] & pb[1]);
  pr[2] = (pm[2] & pa[2]) | (~pm[2] & pb[2]);
  pr[3] = (pm[3] & pa[3]) | (~pm[3] & pb[3]);
  return r;
#endif
}

/* ============================================================
   i32x4 — reductions
   ============================================================ */

PSIMD_INLINE int32_t psimd_reduce_add_i32x4(psimd_i32x4 v) {
  uint32_t p[4];
  memcpy(p, v._, 16);
  return psimd__i32_from_u32(p[0] + p[1] + p[2] + p[3]);
}

PSIMD_INLINE int32_t psimd_reduce_max_i32x4(psimd_i32x4 v) {
  const int32_t* p = (const int32_t*)v._;
  int32_t m = p[0];
  if (p[1] > m)
    m = p[1];
  if (p[2] > m)
    m = p[2];
  if (p[3] > m)
    m = p[3];
  return m;
}

PSIMD_INLINE int32_t psimd_reduce_min_i32x4(psimd_i32x4 v) {
  const int32_t* p = (const int32_t*)v._;
  int32_t m = p[0];
  if (p[1] < m)
    m = p[1];
  if (p[2] < m)
    m = p[2];
  if (p[3] < m)
    m = p[3];
  return m;
}

/* ============================================================
   type conversions
   ============================================================ */

PSIMD_INLINE psimd_i32x4 psimd_cvt_f32x4_i32x4(psimd_f32x4 a) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128i(_mm_cvttps_epi32(psimd__to_m128(a)));
#elif defined(PSIMD_NEON)
  psimd_i32x4 r = {{0}};
  vst1q_s32((int32_t*)r._, vcvtq_s32_f32(vld1q_f32((const float*)a._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_i32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_i32x4_trunc_sat_f32x4(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_i32x4 r = {{0}};
  const float* pa = (const float*)a._;
  int32_t* pr = (int32_t*)r._;
  pr[0] = (int32_t)pa[0];
  pr[1] = (int32_t)pa[1];
  pr[2] = (int32_t)pa[2];
  pr[3] = (int32_t)pa[3];
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_cvt_i32x4_f32x4(psimd_i32x4 a) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_cvtepi32_ps(psimd__to_m128i(a)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  vst1q_f32((float*)r._, vcvtq_f32_s32(vld1q_s32((const int32_t*)a._)));
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_f32x4_convert_i32x4(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const int32_t* pa = (const int32_t*)a._;
  float* pr = (float*)r._;
  pr[0] = (float)pa[0];
  pr[1] = (float)pa[1];
  pr[2] = (float)pa[2];
  pr[3] = (float)pa[3];
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_reinterpret_i32x4_f32x4(psimd_i32x4 a) {
  psimd_f32x4 r = {{0}};
  memcpy(r._, a._, 16);
  return r;
}

PSIMD_INLINE psimd_i32x4 psimd_reinterpret_f32x4_i32x4(psimd_f32x4 a) {
  psimd_i32x4 r = {{0}};
  memcpy(r._, a._, 16);
  return r;
}

PSIMD_INLINE psimd_mask32x4 psimd_reinterpret_i32x4_mask32x4(psimd_i32x4 a) {
  psimd_mask32x4 r = {{0}};
  memcpy(r._, a._, 16);
  return r;
}

PSIMD_INLINE psimd_i32x4 psimd_reinterpret_mask32x4_i32x4(psimd_mask32x4 a) {
  psimd_i32x4 r = {{0}};
  memcpy(r._, a._, 16);
  return r;
}

/* ============================================================
   f32x4 — shuffle / permute
   ============================================================ */

PSIMD_INLINE psimd_f32x4 psimd_shuffle_f32x4(psimd_f32x4 a, psimd_f32x4 b,
                                             int i0, int i1, int i2, int i3) {
  psimd_f32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  float* pr = (float*)r._;
  const float* src[8] = {pa, pa, pa, pa, pb, pb, pb, pb};
  pr[0] = src[i0 >> 2][i0 & 3];
  pr[1] = src[i1 >> 2][i1 & 3];
  pr[2] = src[i2 >> 2][i2 & 3];
  pr[3] = src[i3 >> 2][i3 & 3];
  return r;
}

PSIMD_INLINE psimd_f32x4 psimd_zip_lo_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(
      _mm_unpacklo_ps(psimd__to_m128(a), psimd__to_m128(b)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  float32x4x2_t zipped =
      vzipq_f32(vld1q_f32((const float*)a._), vld1q_f32((const float*)b._));
  vst1q_f32((float*)r._, zipped.val[0]);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  float* pr = (float*)r._;
  pr[0] = pa[0];
  pr[1] = pb[0];
  pr[2] = pa[1];
  pr[3] = pb[1];
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_zip_hi_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  return psimd__from_m128(
      _mm_unpackhi_ps(psimd__to_m128(a), psimd__to_m128(b)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = {{0}};
  float32x4x2_t zipped =
      vzipq_f32(vld1q_f32((const float*)a._), vld1q_f32((const float*)b._));
  vst1q_f32((float*)r._, zipped.val[1]);
  return r;
#else
  psimd_f32x4 r = {{0}};
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  float* pr = (float*)r._;
  pr[0] = pa[2];
  pr[1] = pb[2];
  pr[2] = pa[3];
  pr[3] = pb[3];
  return r;
#endif
}

/* ============================================================
   f32x8 — AVX (emulated on non-AVX via two f32x4)
   ============================================================ */

PSIMD_INLINE psimd_f32x8 psimd_set1_f32x8(float x) {
#if defined(PSIMD_AVX)
  return psimd__from_m256(_mm256_set1_ps(x));
#else
  psimd_f32x8 r = {{0}};
  psimd_f32x4 lo = psimd_set1_f32x4(x);
  memcpy(r._, lo._, 16);
  memcpy(r._ + 16, lo._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x8 psimd_loadu_f32x8(const float* p) {
#if defined(PSIMD_AVX)
  return psimd__from_m256(_mm256_loadu_ps(p));
#else
  psimd_f32x8 r = {{0}};
  psimd_f32x4 lo = psimd_loadu_f32x4(p);
  psimd_f32x4 hi = psimd_loadu_f32x4(p + 4);
  memcpy(r._, lo._, 16);
  memcpy(r._ + 16, hi._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x8 psimd_loada_f32x8(const float* p) {
#if defined(PSIMD_AVX)
  return psimd__from_m256(_mm256_load_ps(p));
#else
  psimd_f32x8 r = {{0}};
  psimd_f32x4 lo = psimd_loada_f32x4(p);
  psimd_f32x4 hi = psimd_loada_f32x4(p + 4);
  memcpy(r._, lo._, 16);
  memcpy(r._ + 16, hi._, 16);
  return r;
#endif
}

PSIMD_INLINE void psimd_storeu_f32x8(float* p, psimd_f32x8 v) {
#if defined(PSIMD_AVX)
  _mm256_storeu_ps(p, psimd__to_m256(v));
#else
  psimd_f32x4 lo, hi;
  memcpy(lo._, v._, 16);
  memcpy(hi._, v._ + 16, 16);
  psimd_storeu_f32x4(p, lo);
  psimd_storeu_f32x4(p + 4, hi);
#endif
}

PSIMD_INLINE void psimd_storea_f32x8(float* p, psimd_f32x8 v) {
#if defined(PSIMD_AVX)
  _mm256_store_ps(p, psimd__to_m256(v));
#else
  psimd_f32x4 lo, hi;
  memcpy(lo._, v._, 16);
  memcpy(hi._, v._ + 16, 16);
  psimd_storea_f32x4(p, lo);
  psimd_storea_f32x4(p + 4, hi);
#endif
}

PSIMD_INLINE psimd_f32x8 psimd_add_f32x8(psimd_f32x8 a, psimd_f32x8 b) {
#if defined(PSIMD_AVX)
  return psimd__from_m256(_mm256_add_ps(psimd__to_m256(a), psimd__to_m256(b)));
#else
  psimd_f32x8 r = {{0}};
  psimd_f32x4 alo, ahi, blo, bhi;
  memcpy(alo._, a._, 16);
  memcpy(ahi._, a._ + 16, 16);
  memcpy(blo._, b._, 16);
  memcpy(bhi._, b._ + 16, 16);
  psimd_f32x4 rlo = psimd_add_f32x4(alo, blo);
  psimd_f32x4 rhi = psimd_add_f32x4(ahi, bhi);
  memcpy(r._, rlo._, 16);
  memcpy(r._ + 16, rhi._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x8 psimd_sub_f32x8(psimd_f32x8 a, psimd_f32x8 b) {
#if defined(PSIMD_AVX)
  return psimd__from_m256(_mm256_sub_ps(psimd__to_m256(a), psimd__to_m256(b)));
#else
  psimd_f32x8 r = {{0}};
  psimd_f32x4 alo, ahi, blo, bhi;
  memcpy(alo._, a._, 16);
  memcpy(ahi._, a._ + 16, 16);
  memcpy(blo._, b._, 16);
  memcpy(bhi._, b._ + 16, 16);
  psimd_f32x4 rlo = psimd_sub_f32x4(alo, blo);
  psimd_f32x4 rhi = psimd_sub_f32x4(ahi, bhi);
  memcpy(r._, rlo._, 16);
  memcpy(r._ + 16, rhi._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x8 psimd_mul_f32x8(psimd_f32x8 a, psimd_f32x8 b) {
#if defined(PSIMD_AVX)
  return psimd__from_m256(_mm256_mul_ps(psimd__to_m256(a), psimd__to_m256(b)));
#else
  psimd_f32x8 r = {{0}};
  psimd_f32x4 alo, ahi, blo, bhi;
  memcpy(alo._, a._, 16);
  memcpy(ahi._, a._ + 16, 16);
  memcpy(blo._, b._, 16);
  memcpy(bhi._, b._ + 16, 16);
  psimd_f32x4 rlo = psimd_mul_f32x4(alo, blo);
  psimd_f32x4 rhi = psimd_mul_f32x4(ahi, bhi);
  memcpy(r._, rlo._, 16);
  memcpy(r._ + 16, rhi._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x8 psimd_div_f32x8(psimd_f32x8 a, psimd_f32x8 b) {
#if defined(PSIMD_AVX)
  return psimd__from_m256(_mm256_div_ps(psimd__to_m256(a), psimd__to_m256(b)));
#else
  psimd_f32x8 r = {{0}};
  psimd_f32x4 alo, ahi, blo, bhi;
  memcpy(alo._, a._, 16);
  memcpy(ahi._, a._ + 16, 16);
  memcpy(blo._, b._, 16);
  memcpy(bhi._, b._ + 16, 16);
  psimd_f32x4 rlo = psimd_div_f32x4(alo, blo);
  psimd_f32x4 rhi = psimd_div_f32x4(ahi, bhi);
  memcpy(r._, rlo._, 16);
  memcpy(r._ + 16, rhi._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x8 psimd_fma_f32x8(psimd_f32x8 a, psimd_f32x8 b,
                                         psimd_f32x8 c) {
#if defined(PSIMD_AVX) && defined(PSIMD_FMA)
  return psimd__from_m256(
      _mm256_fmadd_ps(psimd__to_m256(a), psimd__to_m256(b), psimd__to_m256(c)));
#else
  psimd_f32x8 r = {{0}};
  psimd_f32x4 alo, ahi, blo, bhi, clo, chi;
  memcpy(alo._, a._, 16);
  memcpy(ahi._, a._ + 16, 16);
  memcpy(blo._, b._, 16);
  memcpy(bhi._, b._ + 16, 16);
  memcpy(clo._, c._, 16);
  memcpy(chi._, c._ + 16, 16);
  psimd_f32x4 rlo = psimd_fma_f32x4(alo, blo, clo);
  psimd_f32x4 rhi = psimd_fma_f32x4(ahi, bhi, chi);
  memcpy(r._, rlo._, 16);
  memcpy(r._ + 16, rhi._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x8 psimd_min_f32x8(psimd_f32x8 a, psimd_f32x8 b) {
#if defined(PSIMD_AVX)
  return psimd__from_m256(_mm256_min_ps(psimd__to_m256(a), psimd__to_m256(b)));
#else
  psimd_f32x8 r = {{0}};
  psimd_f32x4 alo, ahi, blo, bhi;
  memcpy(alo._, a._, 16);
  memcpy(ahi._, a._ + 16, 16);
  memcpy(blo._, b._, 16);
  memcpy(bhi._, b._ + 16, 16);
  psimd_f32x4 rlo = psimd_min_f32x4(alo, blo);
  psimd_f32x4 rhi = psimd_min_f32x4(ahi, bhi);
  memcpy(r._, rlo._, 16);
  memcpy(r._ + 16, rhi._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x8 psimd_max_f32x8(psimd_f32x8 a, psimd_f32x8 b) {
#if defined(PSIMD_AVX)
  return psimd__from_m256(_mm256_max_ps(psimd__to_m256(a), psimd__to_m256(b)));
#else
  psimd_f32x8 r = {{0}};
  psimd_f32x4 alo, ahi, blo, bhi;
  memcpy(alo._, a._, 16);
  memcpy(ahi._, a._ + 16, 16);
  memcpy(blo._, b._, 16);
  memcpy(bhi._, b._ + 16, 16);
  psimd_f32x4 rlo = psimd_max_f32x4(alo, blo);
  psimd_f32x4 rhi = psimd_max_f32x4(ahi, bhi);
  memcpy(r._, rlo._, 16);
  memcpy(r._ + 16, rhi._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_mask32x8 psimd_cmplt_f32x8(psimd_f32x8 a, psimd_f32x8 b) {
#if defined(PSIMD_AVX)
  return psimd__from_m256i_mask(_mm256_castps_si256(
      _mm256_cmp_ps(psimd__to_m256(a), psimd__to_m256(b), _CMP_LT_OQ)));
#else
  psimd_mask32x8 r = {{0}};
  psimd_f32x4 alo, ahi, blo, bhi;
  memcpy(alo._, a._, 16);
  memcpy(ahi._, a._ + 16, 16);
  memcpy(blo._, b._, 16);
  memcpy(bhi._, b._ + 16, 16);
  psimd_mask32x4 mlo = psimd_cmplt_f32x4(alo, blo);
  psimd_mask32x4 mhi = psimd_cmplt_f32x4(ahi, bhi);
  memcpy(r._, mlo._, 16);
  memcpy(r._ + 16, mhi._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x8 psimd_select_f32x8(psimd_mask32x8 m, psimd_f32x8 a,
                                            psimd_f32x8 b) {
#if defined(PSIMD_AVX)
  __m256 mf = _mm256_castsi256_ps(psimd__mask_to_m256i(m));
  return psimd__from_m256(
      _mm256_blendv_ps(psimd__to_m256(b), psimd__to_m256(a), mf));
#else
  psimd_f32x8 r = {{0}};
  psimd_mask32x4 mlo, mhi;
  psimd_f32x4 alo, ahi, blo, bhi;
  memcpy(mlo._, m._, 16);
  memcpy(mhi._, m._ + 16, 16);
  memcpy(alo._, a._, 16);
  memcpy(ahi._, a._ + 16, 16);
  memcpy(blo._, b._, 16);
  memcpy(bhi._, b._ + 16, 16);
  psimd_f32x4 rlo = psimd_select_f32x4(mlo, alo, blo);
  psimd_f32x4 rhi = psimd_select_f32x4(mhi, ahi, bhi);
  memcpy(r._, rlo._, 16);
  memcpy(r._ + 16, rhi._, 16);
  return r;
#endif
}

PSIMD_INLINE float psimd_reduce_add_f32x8(psimd_f32x8 v) {
  psimd_f32x4 lo, hi;
  memcpy(lo._, v._, 16);
  memcpy(hi._, v._ + 16, 16);
  return psimd_reduce_add_f32x4(psimd_add_f32x4(lo, hi));
}

PSIMD_INLINE float psimd_reduce_max_f32x8(psimd_f32x8 v) {
  psimd_f32x4 lo, hi;
  memcpy(lo._, v._, 16);
  memcpy(hi._, v._ + 16, 16);
  return psimd_reduce_max_f32x4(psimd_max_f32x4(lo, hi));
}

PSIMD_INLINE float psimd_reduce_min_f32x8(psimd_f32x8 v) {
  psimd_f32x4 lo, hi;
  memcpy(lo._, v._, 16);
  memcpy(hi._, v._ + 16, 16);
  return psimd_reduce_min_f32x4(psimd_min_f32x4(lo, hi));
}

PSIMD_INLINE psimd_f32x4 psimd_lo_f32x8(psimd_f32x8 v) {
  psimd_f32x4 r = {{0}};
  memcpy(r._, v._, 16);
  return r;
}

PSIMD_INLINE psimd_f32x4 psimd_hi_f32x8(psimd_f32x8 v) {
  psimd_f32x4 r = {{0}};
  memcpy(r._, v._ + 16, 16);
  return r;
}

PSIMD_INLINE psimd_f32x8 psimd_combine_f32x8(psimd_f32x4 lo, psimd_f32x4 hi) {
  psimd_f32x8 r = {{0}};
  memcpy(r._, lo._, 16);
  memcpy(r._ + 16, hi._, 16);
  return r;
}

/* ============================================================
   i32x8 — AVX2 / emulated
   ============================================================ */

PSIMD_INLINE psimd_i32x8 psimd_set1_i32x8(int32_t x) {
#if defined(PSIMD_AVX)
  return psimd__from_m256i(_mm256_set1_epi32(x));
#else
  psimd_i32x8 r = {{0}};
  psimd_i32x4 lo = psimd_set1_i32x4(x);
  memcpy(r._, lo._, 16);
  memcpy(r._ + 16, lo._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_i32x8 psimd_add_i32x8(psimd_i32x8 a, psimd_i32x8 b) {
#if defined(PSIMD_AVX2)
  return psimd__from_m256i(
      _mm256_add_epi32(psimd__to_m256i(a), psimd__to_m256i(b)));
#else
  psimd_i32x8 r = {{0}};
  psimd_i32x4 alo, ahi, blo, bhi;
  memcpy(alo._, a._, 16);
  memcpy(ahi._, a._ + 16, 16);
  memcpy(blo._, b._, 16);
  memcpy(bhi._, b._ + 16, 16);
  psimd_i32x4 rlo = psimd_add_i32x4(alo, blo);
  psimd_i32x4 rhi = psimd_add_i32x4(ahi, bhi);
  memcpy(r._, rlo._, 16);
  memcpy(r._ + 16, rhi._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_i32x8 psimd_sub_i32x8(psimd_i32x8 a, psimd_i32x8 b) {
#if defined(PSIMD_AVX2)
  return psimd__from_m256i(
      _mm256_sub_epi32(psimd__to_m256i(a), psimd__to_m256i(b)));
#else
  psimd_i32x8 r = {{0}};
  psimd_i32x4 alo, ahi, blo, bhi;
  memcpy(alo._, a._, 16);
  memcpy(ahi._, a._ + 16, 16);
  memcpy(blo._, b._, 16);
  memcpy(bhi._, b._ + 16, 16);
  psimd_i32x4 rlo = psimd_sub_i32x4(alo, blo);
  psimd_i32x4 rhi = psimd_sub_i32x4(ahi, bhi);
  memcpy(r._, rlo._, 16);
  memcpy(r._ + 16, rhi._, 16);
  return r;
#endif
}

PSIMD_INLINE psimd_i32x4 psimd_lo_i32x8(psimd_i32x8 v) {
  psimd_i32x4 r = {{0}};
  memcpy(r._, v._, 16);
  return r;
}

PSIMD_INLINE psimd_i32x4 psimd_hi_i32x8(psimd_i32x8 v) {
  psimd_i32x4 r = {{0}};
  memcpy(r._, v._ + 16, 16);
  return r;
}

PSIMD_INLINE psimd_i32x8 psimd_combine_i32x8(psimd_i32x4 lo, psimd_i32x4 hi) {
  psimd_i32x8 r = {{0}};
  memcpy(r._, lo._, 16);
  memcpy(r._ + 16, hi._, 16);
  return r;
}

/* ============================================================
   f32x4 — floor / ceil / round (optional math ops)
   ============================================================ */

PSIMD_INLINE psimd_f32x4 psimd_floor_f32x4(psimd_f32x4 a) {
#if defined(PSIMD_SSE41) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_floor_ps(psimd__to_m128(a)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = psimd_zero_f32x4();
#if defined(__aarch64__)
  vst1q_f32((float*)r._, vrndmq_f32(vld1q_f32((const float*)a._)));
#else
  const float* pa = (const float*)a._;
  float* pr = (float*)r._;
  pr[0] = floorf(pa[0]);
  pr[1] = floorf(pa[1]);
  pr[2] = floorf(pa[2]);
  pr[3] = floorf(pa[3]);
#endif
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_f32x4_floor(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = psimd_zero_f32x4();
  const float* pa = (const float*)a._;
  float* pr = (float*)r._;
  pr[0] = floorf(pa[0]);
  pr[1] = floorf(pa[1]);
  pr[2] = floorf(pa[2]);
  pr[3] = floorf(pa[3]);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_ceil_f32x4(psimd_f32x4 a) {
#if defined(PSIMD_SSE41) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_ceil_ps(psimd__to_m128(a)));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = psimd_zero_f32x4();
#if defined(__aarch64__)
  vst1q_f32((float*)r._, vrndpq_f32(vld1q_f32((const float*)a._)));
#else
  const float* pa = (const float*)a._;
  float* pr = (float*)r._;
  pr[0] = ceilf(pa[0]);
  pr[1] = ceilf(pa[1]);
  pr[2] = ceilf(pa[2]);
  pr[3] = ceilf(pa[3]);
#endif
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_f32x4_ceil(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = psimd_zero_f32x4();
  const float* pa = (const float*)a._;
  float* pr = (float*)r._;
  pr[0] = ceilf(pa[0]);
  pr[1] = ceilf(pa[1]);
  pr[2] = ceilf(pa[2]);
  pr[3] = ceilf(pa[3]);
  return r;
#endif
}

PSIMD_INLINE psimd_f32x4 psimd_round_f32x4(psimd_f32x4 a) {
#if defined(PSIMD_SSE41) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_round_ps(
      psimd__to_m128(a), _MM_FROUND_TO_NEAREST_INT | _MM_FROUND_NO_EXC));
#elif defined(PSIMD_NEON)
  psimd_f32x4 r = psimd_zero_f32x4();
#if defined(__aarch64__)
  vst1q_f32((float*)r._, vrndnq_f32(vld1q_f32((const float*)a._)));
#else
  const float* pa = (const float*)a._;
  float* pr = (float*)r._;
  pr[0] = psimd__round_nearest_even_f32(pa[0]);
  pr[1] = psimd__round_nearest_even_f32(pa[1]);
  pr[2] = psimd__round_nearest_even_f32(pa[2]);
  pr[3] = psimd__round_nearest_even_f32(pa[3]);
#endif
  return r;
#elif defined(PSIMD_WASM)
  psimd_f32x4 r = {{0}};
  v128_t va;
  memcpy(&va, a._, 16);
  v128_t vr = wasm_f32x4_nearest(va);
  memcpy(r._, &vr, 16);
  return r;
#else
  psimd_f32x4 r = psimd_zero_f32x4();
  const float* pa = (const float*)a._;
  float* pr = (float*)r._;
  pr[0] = psimd__round_nearest_even_f32(pa[0]);
  pr[1] = psimd__round_nearest_even_f32(pa[1]);
  pr[2] = psimd__round_nearest_even_f32(pa[2]);
  pr[3] = psimd__round_nearest_even_f32(pa[3]);
  return r;
#endif
}

/* ============================================================
   f32x4 — dot product / horizontal add
   ============================================================ */

PSIMD_INLINE float psimd_dot4_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
  return psimd_reduce_add_f32x4(psimd_mul_f32x4(a, b));
}

PSIMD_INLINE psimd_f32x4 psimd_hadd_f32x4(psimd_f32x4 a, psimd_f32x4 b) {
#if defined(PSIMD_SSE41) || defined(PSIMD_AVX)
  return psimd__from_m128(_mm_hadd_ps(psimd__to_m128(a), psimd__to_m128(b)));
#else
  const float *pa = (const float*)a._, *pb = (const float*)b._;
  return psimd_set_f32x4(pa[0] + pa[1], pa[2] + pa[3], pb[0] + pb[1],
                         pb[2] + pb[3]);
#endif
}

/* ============================================================
   u32x4 — basic unsigned integer support
   ============================================================ */

PSIMD_INLINE psimd_u32x4 psimd_set1_u32x4(uint32_t x) {
  psimd_u32x4 r = {{0}};
  memset(r._, 0, 16);
  uint32_t* p = (uint32_t*)r._;
  p[0] = p[1] = p[2] = p[3] = x;
  return r;
}

PSIMD_INLINE psimd_u32x4 psimd_set_u32x4(uint32_t x0, uint32_t x1, uint32_t x2,
                                         uint32_t x3) {
  psimd_u32x4 r = {{0}};
  memset(r._, 0, 16);
  uint32_t* p = (uint32_t*)r._;
  p[0] = x0;
  p[1] = x1;
  p[2] = x2;
  p[3] = x3;
  return r;
}

PSIMD_INLINE psimd_u32x4 psimd_loadu_u32x4(const uint32_t* p) {
  psimd_u32x4 r = {{0}};
  memcpy(r._, p, 16);
  return r;
}

PSIMD_INLINE void psimd_storeu_u32x4(uint32_t* p, psimd_u32x4 v) {
  memcpy(p, v._, 16);
}

PSIMD_INLINE psimd_u32x4 psimd_add_u32x4(psimd_u32x4 a, psimd_u32x4 b) {
  psimd_i32x4 ia, ib;
  memcpy(ia._, a._, 16);
  memcpy(ib._, b._, 16);
  psimd_i32x4 ir = psimd_add_i32x4(ia, ib);
  psimd_u32x4 r = {{0}};
  memcpy(r._, ir._, 16);
  return r;
}

PSIMD_INLINE psimd_u32x4 psimd_sub_u32x4(psimd_u32x4 a, psimd_u32x4 b) {
  psimd_i32x4 ia, ib;
  memcpy(ia._, a._, 16);
  memcpy(ib._, b._, 16);
  psimd_i32x4 ir = psimd_sub_i32x4(ia, ib);
  psimd_u32x4 r = {{0}};
  memcpy(r._, ir._, 16);
  return r;
}

PSIMD_INLINE psimd_u32x4 psimd_and_u32x4(psimd_u32x4 a, psimd_u32x4 b) {
  psimd_i32x4 ia, ib;
  memcpy(ia._, a._, 16);
  memcpy(ib._, b._, 16);
  psimd_i32x4 ir = psimd_and_i32x4(ia, ib);
  psimd_u32x4 r = {{0}};
  memcpy(r._, ir._, 16);
  return r;
}

PSIMD_INLINE psimd_u32x4 psimd_or_u32x4(psimd_u32x4 a, psimd_u32x4 b) {
  psimd_i32x4 ia, ib;
  memcpy(ia._, a._, 16);
  memcpy(ib._, b._, 16);
  psimd_i32x4 ir = psimd_or_i32x4(ia, ib);
  psimd_u32x4 r = {{0}};
  memcpy(r._, ir._, 16);
  return r;
}

PSIMD_INLINE psimd_u32x4 psimd_xor_u32x4(psimd_u32x4 a, psimd_u32x4 b) {
  psimd_i32x4 ia, ib;
  memcpy(ia._, a._, 16);
  memcpy(ib._, b._, 16);
  psimd_i32x4 ir = psimd_xor_i32x4(ia, ib);
  psimd_u32x4 r = {{0}};
  memcpy(r._, ir._, 16);
  return r;
}

PSIMD_INLINE psimd_u32x4 psimd_shl_u32x4(psimd_u32x4 a, int n) {
  psimd_i32x4 ia;
  memcpy(ia._, a._, 16);
  psimd_i32x4 ir = psimd_shl_i32x4(ia, n);
  psimd_u32x4 r = {{0}};
  memcpy(r._, ir._, 16);
  return r;
}

PSIMD_INLINE psimd_u32x4 psimd_shr_u32x4(psimd_u32x4 a, int n) {
  psimd_i32x4 ia;
  memcpy(ia._, a._, 16);
  psimd_i32x4 ir = psimd_shru_i32x4(ia, n);
  psimd_u32x4 r = {{0}};
  memcpy(r._, ir._, 16);
  return r;
}

PSIMD_INLINE psimd_mask32x4 psimd_cmpeq_u32x4(psimd_u32x4 a, psimd_u32x4 b) {
  psimd_i32x4 ia, ib;
  memcpy(ia._, a._, 16);
  memcpy(ib._, b._, 16);
  return psimd_cmpeq_i32x4(ia, ib);
}

/* ============================================================
   prefetch hints
   ============================================================ */

PSIMD_INLINE void psimd_prefetch_r([[maybe_unused]] const void* p) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  _mm_prefetch((const char*)p, _MM_HINT_T0);
#elif defined(PSIMD_NEON)
  __builtin_prefetch(p, 0, 3);
#endif
}

PSIMD_INLINE void psimd_prefetch_w([[maybe_unused]] const void* p) {
#if defined(PSIMD_SSE2) || defined(PSIMD_AVX)
  _mm_prefetch((const char*)p, _MM_HINT_T0);
#elif defined(PSIMD_NEON)
  __builtin_prefetch(p, 1, 3);
#endif
}

/* ============================================================
   backend query
   ============================================================ */

typedef enum {
  psimd_backend_scalar = 0,
  psimd_backend_sse2 = 1,
  psimd_backend_sse41 = 2,
  psimd_backend_avx = 3,
  psimd_backend_neon = 4,
  psimd_backend_wasm = 5,
} psimd_backend;

[[nodiscard]] PSIMD_INLINE psimd_backend psimd_get_backend() {
#if defined(PSIMD_AVX)
  return psimd_backend_avx;
#elif defined(PSIMD_SSE41)
  return psimd_backend_sse41;
#elif defined(PSIMD_SSE2)
  return psimd_backend_sse2;
#elif defined(PSIMD_NEON)
  return psimd_backend_neon;
#elif defined(PSIMD_WASM)
  return psimd_backend_wasm;
#else
  return psimd_backend_scalar;
#endif
}

[[nodiscard]] PSIMD_INLINE int psimd_f32x4_width() {
  return 4;
}

[[nodiscard]] PSIMD_INLINE int psimd_f32x8_width() {
  return 8;
}

[[nodiscard]] PSIMD_INLINE int psimd_i32x4_width() {
  return 4;
}

#endif /* PSIMD_H */
