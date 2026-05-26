// *************************************************************************
//
// microlog extension: CMSIS-RTOS2 mutex lock helper
//
// Usage:
//    #include "ulog_lock_cmsis.h"
//    ...
//    osMutexId_t mutex_id = osMutexNew(nullptr);
//    ulog_lock_cmsis_enable(mutex_id);
//    ulog_info("CMSIS lock active");
//
// *************************************************************************
#pragma once
#include "cmsis_os2.h"
#include "ulog/ulog.h"


/**
 * @brief Enable locking with an existing CMSIS-RTOS2 mutex id.
 * @param mutex_id Handle returned by osMutexNew or equivalent.
 */
[[nodiscard]] ulog_status ulog_lock_cmsis_enable(osMutexId_t mutex_id);

/**
 * @brief Disable logging lock (clears lock function). Keeps mutex.
 */
[[nodiscard]] ulog_status ulog_lock_cmsis_disable();
