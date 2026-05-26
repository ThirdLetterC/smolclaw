#pragma once

/*
 * nanocron.h - C23 CRON library with full nanosecond precision
 *
 * Schedule format (exactly 7 whitespace-separated fields):
 *   nanosecond (0-999999999)  second (0-59)  minute (0-59)  hour (0-23)
 *   day-of-month (1-31)  month (1-12)  day-of-week (0-6, 0=Sunday)
 *
 * Public input contract:
 * - schedule strings are untrusted input and must be at most 512 bytes
 *   excluding the trailing NUL
 * - `struct timespec` arguments with `tv_nsec` outside [0, 999999999] are
 *   rejected
 *
 * Standard vixie-cron DOM/DOW rule is implemented (when both fields are
 * restricted they are OR-ed, otherwise AND).
 *
 * Thread-safety: cron_ctx_t is not internally synchronized; callers must
 * serialize access to each context instance.
 *
 * Reentrant destroy: calling cron_destroy() from inside a callback is
 * supported. Destruction is deferred until the outermost cron_execute_due()
 * unwinds. After that call returns, the context pointer is invalid.
 *
 * Lifetime rules:
 * - a `cron_job_t *` becomes invalid immediately after successful
 *   `cron_remove()`
 * - a `cron_ctx_t *` becomes invalid immediately after `cron_destroy()`
 *   completes, including deferred destruction after execution unwinds
 */

#include <stdint.h>
#include <time.h>

typedef void (*cron_callback_t)(void *user_data,
                                const struct timespec *trigger_time);

typedef struct cron_job cron_job_t;
typedef struct cron_ctx cron_ctx_t;

[[nodiscard]]
cron_ctx_t *cron_create();

void cron_destroy(cron_ctx_t *ctx);

/* Returns opaque job handle for later removal, nullptr on parse error. */
[[nodiscard]]
cron_job_t *cron_add(cron_ctx_t *ctx, const char *schedule, cron_callback_t cb,
                     void *user_data);

[[nodiscard]]
bool cron_remove(cron_ctx_t *ctx, cron_job_t *job);

/* Call with current time (UTC). Fires every matching job exactly once per
 * instant, using the context's configured timezone offset for schedule
 * matching. */
void cron_execute_due(cron_ctx_t *ctx, const struct timespec *now);

/* Convenience: use current UTC time via standard timespec_get */
void cron_tick(cron_ctx_t *ctx);

/* Set fixed timezone offset used for schedule evaluation (minutes from UTC).
 * Default is 0 (UTC). This is a fixed offset and does not apply DST rules. */
[[nodiscard]]
bool cron_set_timezone_offset_minutes(cron_ctx_t *ctx,
                                      int32_t utc_offset_minutes);

/* Returns fixed timezone offset in minutes from UTC (or 0 for nullptr). */
[[nodiscard]]
int32_t cron_get_timezone_offset_minutes(const cron_ctx_t *ctx);

/* Execute all scheduled instants in the window (`after`, `until`] (UTC). */
[[nodiscard]]
bool cron_execute_between(cron_ctx_t *ctx, const struct timespec *after,
                          const struct timespec *until);

/* Compute next trigger strictly after `after` (search horizon: 366 days). */
[[nodiscard]]
bool cron_get_next_trigger(const cron_ctx_t *ctx, const struct timespec *after,
                           struct timespec *next_out);
