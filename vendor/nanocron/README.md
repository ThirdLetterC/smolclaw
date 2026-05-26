# nanocron

Small C23 cron scheduler library with nanosecond precision.

## Features

- Public API header at `include/nanocron/nanocron.h`.
- Implementation in `src/nanocron.c`.
- Full 7-field cron format with nanosecond field.
- Strict input validation for schedule strings and `struct timespec` values.
- Supports wildcard, exact value, ranges, lists, and step expressions.
- Implements vixie-cron day-of-month/day-of-week semantics:
  - If both DOM and DOW are restricted, they are OR-ed.
  - If either DOM or DOW is `*`, they are AND-ed.
- Per-context fixed timezone offset support for schedule evaluation.
- `cron_get_next_trigger` computes the next matching instant strictly after a given time.
- `cron_execute_between` replays missed instants over a bounded UTC time window.
- C23-first implementation with strict warning flags in project build scripts.

## Schedule Format

Every schedule must contain exactly 7 whitespace-separated fields:

`nanosecond second minute hour day_of_month month day_of_week`

Additional parser limits:

- maximum schedule length: `512` bytes excluding the trailing NUL
- maximum atoms per field: `12`

| Field | Range |
| --- | --- |
| nanosecond | `0-999999999` |
| second | `0-59` |
| minute | `0-59` |
| hour | `0-23` |
| day_of_month | `1-31` |
| month | `1-12` |
| day_of_week | `0-6` (`0 = Sunday`) |

Supported syntax per field:

- `*` for any value
- `42` for exact value
- `10-20` for range
- `1,3,5` for list
- `*/15` for step from field minimum
- `10-50/5` for step inside a range

Examples:

- `0 * * * * * *` -> every second on the nanosecond boundary
- `*/100000000 * * * * * *` -> every 100ms
- `0 0 30 9 * * 1-5` -> weekdays at `09:30:00.000000000` UTC
- `0 0 0 0 1 * 5` -> midnight on day `1` of the month OR Friday

## Quick Start

```c
#include <stdio.h>
#include <time.h>
#include "nanocron/nanocron.h"

static void on_fire([[maybe_unused]] void *user_data,
                    const struct timespec *ts) {
  printf("fired at %lld.%09ld\n", (long long)ts->tv_sec, ts->tv_nsec);
}

int main() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr) {
    return 1;
  }

  if (cron_add(ctx, "0 */5 * * * * *", on_fire, nullptr) == nullptr) {
    cron_destroy(ctx);
    return 1;
  }

  while (true) {
    cron_tick(ctx);

    struct timespec sleep_for = {.tv_sec = 0, .tv_nsec = 1'000'000}; /* 1ms */
    nanosleep(&sleep_for, nullptr);
  }

  cron_destroy(ctx);
  return 0;
}
```

Compile manually:

```bash
clang -std=c23 -Wall -Wextra -Wpedantic -Werror \
  -fstack-protector-strong -D_FORTIFY_SOURCE=3 -D_POSIX_C_SOURCE=200809L \
  -fPIE -Iinclude examples/simple.c src/nanocron.c \
  -Wl,-z,relro,-z,now -pie -o examples/simple
```

## API

- `cron_ctx_t *cron_create()`
  - Allocates and returns a scheduler context, or `nullptr` on failure.
- `void cron_destroy(cron_ctx_t *ctx)`
  - Frees all jobs and the context.
  - After it completes, the `cron_ctx_t *` is invalid.
- `cron_job_t *cron_add(cron_ctx_t *ctx, const char *schedule, cron_callback_t cb, void *user_data)`
  - Parses and registers a job. Returns a job handle or `nullptr`.
  - Schedule strings longer than `512` bytes or with more than `12` atoms in a field are rejected.
- `bool cron_remove(cron_ctx_t *ctx, cron_job_t *job)`
  - Removes a previously-added job handle.
  - On success, the `cron_job_t *` is invalid immediately.
- `void cron_execute_due(cron_ctx_t *ctx, const struct timespec *now)`
  - Executes callbacks due at `now` (UTC).
  - Invalid `tv_nsec` values outside `[0, 999999999]` are ignored.
- `void cron_tick(cron_ctx_t *ctx)`
  - Convenience wrapper using current UTC time via `timespec_get`.
- `bool cron_set_timezone_offset_minutes(cron_ctx_t *ctx, int32_t utc_offset_minutes)`
  - Sets fixed timezone offset in minutes (default `0` for UTC, range `[-1440, 1440]`).
- `int32_t cron_get_timezone_offset_minutes(const cron_ctx_t *ctx)`
  - Gets fixed timezone offset in minutes (returns `0` for `nullptr`).
- `bool cron_execute_between(cron_ctx_t *ctx, const struct timespec *after, const struct timespec *until)`
  - Executes all due instants in (`after`, `until`] (UTC), useful for catch-up after sleep or scheduling delays.
  - Invalid `tv_nsec` values cause the call to fail closed.
- `bool cron_get_next_trigger(const cron_ctx_t *ctx, const struct timespec *after, struct timespec *next_out)`
  - Finds the next trigger strictly after `after` (search horizon: 366 days).
  - Returns `false` when the next match falls outside that bound.

## Timezone Offset From IANA Zone

`nanocron` stores timezone as a fixed offset in minutes. If your app uses an
IANA zone name (for example `Europe/Kyiv`), compute the offset for a specific
instant, then pass it to `cron_set_timezone_offset_minutes`.

`Europe/Kyiv` is typically:

- `+120` minutes in winter (`UTC+02:00`)
- `+180` minutes in summer (`UTC+03:00`)

Use runtime conversion so DST changes are handled correctly:

```c
#include <ctype.h>
#include <stdint.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

[[nodiscard]]
static char *dup_c_string(const char *src) {
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

[[nodiscard]]
static bool parse_hhmm_offset_to_minutes(const char *hhmm,
                                         int32_t *out_minutes) {
  if (hhmm == nullptr || out_minutes == nullptr) {
    return false;
  }
  if ((hhmm[0] != '+' && hhmm[0] != '-') || !isdigit((unsigned char)hhmm[1]) ||
      !isdigit((unsigned char)hhmm[2]) || !isdigit((unsigned char)hhmm[3]) ||
      !isdigit((unsigned char)hhmm[4]) || hhmm[5] != '\0') {
    return false;
  }

  const int32_t sign = (hhmm[0] == '-') ? -1 : 1;
  const int32_t hours = (hhmm[1] - '0') * 10 + (hhmm[2] - '0');
  const int32_t mins = (hhmm[3] - '0') * 10 + (hhmm[4] - '0');
  *out_minutes = sign * (hours * 60 + mins);
  return true;
}

[[nodiscard]]
static bool zone_offset_minutes_at(const char *iana_zone, time_t at_utc,
                                   int32_t *out_minutes) {
  if (iana_zone == nullptr || out_minutes == nullptr) {
    return false;
  }

  const char *old_tz = getenv("TZ");
  char *saved_tz = (old_tz != nullptr) ? dup_c_string(old_tz) : nullptr;
  if (old_tz != nullptr && saved_tz == nullptr) {
    return false;
  }

  bool ok = false;
  if (setenv("TZ", iana_zone, 1) == 0) {
    tzset();
    struct tm local_tm;
    if (localtime_r(&at_utc, &local_tm) != nullptr) {
      char hhmm[6] = {0};
      if (strftime(hhmm, sizeof(hhmm), "%z", &local_tm) > 0) {
        ok = parse_hhmm_offset_to_minutes(hhmm, out_minutes);
      }
    }
  }

  if (saved_tz != nullptr) {
    (void)setenv("TZ", saved_tz, 1);
  } else {
    (void)unsetenv("TZ");
  }
  tzset();
  free(saved_tz);
  return ok;
}

/* Usage */
void configure_timezone(cron_ctx_t *ctx) {
  struct timespec now;
  if (timespec_get(&now, TIME_UTC) == 0) {
    return;
  }

  int32_t offset_minutes = 0;
  if (zone_offset_minutes_at("Europe/Kyiv", now.tv_sec, &offset_minutes)) {
    (void)cron_set_timezone_offset_minutes(ctx, offset_minutes);
  }
}
```

Notes:

- Recalculate offset when DST may have changed (for example hourly, daily, or
  before computing long horizons).
- `TZ`/`tzset` are process-global; guard this path if your process is
  multithreaded.

## Build And Test

Using `just`:

```bash
just all           # build examples + tests
just test          # build and run tests
just test-debug    # build with sanitizers and run tests
just examples-build
just examples-debug
just clean
```

Using Zig build:

```bash
zig build          # default: all
zig build examples
zig build tests
zig build test     # build and run tests
```

The project builds with:

- `-std=c23`
- `-Wall -Wextra -Wpedantic -Werror`
- hardening: `-fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE`
- linker hardening: `-Wl,-z,relro,-z,now -pie`

## Notes

- Time handling is UTC-based (`gmtime_r`, `timespec_get`).
- Schedule matching uses a configurable fixed timezone offset per context.
- Callback execution is synchronous in the calling thread.
- The scheduler context is not thread-safe; synchronize externally if shared.
- Deferred destruction from inside callbacks still invalidates the context as soon as the outermost execution call returns.
- For deterministic catch-up, compute absolute wake instants and call `cron_execute_between`.

## License

MIT. See `LICENSE`.
