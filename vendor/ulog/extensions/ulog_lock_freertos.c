// *************************************************************************
//
// microlog extension: FreeRTOS mutex lock helper (implementation)
// *************************************************************************

#include "ulog_lock_freertos.h"
#include "FreeRTOS.h"
#include "semphr.h"

/** @brief Internal FreeRTOS mutex adapter. */
static ulog_status freertos_lock_fn(bool lock, void *arg) {
    auto m = (SemaphoreHandle_t)arg;
    if (m == nullptr) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }
    auto ok = pdFALSE;
    if (lock) {
        ok = xSemaphoreTake(m, portMAX_DELAY);
    } else {
        ok = xSemaphoreGive(m);
    }
    if (ok != pdTRUE) {
        return ULOG_STATUS_ERROR;
    }
    return ULOG_STATUS_OK;
}

/** @copydoc ulog_lock_freertos_enable */
ulog_status ulog_lock_freertos_enable(SemaphoreHandle_t mutex) {
    if (mutex == nullptr) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }
    return ulog_lock_set_fn(freertos_lock_fn, (void *)mutex);
}

/** @copydoc ulog_lock_freertos_disable */
ulog_status ulog_lock_freertos_disable() {
    return ulog_lock_set_fn(nullptr, nullptr);
}
