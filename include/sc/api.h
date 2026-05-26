#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdckdint.h>

#include "sc/version.h"

#define SC_EXPORT __attribute__((visibility("default")))
#define SC_IMPORT

#if defined(SC_BUILDING_SHARED)
#define SC_API SC_EXPORT
#else
#define SC_API
#endif

#define SC_ARRAY_LEN(array) (sizeof(array) / sizeof((array)[0]))

#if defined(__cplusplus)
#define SC_BEGIN_DECLS extern "C" {
#define SC_END_DECLS }
#else
#define SC_BEGIN_DECLS
#define SC_END_DECLS
#endif

#if defined(__STDC_VERSION__) && __STDC_VERSION__ >= 202311L
#define SC_STATIC_ASSERT static_assert
#else
#define SC_STATIC_ASSERT _Static_assert
#endif

static inline bool sc_size_add_overflow(size_t a, size_t b, size_t *out)
{
    if (out == nullptr) {
        return true;
    }
    return ckd_add(out, a, b);
}

static inline bool sc_size_mul_overflow(size_t a, size_t b, size_t *out)
{
    if (out == nullptr) {
        return true;
    }
    return ckd_mul(out, a, b);
}
