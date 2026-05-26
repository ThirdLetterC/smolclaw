// *************************************************************************
//
// microlog extension: FreeRTOS lock helper
//
// Usage:
//    #define ULOG_LOCK_WITH_FREERTOS
//    #include "ulog_lock_freertos.h"
//    ...
//    SemaphoreHandle_t m = xSemaphoreCreateMutex();
//    ulog_lock_freertos_enable(m);
//    ulog_info("FreeRTOS lock active");
//
// *************************************************************************
#pragma once
#include "FreeRTOS.h"
#include "semphr.h"
#include "ulog/ulog.h"


/**
 * @brief Enable locking using an existing FreeRTOS mutex.
 * @param mutex Valid recursive or normal mutex handle.
 * @return ULOG_STATUS_OK or ULOG_STATUS_INVALID_ARGUMENT.
 */
[[nodiscard]] ulog_status ulog_lock_freertos_enable(SemaphoreHandle_t mutex);

/**
 * @brief Disable logging lock (clears lock function). Keeps mutex allocated.
 */
[[nodiscard]] ulog_status ulog_lock_freertos_disable();
