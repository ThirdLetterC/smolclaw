#include <errno.h>
#include <stdio.h>
#include <time.h>

#include "nanocron/nanocron.h"

void my_cb([[maybe_unused]] void *ud,
           [[maybe_unused]] const struct timespec *ts) {
  printf("CRON fired\n");
}

int main() {
  cron_ctx_t *ctx = cron_create();
  if (ctx == nullptr) {
    return 1;
  }

  if (cron_add(ctx, "* */5 * * * * *", my_cb, nullptr) == nullptr) {
    cron_destroy(ctx);
    return 1;
  }

  while (true) {
    cron_tick(ctx);
    /* Tick once per second to avoid repeated firings within a matching second.
     */
    struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
    while (nanosleep(&ts, &ts) == -1) {
      if (errno != EINTR) {
        cron_destroy(ctx);
        return 1;
      }
    }
  }

  cron_destroy(ctx);
  return 0;
}
