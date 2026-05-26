#include "nanocron/nanocron.h"

#include <ctype.h>
#include <limits.h>
#include <stdckdint.h>
#include <stdlib.h>
#include <string.h>

/*
 * Security posture:
 * - all caller-provided schedules, timestamps, and timezone offsets are
 *   treated as untrusted input
 * - callbacks are trusted code running synchronously in the caller thread
 * - this module is not thread-safe; callers must serialize access per context
 */

static constexpr size_t CRON_FIELD_COUNT = 7;
static constexpr size_t CRON_MAX_ATOMS = 12;
static constexpr size_t CRON_MAX_SCHEDULE_LENGTH = 512;
static constexpr size_t CRON_DOM_FIELD = 4;
static constexpr size_t CRON_DOW_FIELD = 6;
static constexpr size_t CRON_SECONDS_PER_DAY = 86'400;
static constexpr size_t CRON_LOOKAHEAD_DAYS = 366;
static constexpr time_t CRON_LOOKAHEAD_SECONDS =
    (time_t)(CRON_LOOKAHEAD_DAYS * CRON_SECONDS_PER_DAY);
static constexpr int32_t CRON_TZ_OFFSET_MINUTES_MIN = -1'440;
static constexpr int32_t CRON_TZ_OFFSET_MINUTES_MAX = 1'440;

static_assert(999'999'999L <= LONG_MAX,
              "timespec tv_nsec must accommodate nanocron precision");

typedef struct {
  uint64_t start;
  uint64_t end;
  uint32_t step;
} cron_atom;

typedef struct {
  cron_atom atoms[CRON_MAX_ATOMS];
  size_t num_atoms;
  bool is_wildcard; /* true only if the field was exactly "*" */
} cron_field;

struct cron_job {
  cron_job_t *next;
  cron_field fields[CRON_FIELD_COUNT];
  cron_callback_t callback;
  void *user_data;
  struct timespec last_fired; /* de-duplication */
  bool has_last_fired;
  bool is_removed; /* deferred removal marker */
};

struct cron_ctx {
  cron_job_t *jobs;
  size_t execution_depth; /* >0 while inside cron_execute_due */
  bool destroy_requested;
  int32_t timezone_offset_minutes;
};

static constexpr uint64_t CRON_FIELD_MIN[CRON_FIELD_COUNT] = {0, 0, 0, 0,
                                                              1, 1, 0};
static constexpr uint64_t CRON_FIELD_MAX[CRON_FIELD_COUNT] = {
    999'999'999ULL, 59, 59, 23, 31, 12, 6};

[[nodiscard]]
static bool timespec_is_valid(const struct timespec *ts) {
  return ts != nullptr && ts->tv_nsec >= 0 && ts->tv_nsec <= 999'999'999L;
}

[[nodiscard]]
static int timespec_compare(const struct timespec *lhs,
                            const struct timespec *rhs) {
  if (lhs->tv_sec < rhs->tv_sec) {
    return -1;
  }
  if (lhs->tv_sec > rhs->tv_sec) {
    return 1;
  }
  if (lhs->tv_nsec < rhs->tv_nsec) {
    return -1;
  }
  if (lhs->tv_nsec > rhs->tv_nsec) {
    return 1;
  }
  return 0;
}

[[nodiscard]]
static bool seconds_to_schedule_tm(const cron_ctx_t *ctx, time_t utc_seconds,
                                   struct tm *out) {
  if (ctx == nullptr || out == nullptr) {
    return false;
  }

  time_t schedule_seconds = utc_seconds;
  if (ctx->timezone_offset_minutes != 0) {
    /* Multiplication is safe because the public API bounds the offset. */
    const time_t offset_seconds = (time_t)ctx->timezone_offset_minutes * 60;
    if (ckd_add(&schedule_seconds, utc_seconds, offset_seconds)) {
      return false;
    }
  }

  return gmtime_r(&schedule_seconds, out) != nullptr;
}

[[nodiscard]]
static bool schedule_length_ok(const char *schedule) {
  if (schedule == nullptr) {
    return false;
  }

  size_t len = 0;
  while (schedule[len] != '\0') {
    if (len >= CRON_MAX_SCHEDULE_LENGTH) {
      return false;
    }
    len++;
  }
  return true;
}

[[nodiscard]]
static bool parse_u64(const char **cursor, uint64_t minv, uint64_t maxv,
                      uint64_t *out) {
  if (cursor == nullptr || *cursor == nullptr || out == nullptr) {
    return false;
  }

  const char *p = *cursor;
  if (!isdigit((unsigned char)*p)) {
    return false;
  }

  uint64_t value = 0;
  while (isdigit((unsigned char)*p)) {
    const uint64_t digit = (uint64_t)(*p - '0');
    if (value > (UINT64_MAX - digit) / 10) {
      return false;
    }
    value = value * 10 + digit;
    p++;
  }

  if (value < minv || value > maxv) {
    return false;
  }

  *out = value;
  *cursor = p;
  return true;
}

[[nodiscard]]
static char *cron_strdup(const char *src) {
  if (src == nullptr) {
    return nullptr;
  }

  const size_t len = strlen(src);
  size_t alloc_size = 0;
  if (ckd_add(&alloc_size, len, (size_t)1)) {
    return nullptr;
  }

  char *copy = calloc(alloc_size, sizeof(char));
  if (copy == nullptr) {
    return nullptr;
  }
  memcpy(copy, src, len);
  return copy;
}

/* Hard part: parse one field. Supports lists, ranges, steps. */
static bool parse_cron_field(const char *str, uint64_t minv, uint64_t maxv,
                             cron_field *f) {
  f->num_atoms = 0;
  f->is_wildcard = false;

  if (str == nullptr || *str == '\0')
    return false;

  if (strcmp(str, "*") == 0) {
    f->is_wildcard = true;
    f->atoms[0].start = minv;
    f->atoms[0].end = maxv;
    f->atoms[0].step = 1;
    f->num_atoms = 1;
    return true;
  }

  char *copy = cron_strdup(str);
  if (copy == nullptr)
    return false;

  char *part = copy;
  while (part && *part) {
    if (f->num_atoms >= CRON_MAX_ATOMS) {
      free(copy);
      return false;
    }

    char *next = part;
    while (*next && *next != ',') {
      next++;
    }

    if (*next == ',') {
      *next = '\0';
      next++;
      if (*next == '\0') {
        goto fail;
      }
    } else {
      next = nullptr;
    }

    if (*part == '\0') {
      goto fail;
    }

    const char *p = part;
    uint64_t start = 0;
    uint64_t end = 0;
    uint64_t step_u64 = 1;
    bool had_range = false;

    /* start value */
    if (*p == '*') {
      start = minv;
      end = maxv;
      had_range = true;
      p++;
    } else {
      if (!parse_u64(&p, minv, maxv, &start))
        goto fail;
      end = start;
    }

    /* range */
    if (*p == '-') {
      had_range = true;
      p++;
      if (!parse_u64(&p, minv, maxv, &end) || end < start)
        goto fail;
    }

    /* step */
    if (*p == '/') {
      p++;
      if (!parse_u64(&p, 1, UINT64_MAX, &step_u64))
        goto fail;
    }

    if (*p != '\0')
      goto fail;

    /* "10/5" -> start=10, end=max (standard cron semantics) */
    if (step_u64 > 1 && !had_range) {
      end = maxv;
    }
    if (step_u64 > UINT32_MAX)
      goto fail;

    f->atoms[f->num_atoms].start = start;
    f->atoms[f->num_atoms].end = end;
    f->atoms[f->num_atoms].step = (uint32_t)step_u64;
    f->num_atoms++;

    part = next;
  }

  free(copy);
  return f->num_atoms > 0;

fail:
  free(copy);
  return false;
}

static bool parse_cron_expression(const char *expr,
                                  cron_field fields[CRON_FIELD_COUNT]) {
  if (expr == nullptr)
    return false;
  if (!schedule_length_ok(expr))
    return false;

  char *copy = cron_strdup(expr);
  if (copy == nullptr)
    return false;

  size_t idx = 0;
  char *cursor = copy;

  while (*cursor) {
    while (*cursor && isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (!*cursor) {
      break;
    }

    char *token = cursor;
    while (*cursor && !isspace((unsigned char)*cursor)) {
      cursor++;
    }
    if (*cursor) {
      *cursor = '\0';
      cursor++;
    }

    if (idx >= CRON_FIELD_COUNT) {
      free(copy);
      return false;
    }
    if (!parse_cron_field(token, CRON_FIELD_MIN[idx], CRON_FIELD_MAX[idx],
                          &fields[idx])) {
      free(copy);
      return false;
    }
    idx++;
  }

  free(copy);
  return idx == CRON_FIELD_COUNT; /* exactly 7 fields required */
}

static bool field_matches(const cron_field *f, uint64_t val) {
  for (size_t i = 0; i < f->num_atoms; i++) {
    const cron_atom *a = &f->atoms[i];
    if (val >= a->start && val <= a->end) {
      if (a->step == 1 || (val - a->start) % a->step == 0)
        return true;
    }
  }
  return false;
}

static bool day_fields_match(const cron_field *dom_field,
                             const cron_field *dow_field, uint64_t dom_value,
                             uint64_t dow_value) {
  const bool dom_match = field_matches(dom_field, dom_value);
  const bool dow_match = field_matches(dow_field, dow_value);

  if (dom_field->is_wildcard || dow_field->is_wildcard) {
    return dom_match && dow_match;
  }
  return dom_match || dow_match;
}

static bool non_day_fields_match(const cron_field fields[CRON_FIELD_COUNT],
                                 const uint64_t values[CRON_FIELD_COUNT],
                                 bool include_nanoseconds) {
  for (size_t i = 0; i < CRON_FIELD_COUNT; i++) {
    if (i == CRON_DOM_FIELD || i == CRON_DOW_FIELD) {
      continue;
    }
    if (!include_nanoseconds && i == 0) {
      continue;
    }
    if (!field_matches(&fields[i], values[i])) {
      return false;
    }
  }
  return true;
}

[[nodiscard]]
static bool field_next_match(const cron_field *f, uint64_t min_candidate,
                             uint64_t maxv, uint64_t *out) {
  if (f == nullptr || out == nullptr || min_candidate > maxv) {
    return false;
  }

  bool found = false;
  uint64_t best = 0;

  for (size_t i = 0; i < f->num_atoms; i++) {
    const cron_atom *atom = &f->atoms[i];
    if (atom->start > maxv) {
      continue;
    }

    const uint64_t atom_end = (atom->end < maxv) ? atom->end : maxv;
    if (min_candidate > atom_end) {
      continue;
    }

    uint64_t candidate = atom->start;
    if (candidate < min_candidate) {
      const uint64_t step = (uint64_t)atom->step;
      const uint64_t delta = min_candidate - atom->start;
      const uint64_t rem = delta % step;
      if (rem == 0) {
        candidate = min_candidate;
      } else {
        const uint64_t bump = step - rem;
        if (ckd_add(&candidate, min_candidate, bump)) {
          continue;
        }
      }
    }

    if (candidate > atom_end) {
      continue;
    }

    if (!found || candidate < best) {
      best = candidate;
      found = true;
    }
  }

  if (!found) {
    return false;
  }
  *out = best;
  return true;
}

[[nodiscard]]
static cron_job_t **find_job_link(cron_ctx_t *ctx, const cron_job_t *job) {
  if (ctx == nullptr || job == nullptr) {
    return nullptr;
  }

  cron_job_t **link = &ctx->jobs;
  while (*link && *link != job) {
    link = &(*link)->next;
  }
  return (*link == job) ? link : nullptr;
}

static void sweep_removed_jobs(cron_ctx_t *ctx) {
  if (ctx == nullptr) {
    return;
  }

  cron_job_t **link = &ctx->jobs;
  while (*link) {
    cron_job_t *job = *link;
    if (job->is_removed) {
      *link = job->next;
      free(job);
      continue;
    }
    link = &job->next;
  }
}

static void finalize_execution_scope(cron_ctx_t *ctx) {
  if (ctx == nullptr || ctx->execution_depth != 0) {
    return;
  }

  sweep_removed_jobs(ctx);
  if (ctx->destroy_requested) {
    cron_job_t *job = ctx->jobs;
    while (job) {
      cron_job_t *next = job->next;
      free(job);
      job = next;
    }
    free(ctx);
  }
}

[[nodiscard]]
cron_ctx_t *cron_create() {
  cron_ctx_t *ctx = calloc(1, sizeof(cron_ctx_t));
  if (ctx == nullptr) {
    return nullptr;
  }
  return ctx;
}

void cron_destroy(cron_ctx_t *ctx) {
  if (ctx == nullptr)
    return;
  if (ctx->execution_depth > 0) {
    ctx->destroy_requested = true;
    return;
  }
  cron_job_t *j = ctx->jobs;
  while (j) {
    cron_job_t *nxt = j->next;
    free(j);
    j = nxt;
  }
  free(ctx);
}

[[nodiscard]]
bool cron_set_timezone_offset_minutes(cron_ctx_t *ctx,
                                      int32_t utc_offset_minutes) {
  if (ctx == nullptr || ctx->destroy_requested) {
    return false;
  }
  if (utc_offset_minutes < CRON_TZ_OFFSET_MINUTES_MIN ||
      utc_offset_minutes > CRON_TZ_OFFSET_MINUTES_MAX) {
    return false;
  }

  ctx->timezone_offset_minutes = utc_offset_minutes;
  return true;
}

[[nodiscard]]
int32_t cron_get_timezone_offset_minutes(const cron_ctx_t *ctx) {
  if (ctx == nullptr) {
    return 0;
  }
  return ctx->timezone_offset_minutes;
}

cron_job_t *cron_add(cron_ctx_t *ctx, const char *schedule, cron_callback_t cb,
                     void *user_data) {
  if (ctx == nullptr || schedule == nullptr || cb == nullptr)
    return nullptr;
  if (ctx->destroy_requested)
    return nullptr;
  if (!schedule_length_ok(schedule))
    return nullptr;

  cron_field fields[CRON_FIELD_COUNT] = {0};
  if (!parse_cron_expression(schedule, fields))
    return nullptr;

  cron_job_t *job = calloc(1, sizeof(cron_job_t));
  if (job == nullptr)
    return nullptr;

  memcpy(job->fields, fields, sizeof(fields));
  job->callback = cb;
  job->user_data = user_data;

  job->next = ctx->jobs;
  ctx->jobs = job;
  return job;
}

bool cron_remove(cron_ctx_t *ctx, cron_job_t *job) {
  if (ctx == nullptr || ctx->destroy_requested) {
    return false;
  }
  cron_job_t **link = find_job_link(ctx, job);
  if (link == nullptr) {
    return false;
  }

  if (ctx->execution_depth > 0) {
    (*link)->is_removed = true;
    return true;
  }

  cron_job_t *victim = *link;
  *link = victim->next;
  free(victim);
  return true;
}

void cron_execute_due(cron_ctx_t *ctx, const struct timespec *now) {
  if (ctx == nullptr || now == nullptr) {
    return;
  }
  if (ctx->destroy_requested) {
    return;
  }
  if (!timespec_is_valid(now)) {
    return;
  }

  struct tm tm;
  if (!seconds_to_schedule_tm(ctx, now->tv_sec, &tm)) {
    return;
  }

  const uint64_t values[CRON_FIELD_COUNT] = {
      (uint64_t)now->tv_nsec, (uint64_t)tm.tm_sec,  (uint64_t)tm.tm_min,
      (uint64_t)tm.tm_hour,   (uint64_t)tm.tm_mday, (uint64_t)tm.tm_mon + 1,
      (uint64_t)tm.tm_wday};

  ctx->execution_depth++;
  cron_job_t *job = ctx->jobs;
  while (job) {
    if (ctx->destroy_requested) {
      break;
    }
    cron_job_t *next = job->next;

    if (job->is_removed) {
      job = next;
      continue;
    }

    if (!non_day_fields_match(job->fields, values, true)) {
      job = next;
      continue;
    }

    const bool day_ok = day_fields_match(
        &job->fields[CRON_DOM_FIELD], &job->fields[CRON_DOW_FIELD],
        values[CRON_DOM_FIELD], values[CRON_DOW_FIELD]);

    if (day_ok) {
      /* Fire only once per distinct nanosecond instant */
      if (!job->has_last_fired || now->tv_sec > job->last_fired.tv_sec ||
          (now->tv_sec == job->last_fired.tv_sec &&
           now->tv_nsec > job->last_fired.tv_nsec)) {

        job->last_fired = *now; /* set before callback for reentrant safety */
        job->has_last_fired = true;
        job->callback(job->user_data, now);
      }
    }
    job = next;
  }

  ctx->execution_depth--;
  finalize_execution_scope(ctx);
}

void cron_tick(cron_ctx_t *ctx) {
  struct timespec now;
  if (timespec_get(&now, TIME_UTC) == 0)
    return;
  cron_execute_due(ctx, &now);
}

[[nodiscard]]
bool cron_execute_between(cron_ctx_t *ctx, const struct timespec *after,
                          const struct timespec *until) {
  if (ctx == nullptr || after == nullptr || until == nullptr) {
    return false;
  }
  if (ctx->destroy_requested) {
    return false;
  }
  if (!timespec_is_valid(after) || !timespec_is_valid(until)) {
    return false;
  }
  if (timespec_compare(until, after) <= 0) {
    return true;
  }

  struct timespec cursor = *after;

  ctx->execution_depth++;
  while (!ctx->destroy_requested) {
    struct timespec next = {0};
    if (!cron_get_next_trigger(ctx, &cursor, &next)) {
      break;
    }
    if (timespec_compare(&next, until) > 0) {
      break;
    }
    cron_execute_due(ctx, &next);
    if (ctx->destroy_requested) {
      break;
    }
    cursor = next;
  }
  ctx->execution_depth--;
  finalize_execution_scope(ctx);

  return true;
}

[[nodiscard]]
bool cron_get_next_trigger(const cron_ctx_t *ctx, const struct timespec *after,
                           struct timespec *next_out) {
  if (ctx == nullptr || after == nullptr || next_out == nullptr)
    return false;
  if (ctx->destroy_requested)
    return false;
  if (!timespec_is_valid(after))
    return false;

  for (time_t sec_off = 0; sec_off < (time_t)CRON_LOOKAHEAD_SECONDS;
       sec_off++) {
    time_t sec = 0;
    if (ckd_add(&sec, after->tv_sec, sec_off)) {
      break;
    }

    struct tm tm;
    if (!seconds_to_schedule_tm(ctx, sec, &tm))
      break;

    const uint64_t values[CRON_FIELD_COUNT] = {
        (uint64_t)0,          (uint64_t)tm.tm_sec,  (uint64_t)tm.tm_min,
        (uint64_t)tm.tm_hour, (uint64_t)tm.tm_mday, (uint64_t)tm.tm_mon + 1,
        (uint64_t)tm.tm_wday};

    bool found_in_second = false;
    uint64_t best_ns = 0;

    const cron_job_t *job = ctx->jobs;
    while (job) {
      if (job->is_removed) {
        job = job->next;
        continue;
      }

      if (!non_day_fields_match(job->fields, values, false)) {
        job = job->next;
        continue;
      }
      if (!day_fields_match(&job->fields[CRON_DOM_FIELD],
                            &job->fields[CRON_DOW_FIELD],
                            values[CRON_DOM_FIELD], values[CRON_DOW_FIELD])) {
        job = job->next;
        continue;
      }

      uint64_t min_ns = 0;
      if (sec_off == 0) {
        if (after->tv_nsec >= 999'999'999L) {
          job = job->next;
          continue;
        }
        min_ns = (uint64_t)after->tv_nsec + 1;
      }

      uint64_t matched_ns = 0;
      if (field_next_match(&job->fields[0], min_ns, CRON_FIELD_MAX[0],
                           &matched_ns)) {
        if (!found_in_second || matched_ns < best_ns) {
          best_ns = matched_ns;
          found_in_second = true;
        }
      }

      job = job->next;
    }

    if (found_in_second) {
      next_out->tv_sec = sec;
      next_out->tv_nsec = (long)best_ns;
      return true;
    }
  }

  return false;
}
