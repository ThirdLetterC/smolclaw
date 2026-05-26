// *************************************************************************
//
// microlog extension: pthread mutex lock helper (implementation)
//
// This file provides convenience functions to enable microlog locking using a
// POSIX pthread mutex. The implementation mirrors coding style used in
// `ulog.c`: explicit braces, early returns, and clear variable naming.
//
// *************************************************************************

#include "ulog_lock_pthread.h"

/**
 * @brief Internal adapter; wraps pthread mutex operations in ulog_lock_fn
 * signature.
 */
static ulog_status pthread_lock_fn(bool lock, void *arg) {
    if (arg == nullptr) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }

    auto mtx = (pthread_mutex_t *)arg;
    auto rc   = -1;

    if (lock) {
        rc = pthread_mutex_lock(mtx);
    } else {
        rc = pthread_mutex_unlock(mtx);
    }

    if (rc != 0) {
        return ULOG_STATUS_ERROR;
    }
    return ULOG_STATUS_OK;
}

/**
 * @copydoc ulog_lock_pthread_enable
 */
ulog_status ulog_lock_pthread_enable(pthread_mutex_t *mtx) {
    if (mtx == nullptr) {
        return ULOG_STATUS_INVALID_ARGUMENT;
    }

    return ulog_lock_set_fn(pthread_lock_fn, mtx);
}

/** @copydoc ulog_lock_pthread_disable */
ulog_status ulog_lock_pthread_disable() {
    return ulog_lock_set_fn(nullptr, nullptr);
}
