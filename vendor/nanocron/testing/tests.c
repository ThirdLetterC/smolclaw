/*
 * tests.c - Comprehensive unit test suite for nanocron (C23)
 *
 * Covers: create/destroy, parsing (valid/invalid), firing, nanosecond
 * precision, DOM/DOW vixie-cron rule, de-duplication, multiple jobs, removal,
 *         next-trigger calculation.
 *
 * Compile:
 *   gcc -std=c23 -Wall -Wextra -pedantic -O2 tests.c -o tests
 *   ./tests
 */

#include "nanocron/nanocron.h"

#include <inttypes.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

static size_t tests_passed = 0;
static size_t tests_failed = 0;

#define TEST(name)                                                             \
  do {                                                                         \
    printf("Running: %s\n", #name);                                            \
    if (run_test_##name()) {                                                   \
      printf("  ✓ PASSED\n");                                                  \
      tests_passed++;                                                          \
    } else {                                                                   \
      printf("  ✗ FAILED\n");                                                  \
      tests_failed++;                                                          \
    }                                                                          \
  } while (0)

static struct timespec make_ts(time_t sec, int32_t nsec) {
  struct timespec ts = {.tv_sec = sec, .tv_nsec = nsec};
  return ts;
}

static size_t callback_count = 0;
static struct timespec last_call_ts = {0};
static cron_job_t *self_remove_job = nullptr;
static bool self_remove_result = false;
static cron_job_t *other_job_handle = nullptr;
static bool remove_other_result = false;
static size_t remover_callback_count = 0;
static size_t reentrant_callback_count = 0;
static cron_job_t *guard_victim_job = nullptr;
static bool guard_remove_victim_result = false;
static bool guard_removed_job_visible_next = false;
static bool guard_set_tz_after_destroy = true;
static cron_job_t *guard_add_after_destroy = (cron_job_t *)1;
static bool guard_remove_after_destroy = true;
static bool guard_between_after_destroy = true;
static bool guard_next_after_destroy = true;
static size_t destroy_guard_callback_count = 0;

static void test_callback([[maybe_unused]] void *user_data,
                          [[maybe_unused]] const struct timespec *ts) {
  callback_count++;
  last_call_ts = *ts;
}

static void reset_callback() {
  callback_count = 0;
  last_call_ts.tv_sec = 0;
  last_call_ts.tv_nsec = 0;
}

static void self_remove_callback(void *user_data,
                                 [[maybe_unused]] const struct timespec *ts) {
  cron_ctx_t *ctx = user_data;
  callback_count++;
  self_remove_result = cron_remove(ctx, self_remove_job);
}

static void remove_other_callback(void *user_data,
                                  [[maybe_unused]] const struct timespec *ts) {
  cron_ctx_t *ctx = user_data;
  remover_callback_count++;

  if (other_job_handle != nullptr) {
    remove_other_result = cron_remove(ctx, other_job_handle);
    if (remove_other_result) {
      other_job_handle = nullptr;
    }
  }
}

static void reentrant_callback(void *user_data, const struct timespec *ts) {
  cron_ctx_t *ctx = user_data;
  reentrant_callback_count++;
  if (reentrant_callback_count == 1) {
    cron_execute_due(ctx, ts);
  }
}

static void destroy_ctx_callback(void *user_data,
                                 [[maybe_unused]] const struct timespec *ts) {
  cron_ctx_t *ctx = user_data;
  callback_count++;
  cron_destroy(ctx);
}

static void destroy_guard_callback(void *user_data, const struct timespec *ts) {
  cron_ctx_t *ctx = user_data;
  destroy_guard_callback_count++;

  if (guard_victim_job != nullptr) {
    guard_remove_victim_result = cron_remove(ctx, guard_victim_job);
    guard_victim_job = nullptr;
  }

  struct timespec next = {0};
  guard_removed_job_visible_next = cron_get_next_trigger(ctx, ts, &next);

  cron_destroy(ctx);

  guard_set_tz_after_destroy = cron_set_timezone_offset_minutes(ctx, 60);
  guard_add_after_destroy =
      cron_add(ctx, "0 * * * * * *", test_callback, nullptr);
  guard_remove_after_destroy = cron_remove(ctx, nullptr);

  struct timespec until = *ts;
  until.tv_sec++;
  guard_between_after_destroy = cron_execute_between(ctx, ts, &until);
  guard_next_after_destroy = cron_get_next_trigger(ctx, ts, &next);
}

/* ================================================================ */

static bool run_test_create_destroy() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr)
    return false;
  cron_destroy(ctx);
  return true;
}

static bool run_test_invalid_schedules() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr)
    return false;

  const char *bad[] = {"",                                 /* empty */
                       "* * * * *",                        /* 5 fields */
                       "* * * * * * * *",                  /* 8 fields */
                       "1000000000 * * * * * *",           /* ns > 999999999 */
                       "abc * * * * * *",                  /* invalid token */
                       "* 60 * * * * *",                   /* sec > 59 */
                       "18446744073709551616 * * * * * *", /* u64 overflow */
                       "0, * * * * * *",                   /* trailing comma */
                       "1-2-3 * * * * * *",                /* malformed range */
                       "0-10/0 * * * * * *",               /* zero step */
                       "0/4294967296 * * * * * *",         /* step > uint32 */
                       "0,1,2,3,4,5,6,7,8,9,10,11,12 * * * * * *", /* >12 */
                       nullptr};

  for (size_t i = 0; bad[i]; i++) {
    if (cron_add(ctx, bad[i], test_callback, nullptr) != nullptr) {
      cron_destroy(ctx);
      return false;
    }
  }
  cron_destroy(ctx);
  return true;
}

static bool run_test_schedule_length_limit() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr) {
    return false;
  }

  constexpr size_t long_len = 600;
  char too_long[long_len + 1];
  for (size_t i = 0; i < long_len; i++) {
    too_long[i] = '1';
  }
  too_long[long_len] = '\0';

  const bool rejected =
      cron_add(ctx, too_long, test_callback, nullptr) == nullptr;
  cron_destroy(ctx);
  return rejected;
}

static bool run_test_whitespace_and_step_parsing() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_add(ctx, "   */250000000  * * * * * *   ", test_callback, nullptr) ==
      nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec t = make_ts(1739788200, 500000000);
  cron_execute_due(ctx, &t);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_every_second() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();

  if (cron_add(ctx, "0 * * * * * *", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec t = make_ts(1739788200, 0); /* 2025-02-17 10:30:00 UTC */
  cron_execute_due(ctx, &t);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  /* same instant → dedup */
  cron_execute_due(ctx, &t);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  t.tv_sec++;
  cron_execute_due(ctx, &t);
  if (callback_count != 2) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_nanosecond_precision() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();

  if (cron_add(ctx, "250000000,750000000 * * * * * *", test_callback,
               nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec t = make_ts(1739788200, 250000000);
  cron_execute_due(ctx, &t);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  t.tv_nsec = 750000000;
  cron_execute_due(ctx, &t);
  if (callback_count != 2) {
    cron_destroy(ctx);
    return false;
  }

  t.tv_nsec = 500000000; /* must not fire */
  cron_execute_due(ctx, &t);
  if (callback_count != 2) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_dom_dow_logic() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();

  /* 00:00:00.000 on the 1st of any month OR on Fridays (DOW=5) */
  if (cron_add(ctx, "0 0 0 0 1 * 5", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  /* 2025-02-01 00:00:00 (Saturday + 1st of month) → fires */
  struct timespec t1 = make_ts(1738368000, 0);
  cron_execute_due(ctx, &t1);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  /* 2025-02-07 00:00:00 (Friday, not 1st) → fires */
  reset_callback();
  struct timespec t2 = make_ts(1738886400, 0);
  cron_execute_due(ctx, &t2);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  /* 2025-02-03 00:00:00 (Monday, neither) → no fire */
  reset_callback();
  struct timespec t3 = make_ts(1738531200, 0);
  cron_execute_due(ctx, &t3);
  if (callback_count != 0) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_weekdays() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();

  /* 09:00:00.000 every Monday–Friday */
  if (cron_add(ctx, "0 0 0 9 * * 1-5", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  /* 2025-02-17 Monday 09:00:00 → fires */
  struct timespec mon = make_ts(1739782800, 0);
  cron_execute_due(ctx, &mon);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  /* 2025-02-16 Sunday 09:00:00 → no fire */
  reset_callback();
  struct timespec sun = make_ts(1739523600, 0);
  cron_execute_due(ctx, &sun);
  if (callback_count != 0) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_timezone_offset_execute_due() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  if (ctx == nullptr) {
    return false;
  }

  if (!cron_set_timezone_offset_minutes(ctx, 120)) {
    cron_destroy(ctx);
    return false;
  }

  /* 09:30 local time (UTC+02:00), Monday-Friday */
  if (cron_add(ctx, "0 0 30 9 * * 1-5", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  /* 2025-02-17 07:30:00 UTC -> 09:30:00 local Monday -> fires */
  struct timespec t = make_ts(1739777400, 0);
  cron_execute_due(ctx, &t);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  /* 2025-02-17 09:30:00 UTC -> 11:30:00 local -> no additional fire */
  t = make_ts(1739784600, 0);
  cron_execute_due(ctx, &t);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_timezone_offset_execute_due_negative() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  if (ctx == nullptr) {
    return false;
  }

  if (!cron_set_timezone_offset_minutes(ctx, -120)) {
    cron_destroy(ctx);
    return false;
  }

  /* 09:30 local time (UTC-02:00), Monday-Friday */
  if (cron_add(ctx, "0 0 30 9 * * 1-5", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  /* 2025-02-17 11:30:00 UTC -> 09:30:00 local Monday -> fires */
  struct timespec t = make_ts(1739791800, 0);
  cron_execute_due(ctx, &t);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  /* 2025-02-17 09:30:00 UTC -> 07:30:00 local -> no additional fire */
  t = make_ts(1739784600, 0);
  cron_execute_due(ctx, &t);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_timezone_offset_next_trigger() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr) {
    return false;
  }

  if (!cron_set_timezone_offset_minutes(ctx, 120)) {
    cron_destroy(ctx);
    return false;
  }

  /* 09:30 local time (UTC+02:00), Monday-Friday */
  if (cron_add(ctx, "0 0 30 9 * * 1-5", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec after = make_ts(1739777399, 0);
  struct timespec next = {0};
  if (!cron_get_next_trigger(ctx, &after, &next)) {
    cron_destroy(ctx);
    return false;
  }

  if (next.tv_sec != 1739777400 || next.tv_nsec != 0) {
    fprintf(stderr, "  expected 1739777400.000000000, got %lld.%09ld\n",
            (long long)next.tv_sec, next.tv_nsec);
    cron_destroy(ctx);
    return false;
  }

  after = make_ts(1739777400, 0); /* exact trigger instant */
  if (!cron_get_next_trigger(ctx, &after, &next)) {
    cron_destroy(ctx);
    return false;
  }

  if (next.tv_sec != 1739863800 || next.tv_nsec != 0) {
    fprintf(stderr, "  expected 1739863800.000000000, got %lld.%09ld\n",
            (long long)next.tv_sec, next.tv_nsec);
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_timezone_offset_validation() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_get_timezone_offset_minutes(ctx) != 0) {
    cron_destroy(ctx);
    return false;
  }

  if (cron_set_timezone_offset_minutes(ctx, 1'441)) {
    cron_destroy(ctx);
    return false;
  }
  if (cron_set_timezone_offset_minutes(ctx, -1'441)) {
    cron_destroy(ctx);
    return false;
  }

  if (!cron_set_timezone_offset_minutes(ctx, -480)) {
    cron_destroy(ctx);
    return false;
  }
  if (cron_get_timezone_offset_minutes(ctx) != -480) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_job_removal() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();

  cron_job_t *job = cron_add(ctx, "0 * * * * * *", test_callback, nullptr);
  if (job == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec t = make_ts(1739788200, 0);
  cron_execute_due(ctx, &t);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  if (!cron_remove(ctx, job)) {
    cron_destroy(ctx);
    return false;
  }

  reset_callback();
  cron_execute_due(ctx, &t);
  if (callback_count != 0) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_callback_self_removal() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  self_remove_job = nullptr;
  self_remove_result = false;

  self_remove_job = cron_add(ctx, "0 * * * * * *", self_remove_callback, ctx);
  if (self_remove_job == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec t = make_ts(1739788200, 0);
  cron_execute_due(ctx, &t);
  if (callback_count != 1 || !self_remove_result) {
    cron_destroy(ctx);
    return false;
  }

  t.tv_sec++;
  cron_execute_due(ctx, &t);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  self_remove_job = nullptr;
  return true;
}

static bool run_test_callback_destroy_context() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_add(ctx, "0 * * * * * *", destroy_ctx_callback, ctx) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec t = make_ts(1739788200, 0);
  cron_execute_due(ctx, &t);

  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  return true;
}

static bool run_test_callback_remove_other_job() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  other_job_handle = nullptr;
  remove_other_result = false;
  remover_callback_count = 0;

  other_job_handle = cron_add(ctx, "0 * * * * * *", test_callback, nullptr);
  if (other_job_handle == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  if (cron_add(ctx, "0 * * * * * *", remove_other_callback, ctx) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec t = make_ts(1739788200, 0);
  cron_execute_due(ctx, &t);
  if (remover_callback_count != 1 || !remove_other_result ||
      callback_count != 0) {
    cron_destroy(ctx);
    return false;
  }

  t.tv_sec++;
  cron_execute_due(ctx, &t);
  if (callback_count != 0 || remover_callback_count != 2) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  other_job_handle = nullptr;
  return true;
}

static bool run_test_reentrant_execute_due_dedup() {
  cron_ctx_t *ctx = cron_create();
  reentrant_callback_count = 0;

  if (cron_add(ctx, "0 * * * * * *", reentrant_callback, ctx) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec t = make_ts(1739788200, 0);
  cron_execute_due(ctx, &t);
  if (reentrant_callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  t.tv_sec++;
  cron_execute_due(ctx, &t);
  if (reentrant_callback_count != 2) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_api_input_validation() {
  cron_destroy(nullptr);

  if (cron_get_timezone_offset_minutes(nullptr) != 0) {
    return false;
  }
  if (cron_set_timezone_offset_minutes(nullptr, 60)) {
    return false;
  }

  if (cron_add(nullptr, "0 * * * * * *", test_callback, nullptr) != nullptr) {
    return false;
  }

  cron_ctx_t *ctx = cron_create();
  cron_ctx_t *ctx_other = cron_create();
  reset_callback();
  if (ctx == nullptr || ctx_other == nullptr) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }

  if (cron_add(ctx, nullptr, test_callback, nullptr) != nullptr) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }
  if (cron_add(ctx, "0 * * * * * *", nullptr, nullptr) != nullptr) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }

  if (cron_remove(nullptr, nullptr)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }
  if (cron_remove(ctx, nullptr)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }

  cron_job_t *foreign_job =
      cron_add(ctx_other, "0 * * * * * *", test_callback, nullptr);
  if (foreign_job == nullptr) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }
  if (cron_remove(ctx, foreign_job)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }

  if (cron_add(ctx, "* * * * * * *", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }

  struct timespec invalid_low = make_ts(1739788200, -1);
  struct timespec invalid_high = make_ts(1739788200, 1'000'000'000);
  cron_execute_due(ctx, &invalid_low);
  cron_execute_due(ctx, &invalid_high);
  if (callback_count != 0) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }

  struct timespec valid = make_ts(1739788200, 0);
  cron_execute_due(nullptr, &valid);
  cron_execute_due(ctx, nullptr);

  struct timespec next = {0};
  if (cron_get_next_trigger(nullptr, &valid, &next)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }
  if (cron_get_next_trigger(ctx, nullptr, &next)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }
  if (cron_get_next_trigger(ctx, &valid, nullptr)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }
  if (cron_get_next_trigger(ctx, &invalid_low, &next)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }
  if (cron_get_next_trigger(ctx, &invalid_high, &next)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }

  if (cron_execute_between(nullptr, &valid, &valid)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }
  if (cron_execute_between(ctx, nullptr, &valid)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }
  if (cron_execute_between(ctx, &valid, nullptr)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }
  if (cron_execute_between(ctx, &invalid_low, &valid)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }
  if (cron_execute_between(ctx, &valid, &invalid_high)) {
    cron_destroy(ctx);
    cron_destroy(ctx_other);
    return false;
  }

  cron_destroy(ctx);
  cron_destroy(ctx_other);
  return true;
}

static bool run_test_destroy_requested_guards_and_iteration_break() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  if (ctx == nullptr) {
    return false;
  }

  guard_victim_job = nullptr;
  guard_remove_victim_result = false;
  guard_removed_job_visible_next = false;
  guard_set_tz_after_destroy = true;
  guard_add_after_destroy = (cron_job_t *)1;
  guard_remove_after_destroy = true;
  guard_between_after_destroy = true;
  guard_next_after_destroy = true;
  destroy_guard_callback_count = 0;

  guard_victim_job = cron_add(ctx, "0 * * * * * *", test_callback, nullptr);
  if (guard_victim_job == nullptr) {
    cron_destroy(ctx);
    return false;
  }
  if (cron_add(ctx, "0 * * * * * *", destroy_guard_callback, ctx) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec t = make_ts(1739788200, 0);
  cron_execute_due(ctx, &t);

  return destroy_guard_callback_count == 1 && callback_count == 0 &&
         guard_remove_victim_result && guard_removed_job_visible_next &&
         !guard_set_tz_after_destroy && guard_add_after_destroy == nullptr &&
         !guard_remove_after_destroy && !guard_between_after_destroy &&
         !guard_next_after_destroy;
}

static bool run_test_next_trigger() {
  cron_ctx_t *ctx = cron_create();

  /* Weekdays at 09:30:00.000 */
  if (cron_add(ctx, "0 0 30 9 * * 1-5", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec after = make_ts(1739788200, 0); /* 2025-02-17 10:30 Monday */
  struct timespec next;

  bool found = cron_get_next_trigger(ctx, &after, &next);
  if (!found) {
    cron_destroy(ctx);
    return false;
  }

  /* next trigger = 2025-02-18 09:30:00 (Tuesday) */
  const int64_t expected_next = 1'739'871'000LL;
  const int64_t observed_next = (int64_t)next.tv_sec;
  if (observed_next != expected_next) {
    fprintf(stderr, "  expected %" PRId64 ", got %" PRId64 "\n", expected_next,
            observed_next);
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_next_trigger_nanoseconds_and_strict() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_add(ctx, "0,500000000 * * * * * *", test_callback, nullptr) ==
      nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec after = make_ts(1739788200, 0);
  struct timespec next;
  if (!cron_get_next_trigger(ctx, &after, &next)) {
    cron_destroy(ctx);
    return false;
  }

  if (next.tv_sec != 1739788200 || next.tv_nsec != 500000000L) {
    fprintf(stderr, "  expected 1739788200.500000000, got %lld.%09ld\n",
            (long long)next.tv_sec, next.tv_nsec);
    cron_destroy(ctx);
    return false;
  }

  after = make_ts(1739788200, 500000000);
  if (!cron_get_next_trigger(ctx, &after, &next)) {
    cron_destroy(ctx);
    return false;
  }

  if (next.tv_sec != 1739788201 || next.tv_nsec != 0) {
    fprintf(stderr, "  expected 1739788201.000000000, got %lld.%09ld\n",
            (long long)next.tv_sec, next.tv_nsec);
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_next_trigger_dom_dow_logic() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr) {
    return false;
  }

  /* 00:00:00.000 on day-of-month=1 OR day-of-week=Friday */
  if (cron_add(ctx, "0 0 0 0 1 * 5", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec after = make_ts(1738540800, 0); /* 2025-02-03 Monday */
  struct timespec next;
  if (!cron_get_next_trigger(ctx, &after, &next)) {
    cron_destroy(ctx);
    return false;
  }

  if (next.tv_sec != 1738886400 || next.tv_nsec != 0) {
    fprintf(stderr, "  expected 1738886400.000000000, got %lld.%09ld\n",
            (long long)next.tv_sec, next.tv_nsec);
    cron_destroy(ctx);
    return false;
  }

  after = make_ts(1738886400, 0); /* exactly at Friday trigger */
  if (!cron_get_next_trigger(ctx, &after, &next)) {
    cron_destroy(ctx);
    return false;
  }

  if (next.tv_sec != 1739491200 || next.tv_nsec != 0) {
    fprintf(stderr, "  expected 1739491200.000000000, got %lld.%09ld\n",
            (long long)next.tv_sec, next.tv_nsec);
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_next_trigger_field_alignment() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_add(ctx, "10/5 * * * * * *", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec after = make_ts(1739788200, 12);
  struct timespec next = {0};
  if (!cron_get_next_trigger(ctx, &after, &next)) {
    cron_destroy(ctx);
    return false;
  }
  if (next.tv_sec != 1739788200 || next.tv_nsec != 15) {
    cron_destroy(ctx);
    return false;
  }

  after = make_ts(1739788200, 14);
  if (!cron_get_next_trigger(ctx, &after, &next)) {
    cron_destroy(ctx);
    return false;
  }
  if (next.tv_sec != 1739788200 || next.tv_nsec != 15) {
    cron_destroy(ctx);
    return false;
  }

  after = make_ts(1739788200, 999999999);
  if (!cron_get_next_trigger(ctx, &after, &next)) {
    cron_destroy(ctx);
    return false;
  }
  if (next.tv_sec != 1739788201 || next.tv_nsec != 10) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);

  ctx = cron_create();
  if (ctx == nullptr) {
    return false;
  }
  if (cron_add(ctx, "10-12/5 * * * * * *", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  after = make_ts(1739788200, 10);
  if (!cron_get_next_trigger(ctx, &after, &next)) {
    cron_destroy(ctx);
    return false;
  }
  if (next.tv_sec != 1739788201 || next.tv_nsec != 10) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_next_trigger_lookahead_bound() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_add(ctx, "0 0 0 0 29 2 *", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec after = make_ts(1740787200, 0); /* 2025-03-01 00:00:00 UTC */
  struct timespec next = {0};
  if (cron_get_next_trigger(ctx, &after, &next)) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_execute_between_catch_up() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_add(ctx, "0 * * * * * *", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  const struct timespec after = make_ts(1739788200, 0);
  const struct timespec until = make_ts(1739788203, 0);
  if (!cron_execute_between(ctx, &after, &until)) {
    cron_destroy(ctx);
    return false;
  }

  if (callback_count != 3 || last_call_ts.tv_sec != 1739788203 ||
      last_call_ts.tv_nsec != 0) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_execute_between_strict_lower_bound() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_add(ctx, "0,500000000 * * * * * *", test_callback, nullptr) ==
      nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec after = make_ts(1739788200, 0);
  struct timespec until = make_ts(1739788200, 500000000);
  if (!cron_execute_between(ctx, &after, &until)) {
    cron_destroy(ctx);
    return false;
  }

  if (callback_count != 1 || last_call_ts.tv_sec != 1739788200 ||
      last_call_ts.tv_nsec != 500000000L) {
    cron_destroy(ctx);
    return false;
  }

  reset_callback();
  after = make_ts(1739788200, 500000000);
  until = make_ts(1739788201, 0);
  if (!cron_execute_between(ctx, &after, &until)) {
    cron_destroy(ctx);
    return false;
  }

  if (callback_count != 1 || last_call_ts.tv_sec != 1739788201 ||
      last_call_ts.tv_nsec != 0) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_execute_between_reverse_window_noop() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_add(ctx, "0 * * * * * *", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  const struct timespec after = make_ts(1739788205, 0);
  const struct timespec until = make_ts(1739788204, 0);
  if (!cron_execute_between(ctx, &after, &until)) {
    cron_destroy(ctx);
    return false;
  }

  if (callback_count != 0) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_execute_between_edge_cases() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  if (ctx == nullptr) {
    return false;
  }

  /* Same-second reverse window exercises nsec ordering branch. */
  struct timespec after = make_ts(1739788200, 500000000);
  struct timespec until = make_ts(1739788200, 400000000);
  if (!cron_execute_between(ctx, &after, &until)) {
    cron_destroy(ctx);
    return false;
  }

  /* No jobs: window walk should stop when no next trigger exists. */
  after = make_ts(1739788200, 0);
  until = make_ts(1739788205, 0);
  if (!cron_execute_between(ctx, &after, &until)) {
    cron_destroy(ctx);
    return false;
  }
  if (callback_count != 0) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_execute_between_lookahead_bound() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_add(ctx, "0 0 0 0 29 2 *", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  const struct timespec after = make_ts(1740787200, 0);   /* 2025-03-01 UTC */
  const struct timespec until = make_ts(1835395200, 0);   /* 2028-03-29 UTC */
  if (!cron_execute_between(ctx, &after, &until)) {
    cron_destroy(ctx);
    return false;
  }

  if (callback_count != 0) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_execute_between_destroy_requested_break() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_add(ctx, "0 * * * * * *", destroy_ctx_callback, ctx) == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  const struct timespec after = make_ts(1739788200, 0);
  const struct timespec until = make_ts(1739788203, 0);
  if (!cron_execute_between(ctx, &after, &until)) {
    return false;
  }

  return callback_count == 1;
}

static bool run_test_tick_smoke() {
  cron_tick(nullptr);

  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr) {
    return false;
  }

  if (cron_add(ctx, "* * * * * * *", test_callback, nullptr) == nullptr) {
    cron_destroy(ctx);
    return false;
  }
  reset_callback();
  cron_tick(ctx);
  if (callback_count > 1) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

static bool run_test_multiple_jobs() {
  cron_ctx_t *ctx = cron_create();
  reset_callback();

  cron_job_t *job_a = cron_add(ctx, "0 * * * * * *", test_callback,
                               nullptr); /* every second at ns=0 */
  if (job_a == nullptr) {
    cron_destroy(ctx);
    return false;
  }
  cron_job_t *job_b = cron_add(ctx, "500000000 * * * * * *", test_callback,
                               nullptr); /* every .5s */
  if (job_b == nullptr) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec t1 = make_ts(1739788200, 0);
  cron_execute_due(ctx, &t1);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  struct timespec t2 = make_ts(1739788200, 500000000);
  reset_callback();
  cron_execute_due(ctx, &t2);
  if (callback_count != 1) {
    cron_destroy(ctx);
    return false;
  }

  cron_destroy(ctx);
  return true;
}

/* ================================================================ */

int main() {
  printf("=== nanocron Test Suite (C23) ===\n\n");

  TEST(create_destroy);
  TEST(invalid_schedules);
  TEST(schedule_length_limit);
  TEST(whitespace_and_step_parsing);
  TEST(every_second);
  TEST(nanosecond_precision);
  TEST(dom_dow_logic);
  TEST(weekdays);
  TEST(timezone_offset_execute_due);
  TEST(timezone_offset_execute_due_negative);
  TEST(timezone_offset_next_trigger);
  TEST(timezone_offset_validation);
  TEST(job_removal);
  TEST(callback_self_removal);
  TEST(callback_destroy_context);
  TEST(callback_remove_other_job);
  TEST(api_input_validation);
  TEST(destroy_requested_guards_and_iteration_break);
  TEST(next_trigger);
  TEST(next_trigger_nanoseconds_and_strict);
  TEST(next_trigger_dom_dow_logic);
  TEST(next_trigger_field_alignment);
  TEST(next_trigger_lookahead_bound);
  TEST(execute_between_catch_up);
  TEST(execute_between_strict_lower_bound);
  TEST(execute_between_reverse_window_noop);
  TEST(execute_between_edge_cases);
  TEST(execute_between_lookahead_bound);
  TEST(execute_between_destroy_requested_break);
  TEST(reentrant_execute_due_dedup);
  TEST(tick_smoke);
  TEST(multiple_jobs);

  printf("\n=== SUMMARY ===\n");
  printf("Passed: %zu\n", tests_passed);
  printf("Failed: %zu\n", tests_failed);

  if (tests_failed == 0) {
    printf("ALL TESTS PASSED\n");
    return 0;
  } else {
    printf("%zu test(s) FAILED\n", tests_failed);
    return 1;
  }
}
