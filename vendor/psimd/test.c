#include "psimd.h"

#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_passed;
static int g_failed;

static PSIMD_CONSTEXPR float f32_epsilon = 1e-5f;
static PSIMD_CONSTEXPR uint32_t mask32_true_bits = UINT32_MAX;

static void check(bool cond, const char* name) {
  if (cond) {
    printf("  PASS  %s\n", name);
    g_passed++;
    return;
  }

  printf("  FAIL  %s\n", name);
  g_failed++;
}

static void check_f32(float a, float b, const char* name) {
  check(fabsf(a - b) < f32_epsilon, name);
}

[[nodiscard]] static const char* backend_name(psimd_backend b) {
  switch (b) {
  case psimd_backend_scalar:
    return "scalar";
  case psimd_backend_sse2:
    return "SSE2";
  case psimd_backend_sse41:
    return "SSE4.1";
  case psimd_backend_avx:
    return "AVX/AVX2";
  case psimd_backend_neon:
    return "NEON";
  case psimd_backend_wasm:
    return "WASM SIMD";
  default:
    return "unknown";
  }
}

/* ---- helpers ---- */

static bool f32x4_eq(psimd_f32x4 v, float x0, float x1, float x2, float x3) {
  float out[4];
  psimd_storeu_f32x4(out, v);
  return fabsf(out[0] - x0) < f32_epsilon && fabsf(out[1] - x1) < f32_epsilon &&
         fabsf(out[2] - x2) < f32_epsilon && fabsf(out[3] - x3) < f32_epsilon;
}

static bool f32x8_eq(psimd_f32x8 v, float x0, float x1, float x2, float x3,
                     float x4, float x5, float x6, float x7) {
  float out[8];
  psimd_storeu_f32x8(out, v);
  return fabsf(out[0] - x0) < f32_epsilon && fabsf(out[1] - x1) < f32_epsilon &&
         fabsf(out[2] - x2) < f32_epsilon && fabsf(out[3] - x3) < f32_epsilon &&
         fabsf(out[4] - x4) < f32_epsilon && fabsf(out[5] - x5) < f32_epsilon &&
         fabsf(out[6] - x6) < f32_epsilon && fabsf(out[7] - x7) < f32_epsilon;
}

static bool i32x4_eq(psimd_i32x4 v, int32_t x0, int32_t x1, int32_t x2,
                     int32_t x3) {
  int32_t out[4];
  psimd_storeu_i32x4(out, v);
  return out[0] == x0 && out[1] == x1 && out[2] == x2 && out[3] == x3;
}

static bool mask32x4_lanes(psimd_mask32x4 m, bool l0, bool l1, bool l2,
                           bool l3) {
  uint32_t out[4];
  memcpy(out, m._, 16);
  auto b0 = out[0] == mask32_true_bits;
  auto b1 = out[1] == mask32_true_bits;
  auto b2 = out[2] == mask32_true_bits;
  auto b3 = out[3] == mask32_true_bits;
  return b0 == l0 && b1 == l1 && b2 == l2 && b3 == l3;
}

/* ================================================================
   f32x4 construction
   ================================================================ */

static void test_f32x4_construction() {
  printf("\nf32x4 construction\n");

  psimd_f32x4 a = psimd_set1_f32x4(3.14f);
  check(f32x4_eq(a, 3.14f, 3.14f, 3.14f, 3.14f), "set1");

  psimd_f32x4 b = psimd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  check(f32x4_eq(b, 1.0f, 2.0f, 3.0f, 4.0f), "set");

  psimd_f32x4 z = psimd_zero_f32x4();
  check(f32x4_eq(z, 0.0f, 0.0f, 0.0f, 0.0f), "zero");

  float src[8] = {10.0f, 20.0f, 30.0f, 40.0f, 99.0f, 99.0f, 99.0f, 99.0f};
  psimd_f32x4 lu = psimd_loadu_f32x4(src);
  check(f32x4_eq(lu, 10.0f, 20.0f, 30.0f, 40.0f), "loadu");

  alignas(16) float asrc[4] = {5.0f, 6.0f, 7.0f, 8.0f};
  psimd_f32x4 la = psimd_loada_f32x4(asrc);
  check(f32x4_eq(la, 5.0f, 6.0f, 7.0f, 8.0f), "loada");

  float dst[4] = {0};
  psimd_storeu_f32x4(dst, b);
  check(dst[0] == 1.0f && dst[1] == 2.0f && dst[2] == 3.0f && dst[3] == 4.0f,
        "storeu");

  alignas(16) float adst[4] = {0};
  psimd_storea_f32x4(adst, b);
  check(adst[0] == 1.0f && adst[1] == 2.0f && adst[2] == 3.0f &&
            adst[3] == 4.0f,
        "storea");

  check(psimd_get_lane_f32x4(b, 2) == 3.0f, "get_lane");
  psimd_f32x4 bl = psimd_set_lane_f32x4(b, 1, 99.0f);
  check(f32x4_eq(bl, 1.0f, 99.0f, 3.0f, 4.0f) &&
            psimd_get_lane_f32x4(b, 1) == 2.0f,
        "set_lane (returns modified copy)");
}

/* ================================================================
   f32x4 arithmetic
   ================================================================ */

static void test_f32x4_arithmetic() {
  printf("\nf32x4 arithmetic\n");

  psimd_f32x4 a = psimd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  psimd_f32x4 b = psimd_set_f32x4(4.0f, 3.0f, 2.0f, 1.0f);

  check(f32x4_eq(psimd_add_f32x4(a, b), 5.0f, 5.0f, 5.0f, 5.0f), "add");
  check(f32x4_eq(psimd_sub_f32x4(a, b), -3.0f, -1.0f, 1.0f, 3.0f), "sub");
  check(f32x4_eq(psimd_mul_f32x4(a, b), 4.0f, 6.0f, 6.0f, 4.0f), "mul");
  check(f32x4_eq(psimd_div_f32x4(b, a), 4.0f, 1.5f, 2.0f / 3.0f, 0.25f), "div");
  check(f32x4_eq(psimd_neg_f32x4(a), -1.0f, -2.0f, -3.0f, -4.0f), "neg");
  check(f32x4_eq(psimd_abs_f32x4(psimd_neg_f32x4(a)), 1.0f, 2.0f, 3.0f, 4.0f),
        "abs");

  psimd_f32x4 sq = psimd_set_f32x4(4.0f, 9.0f, 16.0f, 25.0f);
  check(f32x4_eq(psimd_sqrt_f32x4(sq), 2.0f, 3.0f, 4.0f, 5.0f), "sqrt");

  psimd_f32x4 rsq = psimd_rsqrt_f32x4(sq);
  float rout[4];
  psimd_storeu_f32x4(rout, rsq);
  check(fabsf(rout[0] - 0.5f) < 1e-3f && fabsf(rout[1] - 1.0f / 3.0f) < 1e-3f,
        "rsqrt");
}

/* ================================================================
   f32x4 FMA
   ================================================================ */

static void test_f32x4_fma() {
  printf("\nf32x4 fma\n");

  psimd_f32x4 a = psimd_set1_f32x4(2.0f);
  psimd_f32x4 b = psimd_set1_f32x4(3.0f);
  psimd_f32x4 c = psimd_set1_f32x4(1.0f);

  check(f32x4_eq(psimd_fma_f32x4(a, b, c), 7.0f, 7.0f, 7.0f, 7.0f),
        "fma  a*b+c");
  check(f32x4_eq(psimd_fnma_f32x4(a, b, c), -5.0f, -5.0f, -5.0f, -5.0f),
        "fnma -a*b+c");
}

/* ================================================================
   f32x4 min / max
   ================================================================ */

static void test_f32x4_minmax() {
  printf("\nf32x4 min/max\n");

  psimd_f32x4 a = psimd_set_f32x4(1.0f, 5.0f, 2.0f, 8.0f);
  psimd_f32x4 b = psimd_set_f32x4(4.0f, 2.0f, 7.0f, 3.0f);

  check(f32x4_eq(psimd_min_f32x4(a, b), 1.0f, 2.0f, 2.0f, 3.0f), "min");
  check(f32x4_eq(psimd_max_f32x4(a, b), 4.0f, 5.0f, 7.0f, 8.0f), "max");
}

/* ================================================================
   f32x4 comparisons
   ================================================================ */

static void test_f32x4_comparisons() {
  printf("\nf32x4 comparisons\n");

  psimd_f32x4 a = psimd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  psimd_f32x4 b = psimd_set_f32x4(2.0f, 2.0f, 2.0f, 2.0f);

  check(mask32x4_lanes(psimd_cmpeq_f32x4(a, b), false, true, false, false),
        "cmpeq");
  check(mask32x4_lanes(psimd_cmpne_f32x4(a, b), true, false, true, true),
        "cmpne");
  check(mask32x4_lanes(psimd_cmplt_f32x4(a, b), true, false, false, false),
        "cmplt");
  check(mask32x4_lanes(psimd_cmple_f32x4(a, b), true, true, false, false),
        "cmple");
  check(mask32x4_lanes(psimd_cmpgt_f32x4(a, b), false, false, true, true),
        "cmpgt");
  check(mask32x4_lanes(psimd_cmpge_f32x4(a, b), false, true, true, true),
        "cmpge");
}

/* ================================================================
   f32x4 select
   ================================================================ */

static void test_f32x4_select() {
  printf("\nf32x4 select\n");

  psimd_f32x4 a = psimd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  psimd_f32x4 b = psimd_set_f32x4(10.0f, 20.0f, 30.0f, 40.0f);
  psimd_f32x4 limit = psimd_set1_f32x4(2.5f);

  psimd_mask32x4 m = psimd_cmplt_f32x4(a, limit);
  psimd_f32x4 r = psimd_select_f32x4(m, a, b);
  check(f32x4_eq(r, 1.0f, 2.0f, 30.0f, 40.0f), "select via cmplt");

  psimd_mask32x4 mt = psimd_true_mask32x4();
  psimd_mask32x4 mf = psimd_false_mask32x4();
  check(f32x4_eq(psimd_select_f32x4(mt, a, b), 1.0f, 2.0f, 3.0f, 4.0f),
        "select true");
  check(f32x4_eq(psimd_select_f32x4(mf, a, b), 10.0f, 20.0f, 30.0f, 40.0f),
        "select false");
}

/* ================================================================
   f32x4 reductions
   ================================================================ */

static void test_f32x4_reductions() {
  printf("\nf32x4 reductions\n");

  psimd_f32x4 v = psimd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);

  check_f32(psimd_reduce_add_f32x4(v), 10.0f, "reduce_add");
  check_f32(psimd_reduce_max_f32x4(v), 4.0f, "reduce_max");
  check_f32(psimd_reduce_min_f32x4(v), 1.0f, "reduce_min");
  check_f32(psimd_reduce_mul_f32x4(v), 24.0f, "reduce_mul");
  check_f32(psimd_dot4_f32x4(v, v), 30.0f, "dot4");

  psimd_f32x4 a = psimd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  psimd_f32x4 b = psimd_set_f32x4(5.0f, 6.0f, 7.0f, 8.0f);
  check(f32x4_eq(psimd_hadd_f32x4(a, b), 3.0f, 7.0f, 11.0f, 15.0f), "hadd");
}

/* ================================================================
   f32x4 math ops
   ================================================================ */

static void test_f32x4_math() {
  printf("\nf32x4 math\n");

  psimd_f32x4 v = psimd_set_f32x4(-1.7f, 1.2f, 2.9f, -0.1f);

  check(f32x4_eq(psimd_floor_f32x4(v), -2.0f, 1.0f, 2.0f, -1.0f), "floor");
  check(f32x4_eq(psimd_ceil_f32x4(v), -1.0f, 2.0f, 3.0f, 0.0f), "ceil");

  psimd_f32x4 rv = psimd_set_f32x4(0.4f, 1.4f, 2.6f, -0.6f);
  float rout[4];
  psimd_storeu_f32x4(rout, psimd_round_f32x4(rv));
  check(rout[0] == 0.0f && rout[1] == 1.0f && rout[2] == 3.0f &&
            rout[3] == -1.0f,
        "round (unambiguous cases)");

  psimd_f32x4 ties = psimd_set_f32x4(0.5f, 1.5f, 2.5f, -1.5f);
  psimd_storeu_f32x4(rout, psimd_round_f32x4(ties));
  check(rout[0] == 0.0f && rout[1] == 2.0f && rout[2] == 2.0f &&
            rout[3] == -2.0f,
        "round ties to even");
}

/* ================================================================
   f32x4 shuffle / zip
   ================================================================ */

static void test_f32x4_shuffle() {
  printf("\nf32x4 shuffle / zip\n");

  psimd_f32x4 a = psimd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  psimd_f32x4 b = psimd_set_f32x4(5.0f, 6.0f, 7.0f, 8.0f);

  check(f32x4_eq(psimd_zip_lo_f32x4(a, b), 1.0f, 5.0f, 2.0f, 6.0f), "zip_lo");
  check(f32x4_eq(psimd_zip_hi_f32x4(a, b), 3.0f, 7.0f, 4.0f, 8.0f), "zip_hi");
}

/* ================================================================
   mask32x4 logical operations
   ================================================================ */

static void test_mask32x4_ops() {
  printf("\nmask32x4 logical\n");

  psimd_f32x4 a = psimd_set_f32x4(1.0f, 2.0f, 3.0f, 4.0f);
  psimd_f32x4 t = psimd_set1_f32x4(2.5f);
  psimd_mask32x4 lo = psimd_cmplt_f32x4(a, t);
  psimd_mask32x4 hi = psimd_cmpgt_f32x4(a, t);
  psimd_mask32x4 all = psimd_true_mask32x4();
  psimd_mask32x4 non = psimd_false_mask32x4();

  check(psimd_any_mask32x4(lo), "any (true)");
  check(!psimd_any_mask32x4(non), "any (false)");
  check(psimd_all_mask32x4(all), "all (true)");
  check(!psimd_all_mask32x4(lo), "all (false)");

  check(mask32x4_lanes(psimd_and_mask32x4(lo, hi), false, false, false, false),
        "and");
  check(mask32x4_lanes(psimd_or_mask32x4(lo, hi), true, true, true, true),
        "or");
  check(mask32x4_lanes(psimd_xor_mask32x4(lo, lo), false, false, false, false),
        "xor self");
  check(mask32x4_lanes(psimd_not_mask32x4(lo), false, false, true, true),
        "not");
}

/* ================================================================
   i32x4 construction
   ================================================================ */

static void test_i32x4_construction() {
  printf("\ni32x4 construction\n");

  check(i32x4_eq(psimd_set1_i32x4(7), 7, 7, 7, 7), "set1");
  check(i32x4_eq(psimd_set_i32x4(-1, 0, 1, 2), -1, 0, 1, 2), "set");
  check(i32x4_eq(psimd_zero_i32x4(), 0, 0, 0, 0), "zero");

  int32_t src[4] = {10, 20, 30, 40};
  check(i32x4_eq(psimd_loadu_i32x4(src), 10, 20, 30, 40), "loadu");

  int32_t dst[4] = {0};
  psimd_storeu_i32x4(dst, psimd_set_i32x4(1, 2, 3, 4));
  check(dst[0] == 1 && dst[1] == 2 && dst[2] == 3 && dst[3] == 4, "storeu");
}

/* ================================================================
   i32x4 arithmetic
   ================================================================ */

static void test_i32x4_arithmetic() {
  printf("\ni32x4 arithmetic\n");

  psimd_i32x4 a = psimd_set_i32x4(10, 20, 30, 40);
  psimd_i32x4 b = psimd_set_i32x4(1, 2, 3, 4);

  check(i32x4_eq(psimd_add_i32x4(a, b), 11, 22, 33, 44), "add");
  check(i32x4_eq(psimd_sub_i32x4(a, b), 9, 18, 27, 36), "sub");
  check(i32x4_eq(psimd_mul_i32x4(a, b), 10, 40, 90, 160), "mul");
  check(i32x4_eq(psimd_neg_i32x4(b), -1, -2, -3, -4), "neg");
  check(i32x4_eq(psimd_abs_i32x4(psimd_neg_i32x4(b)), 1, 2, 3, 4), "abs");
  check(i32x4_eq(psimd_min_i32x4(a, psimd_set1_i32x4(25)), 10, 20, 25, 25),
        "min");
  check(i32x4_eq(psimd_max_i32x4(a, psimd_set1_i32x4(25)), 25, 25, 30, 40),
        "max");
  check(i32x4_eq(
            psimd_add_i32x4(psimd_set1_i32x4(INT32_MAX), psimd_set1_i32x4(1)),
            INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN),
        "add wraps");
  check(i32x4_eq(
            psimd_sub_i32x4(psimd_set1_i32x4(INT32_MIN), psimd_set1_i32x4(1)),
            INT32_MAX, INT32_MAX, INT32_MAX, INT32_MAX),
        "sub wraps");
  check(i32x4_eq(
            psimd_mul_i32x4(psimd_set1_i32x4(INT32_MIN), psimd_set1_i32x4(-1)),
            INT32_MIN, INT32_MIN, INT32_MIN, INT32_MIN),
        "mul wraps");
  check(i32x4_eq(psimd_neg_i32x4(psimd_set1_i32x4(INT32_MIN)), INT32_MIN,
                 INT32_MIN, INT32_MIN, INT32_MIN),
        "neg wraps");
  check(i32x4_eq(psimd_abs_i32x4(psimd_set1_i32x4(INT32_MIN)), INT32_MIN,
                 INT32_MIN, INT32_MIN, INT32_MIN),
        "abs INT32_MIN wraps");
}

/* ================================================================
   i32x4 bitwise and shifts
   ================================================================ */

static void test_i32x4_bitwise() {
  printf("\ni32x4 bitwise / shift\n");

  psimd_i32x4 a = psimd_set1_i32x4(0x0f0f0f0f);
  psimd_i32x4 b = psimd_set1_i32x4(0x00ff00ff);

  check(i32x4_eq(psimd_and_i32x4(a, b), 0x000f000f, 0x000f000f, 0x000f000f,
                 0x000f000f),
        "and");
  check(i32x4_eq(psimd_or_i32x4(a, b), 0x0fff0fff, 0x0fff0fff, 0x0fff0fff,
                 0x0fff0fff),
        "or");
  check(i32x4_eq(psimd_xor_i32x4(a, b), 0x0ff00ff0, 0x0ff00ff0, 0x0ff00ff0,
                 0x0ff00ff0),
        "xor");

  psimd_i32x4 v = psimd_set1_i32x4(8);
  check(i32x4_eq(psimd_shl_i32x4(v, 3), 64, 64, 64, 64), "shl");
  check(i32x4_eq(psimd_shr_i32x4(v, 1), 4, 4, 4, 4), "shr (arithmetic)");
  check(i32x4_eq(psimd_shru_i32x4(v, 1), 4, 4, 4, 4), "shru (logical)");

  psimd_i32x4 neg = psimd_set1_i32x4(-8);
  int32_t sout[4], uout[4];
  psimd_storeu_i32x4(sout, psimd_shr_i32x4(neg, 1));
  psimd_storeu_i32x4(uout, psimd_shru_i32x4(neg, 1));
  check(sout[0] == -4, "shr negative (sign extend)");
  check(uout[0] > 0, "shru negative (zero fill)");
}

/* ================================================================
   i32x4 comparisons and select
   ================================================================ */

static void test_i32x4_comparisons() {
  printf("\ni32x4 comparisons / select\n");

  psimd_i32x4 a = psimd_set_i32x4(1, 2, 3, 4);
  psimd_i32x4 b = psimd_set1_i32x4(2);

  check(mask32x4_lanes(psimd_cmpeq_i32x4(a, b), false, true, false, false),
        "cmpeq");
  check(mask32x4_lanes(psimd_cmplt_i32x4(a, b), true, false, false, false),
        "cmplt");
  check(mask32x4_lanes(psimd_cmpgt_i32x4(a, b), false, false, true, true),
        "cmpgt");

  psimd_mask32x4 m = psimd_cmpgt_i32x4(a, b);
  psimd_i32x4 r = psimd_select_i32x4(m, a, psimd_set1_i32x4(0));
  check(i32x4_eq(r, 0, 0, 3, 4), "select");
}

/* ================================================================
   i32x4 reductions
   ================================================================ */

static void test_i32x4_reductions() {
  printf("\ni32x4 reductions\n");

  psimd_i32x4 v = psimd_set_i32x4(1, 2, 3, 4);
  check(psimd_reduce_add_i32x4(v) == 10, "reduce_add");
  check(psimd_reduce_max_i32x4(v) == 4, "reduce_max");
  check(psimd_reduce_min_i32x4(v) == 1, "reduce_min");

  psimd_i32x4 wrap = psimd_set_i32x4(INT32_MAX, 1, 1, 0);
  check(psimd_reduce_add_i32x4(wrap) == INT32_MIN + 1, "reduce_add wraps");
}

/* ================================================================
   type conversions
   ================================================================ */

static void test_conversions() {
  printf("\ntype conversions\n");

  psimd_f32x4 f = psimd_set_f32x4(1.9f, 2.1f, -3.7f, 4.0f);
  psimd_i32x4 i = psimd_cvt_f32x4_i32x4(f);
  check(i32x4_eq(i, 1, 2, -3, 4), "cvt f32->i32 (truncate)");

  psimd_i32x4 src = psimd_set_i32x4(-2, 0, 1, 100);
  psimd_f32x4 r = psimd_cvt_i32x4_f32x4(src);
  check(f32x4_eq(r, -2.0f, 0.0f, 1.0f, 100.0f), "cvt i32->f32");

  psimd_i32x4 bits = psimd_set1_i32x4(0x3f800000);
  psimd_f32x4 rf = psimd_reinterpret_i32x4_f32x4(bits);
  float fout[4];
  psimd_storeu_f32x4(fout, rf);
  check_f32(fout[0], 1.0f, "reinterpret i32->f32 (IEEE 1.0)");

  psimd_i32x4 rb = psimd_reinterpret_f32x4_i32x4(rf);
  int32_t iout[4];
  psimd_storeu_i32x4(iout, rb);
  check(iout[0] == 0x3f800000, "reinterpret f32->i32 (round-trip)");

  psimd_mask32x4 mk = psimd_reinterpret_i32x4_mask32x4(psimd_set1_i32x4(-1));
  check(psimd_all_mask32x4(mk), "reinterpret i32->mask (all-ones)");
}

/* ================================================================
   f32x8
   ================================================================ */

static void test_f32x8() {
  printf("\nf32x8\n");

  float a[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  float b[8] = {8, 7, 6, 5, 4, 3, 2, 1};
  float out[8];

  psimd_f32x8 va = psimd_loadu_f32x8(a);
  psimd_f32x8 vb = psimd_loadu_f32x8(b);

  psimd_storeu_f32x8(out, psimd_add_f32x8(va, vb));
  bool add_ok = true;
  for (int i = 0; i < 8; i++)
    if (fabsf(out[i] - 9.0f) > f32_epsilon)
      add_ok = false;
  check(add_ok, "add");

  psimd_storeu_f32x8(out, psimd_mul_f32x8(va, vb));
  check(fabsf(out[0] - 8.0f) < f32_epsilon &&
            fabsf(out[7] - 8.0f) < f32_epsilon,
        "mul");

  check(f32x8_eq(psimd_min_f32x8(va, vb), 1, 2, 3, 4, 4, 3, 2, 1), "min");
  check(f32x8_eq(psimd_max_f32x8(va, vb), 8, 7, 6, 5, 5, 6, 7, 8), "max");

  psimd_f32x8 vc = psimd_add_f32x8(va, vb);
  check_f32(psimd_reduce_add_f32x8(vc), 72.0f, "reduce_add");
  check_f32(psimd_reduce_max_f32x8(vc), 9.0f, "reduce_max");
  check_f32(psimd_reduce_min_f32x8(vc), 9.0f, "reduce_min");

  psimd_f32x4 lo = psimd_lo_f32x8(va);
  psimd_f32x4 hi = psimd_hi_f32x8(va);
  check(f32x4_eq(lo, 1, 2, 3, 4), "lo_f32x8");
  check(f32x4_eq(hi, 5, 6, 7, 8), "hi_f32x8");

  psimd_f32x8 rc = psimd_combine_f32x8(hi, lo);
  check(f32x8_eq(rc, 5, 6, 7, 8, 1, 2, 3, 4), "combine_f32x8");
}

/* ================================================================
   i32x8
   ================================================================ */

static void test_i32x8() {
  printf("\ni32x8\n");

  psimd_i32x8 a = psimd_set1_i32x8(3);
  psimd_i32x8 b = psimd_set1_i32x8(7);
  psimd_i32x8 r = psimd_add_i32x8(a, b);
  psimd_i32x4 lo = psimd_lo_i32x8(r);
  psimd_i32x4 hi = psimd_hi_i32x8(r);
  check(i32x4_eq(lo, 10, 10, 10, 10), "add lo");
  check(i32x4_eq(hi, 10, 10, 10, 10), "add hi");

  psimd_i32x8 s = psimd_sub_i32x8(b, a);
  psimd_i32x4 slo = psimd_lo_i32x8(s);
  check(i32x4_eq(slo, 4, 4, 4, 4), "sub");

  psimd_i32x8 comb = psimd_combine_i32x8(psimd_set_i32x4(1, 2, 3, 4),
                                         psimd_set_i32x4(5, 6, 7, 8));
  check(i32x4_eq(psimd_lo_i32x8(comb), 1, 2, 3, 4), "combine lo");
  check(i32x4_eq(psimd_hi_i32x8(comb), 5, 6, 7, 8), "combine hi");
}

/* ================================================================
   u32x4
   ================================================================ */

static void test_u32x4() {
  printf("\nu32x4\n");

  psimd_u32x4 a = psimd_set1_u32x4(0xdeadbeef);
  psimd_u32x4 b = psimd_set1_u32x4(0x00ff00ff);
  uint32_t out[4];

  psimd_storeu_u32x4(out, psimd_and_u32x4(a, b));
  check(out[0] == 0x00ad00ef, "and");

  psimd_storeu_u32x4(out, psimd_or_u32x4(a, b));
  check(out[0] == 0xdeffbeffu, "or");

  psimd_u32x4 v = psimd_set1_u32x4(1u);
  psimd_storeu_u32x4(out, psimd_shl_u32x4(v, 31));
  check(out[0] == 0x80000000u, "shl to MSB");

  psimd_storeu_u32x4(out, psimd_shr_u32x4(psimd_set1_u32x4(0x80000000u), 1));
  check(out[0] == 0x40000000u, "shr logical (no sign ext)");

  psimd_u32x4 eq1 = psimd_set1_u32x4(42u);
  psimd_u32x4 eq2 = psimd_set_u32x4(42u, 0u, 42u, 1u);
  check(mask32x4_lanes(psimd_cmpeq_u32x4(eq1, eq2), true, false, true, false),
        "cmpeq");
}

/* ================================================================
   kernel: array elementwise ops
   ================================================================ */

static void test_kernel_array_ops() {
  printf("\nkernel: array ops\n");

  PSIMD_CONSTEXPR size_t n = 16;
  alignas(16) float a[16], b[16], out[16];
  for (size_t i = 0; i < n; i++) {
    a[i] = (float)(i + 1);
    b[i] = (float)(n - i);
  }

  for (size_t i = 0; i < n; i += 4) {
    psimd_f32x4 va = psimd_loada_f32x4(a + i);
    psimd_f32x4 vb = psimd_loada_f32x4(b + i);
    psimd_storea_f32x4(out + i, psimd_add_f32x4(va, vb));
  }
  bool ok = true;
  for (size_t i = 0; i < n; i++)
    if (fabsf(out[i] - (float)(n + 1)) > f32_epsilon)
      ok = false;
  check(ok, "elementwise add (aligned stride-4)");

  for (size_t i = 0; i < n; i += 4) {
    psimd_f32x4 va = psimd_loada_f32x4(a + i);
    psimd_f32x4 vb = psimd_loada_f32x4(b + i);
    psimd_f32x4 res = psimd_fma_f32x4(va, vb, psimd_set1_f32x4(1.0f));
    psimd_storea_f32x4(out + i, res);
  }
  float expected0 = a[0] * b[0] + 1.0f;
  check_f32(out[0], expected0, "FMA kernel lane 0");
}

/* ================================================================
   kernel: relu
   ================================================================ */

static void test_kernel_relu() {
  printf("\nkernel: relu\n");

  PSIMD_CONSTEXPR size_t n = 8;
  float in[8] = {-3.0f, 1.0f, -0.5f, 2.0f, 0.0f, -1.0f, 4.0f, -2.0f};
  float out[8] = {0};
  psimd_f32x4 zero = psimd_zero_f32x4();

  for (size_t i = 0; i < n; i += 4) {
    psimd_f32x4 v = psimd_loadu_f32x4(in + i);
    psimd_storeu_f32x4(out + i, psimd_max_f32x4(v, zero));
  }

  float expected[8] = {0, 1.0f, 0, 2.0f, 0, 0, 4.0f, 0};
  bool ok = true;
  for (size_t i = 0; i < n; i++)
    if (fabsf(out[i] - expected[i]) > f32_epsilon)
      ok = false;
  check(ok, "relu (max with zero)");
}

/* ================================================================
   kernel: clamp
   ================================================================ */

static void test_kernel_clamp() {
  printf("\nkernel: clamp\n");

  PSIMD_CONSTEXPR size_t n = 8;
  float in[8] = {-5.0f, 0.5f, 1.5f, 3.0f, -1.0f, 0.0f, 2.5f, 10.0f};
  float out[8] = {0};
  psimd_f32x4 lo = psimd_set1_f32x4(0.0f);
  psimd_f32x4 hi = psimd_set1_f32x4(2.0f);

  for (size_t i = 0; i < n; i += 4) {
    psimd_f32x4 v = psimd_loadu_f32x4(in + i);
    psimd_storeu_f32x4(out + i, psimd_min_f32x4(psimd_max_f32x4(v, lo), hi));
  }

  float expected[8] = {0.0f, 0.5f, 1.5f, 2.0f, 0.0f, 0.0f, 2.0f, 2.0f};
  bool ok = true;
  for (size_t i = 0; i < n; i++)
    if (fabsf(out[i] - expected[i]) > f32_epsilon)
      ok = false;
  check(ok, "clamp [0, 2]");
}

/* ================================================================
   kernel: dot product
   ================================================================ */

static void test_kernel_dot() {
  printf("\nkernel: dot product\n");

  PSIMD_CONSTEXPR size_t n = 16;
  float a[16], b[16];
  float expected = 0.0f;
  for (size_t i = 0; i < n; i++) {
    a[i] = (float)(i + 1);
    b[i] = (float)(i + 1);
    expected += a[i] * b[i];
  }

  psimd_f32x4 acc = psimd_zero_f32x4();
  for (size_t i = 0; i < n; i += 4) {
    acc = psimd_fma_f32x4(psimd_loadu_f32x4(a + i), psimd_loadu_f32x4(b + i),
                          acc);
  }
  float result = psimd_reduce_add_f32x4(acc);
  check_f32(result, expected, "dot product via FMA+reduce");
}

/* ================================================================
   kernel: conditional blend
   ================================================================ */

static void test_kernel_blend() {
  printf("\nkernel: conditional blend\n");

  PSIMD_CONSTEXPR size_t n = 8;
  float src[8] = {1.0f, -1.0f, 2.0f, -2.0f, 3.0f, -3.0f, 4.0f, -4.0f};
  float sign[8] = {0};
  psimd_f32x4 zero = psimd_zero_f32x4();
  psimd_f32x4 pos = psimd_set1_f32x4(1.0f);
  psimd_f32x4 neg = psimd_set1_f32x4(-1.0f);

  for (size_t i = 0; i < n; i += 4) {
    psimd_f32x4 v = psimd_loadu_f32x4(src + i);
    psimd_mask32x4 gtz = psimd_cmpgt_f32x4(v, zero);
    psimd_mask32x4 ltz = psimd_cmplt_f32x4(v, zero);
    psimd_f32x4 r =
        psimd_select_f32x4(gtz, pos, psimd_select_f32x4(ltz, neg, zero));
    psimd_storeu_f32x4(sign + i, r);
  }

  float expected[8] = {1, -1, 1, -1, 1, -1, 1, -1};
  bool ok = true;
  for (size_t i = 0; i < n; i++)
    if (fabsf(sign[i] - expected[i]) > f32_epsilon)
      ok = false;
  check(ok, "sign extraction via nested select");
}

/* ================================================================
   main
   ================================================================ */

int main() {
  printf("psimd test suite\n");
  printf("backend: %s\n", backend_name(psimd_get_backend()));

  test_f32x4_construction();
  test_f32x4_arithmetic();
  test_f32x4_fma();
  test_f32x4_minmax();
  test_f32x4_comparisons();
  test_f32x4_select();
  test_f32x4_reductions();
  test_f32x4_math();
  test_f32x4_shuffle();
  test_mask32x4_ops();
  test_i32x4_construction();
  test_i32x4_arithmetic();
  test_i32x4_bitwise();
  test_i32x4_comparisons();
  test_i32x4_reductions();
  test_conversions();
  test_f32x8();
  test_i32x8();
  test_u32x4();
  test_kernel_array_ops();
  test_kernel_relu();
  test_kernel_clamp();
  test_kernel_dot();
  test_kernel_blend();

  printf("\n%d passed, %d failed\n", g_passed, g_failed);
  return g_failed > 0;
}
