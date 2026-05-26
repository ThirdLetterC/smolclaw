#include <stdint.h>

#include "ulog/ulog.h"

int main() {
    constexpr unsigned boot_id     = 1U;
    constexpr unsigned battery_pct = 12U;

    ulog_info("boot %u", boot_id);
    ulog_warn("low battery: %u%%", battery_pct);

    auto retries = 3U;
    ulog_info("retries: %u", retries);

    auto reason = (const char *)nullptr;
    if (reason == nullptr) {
        ulog_info("no shutdown reason");
    }

    return 0;
}
