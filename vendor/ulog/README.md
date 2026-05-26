# ulog

A small, portable, and configurable logging library for C23. It favors minimal overhead, explicit feature toggles, and straightforward integration in embedded or desktop codebases.

**Key Features**
- C23-first API and implementation
- Single translation unit core (`src/ulog.c`) + public header (`include/ulog/ulog.h`)
- Compile-time feature selection with optional runtime toggles
- Topics, custom outputs, and custom prefixes
- Optional timestamps, source locations, and colorized output
- Thread-safety via user-provided lock callback
- Extensions for syslog levels, RTOS locks, and compatibility shims

**Quick Start**

1. Add `include/ulog/ulog.h` and `src/ulog.c` to your build.
2. Compile with C23 enabled.

```sh
cc -std=c23 -Iinclude src/ulog.c main.c -o app
```

For hardened local testing, the `justfile` also exposes
`just cc-debug-example` and `just cc-debug-all-features`, which compile with
`-fsanitize=address,undefined,leak`.

The Zig build now supports the same sanitizer set through
`zig build -Dsanitizers=true`.

```c
#include "ulog/ulog.h"

int main() {
    ulog_info("boot %d", 1);
    ulog_warn("low battery: %u%%", 12U);
    return 0;
}
```

**Core API**

- Logging macros: `ulog_trace`, `ulog_debug`, `ulog_info`, `ulog_warn`, `ulog_error`, `ulog_fatal`, or generic `ulog(LEVEL, ...)`.
- Topics: `ulog_topic_add`, `ulog_topic_remove`, `ulog_topic_level_set`, plus `ulog_t_*` macros.
- Outputs: `ulog_output_add`, `ulog_output_add_file`, `ulog_output_remove`, `ulog_output_level_set`.
- Prefix: `ulog_prefix_set_fn` with `ULOG_BUILD_PREFIX_SIZE` or `ULOG_BUILD_DYNAMIC_CONFIG`.
- Lock: `ulog_lock_set_fn` for thread safety.

**Build Configuration**

Define `ULOG_BUILD_*` macros before including `ulog/ulog.h` or via compiler flags. Defaults are shown below.
If you set `ULOG_BUILD_CONFIG_HEADER_ENABLED`, define all `ULOG_BUILD_*` values in the header named by
`ULOG_BUILD_CONFIG_HEADER_NAME` and do not define them elsewhere.

| Build Option                     | Default                    | Purpose                              |
| -------------------------------- | -------------------------- | ------------------------------------ |
| `ULOG_BUILD_COLOR`               | `0`                        | Compile color output paths           |
| `ULOG_BUILD_PREFIX_SIZE`         | `0`                        | Enable prefix buffer and callback    |
| `ULOG_BUILD_EXTRA_OUTPUTS`       | `0`                        | Enable custom output backends        |
| `ULOG_BUILD_SOURCE_LOCATION`     | `1`                        | Enable `file:line` output            |
| `ULOG_BUILD_LEVEL_SHORT`         | `0`                        | Short level names (e.g. `D`)         |
| `ULOG_BUILD_TIME`                | `0`                        | Timestamp support                    |
| `ULOG_BUILD_TOPICS_MODE`         | `ULOG_BUILD_TOPICS_MODE_OFF` | Topics support                       |
| `ULOG_BUILD_TOPICS_STATIC_NUM`   | `0`                        | Static topics capacity               |
| `ULOG_BUILD_DYNAMIC_CONFIG`      | `0`                        | Enable runtime config toggles        |
| `ULOG_BUILD_WARN_NOT_ENABLED`    | `1`                        | Warn when calling disabled features  |
| `ULOG_BUILD_CONFIG_HEADER_ENABLED` | `0`                      | Read config from header              |
| `ULOG_BUILD_CONFIG_HEADER_NAME`  | `"ulog_config.h"`          | Configuration header name            |
| `ULOG_BUILD_DISABLED`            | `0`                        | Compile all logging as no-ops        |

**Topics Mode**

Use `ULOG_BUILD_TOPICS_MODE` with one of the following values:
- `ULOG_BUILD_TOPICS_MODE_OFF` disables topics.
- `ULOG_BUILD_TOPICS_MODE_STATIC` enables a fixed-size topic table.
- `ULOG_BUILD_TOPICS_MODE_DYNAMIC` enables dynamically allocated topics.

Example:

```c
#define ULOG_BUILD_TOPICS_MODE ULOG_BUILD_TOPICS_MODE_DYNAMIC
#include "ulog/ulog.h"

void net_init() {
    ulog_topic_add("net", ULOG_OUTPUT_ALL, ULOG_LEVEL_INFO);
    ulog_t_info("net", "link up");
}
```

**Runtime Configuration (Optional)**

Set `ULOG_BUILD_DYNAMIC_CONFIG=1` to enable runtime toggles:
- `ulog_color_config`, `ulog_prefix_config`, `ulog_source_location_config`, `ulog_time_config`
- `ulog_level_config` (short or default level names)
- `ulog_topic_config` (enable/disable topics at runtime)

Pass `nullptr` to `ulog_prefix_set_fn` to disable a previously installed
prefix callback.

When dynamic configuration is enabled, the build forces a set of defaults internally:
`ULOG_BUILD_EXTRA_OUTPUTS=8`, `ULOG_BUILD_PREFIX_SIZE=64`, and `ULOG_BUILD_TOPICS_MODE=ULOG_BUILD_TOPICS_MODE_DYNAMIC`,
with color, time, source location, and topics enabled.

**Custom Output Example**

```c
static void my_output(ulog_event *ev, void *arg) {
    (void)arg;
    char line[256];
    if (ulog_event_to_cstr(ev, line, sizeof(line)) == ULOG_STATUS_OK) {
        fputs(line, stdout);
        fputc('\n', stdout);
    }
}

void install_output() {
    ulog_output_add(my_output, nullptr, ULOG_LEVEL_DEBUG);
}
```

When `ULOG_BUILD_TIME=1`, `ulog_event_get_time` returns a per-event snapshot,
so output callbacks can read timestamps without relying on `localtime`'s shared
static storage.

**Thread Safety**

You can register a lock function with `ulog_lock_set_fn`. When installed, the
same lock is used for configuration changes, topic/output table updates, and
log dispatch. For convenience, platform helpers live in `extensions/`. Example
with pthreads:

```c
#include "ulog_lock_pthread.h"

void enable_lock(pthread_mutex_t *mtx) {
    ulog_lock_pthread_enable(mtx);
}
```

**Extensions**

Optional extensions live under `extensions/`. Highlights include:
- Syslog level descriptors: `extensions/ulog_syslog.h`
- RTOS and platform locks: `ulog_lock_cmsis.h`, `ulog_lock_freertos.h`, `ulog_lock_pthread.h`, `ulog_lock_threadx.h`, `ulog_lock_win.h`
- Compatibility layer: `extensions/ulog_microlog6.h`
- Generic logger shim: `extensions/ulog_generic_interface.h`

See `extensions/README.md` for details.

**License**

MIT. See `LICENSE`.
