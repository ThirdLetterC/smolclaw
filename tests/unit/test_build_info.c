#include "core/build_info.h"

#include <stdio.h>
#include <string.h>

#include "sc/version.h"
static int expect_int(const char *label, int actual, int expected)
{
    if (actual != expected) {
        (void)fprintf(stderr, "%s: expected %d, got %d\n", label, expected, actual);
        return 1;
    }

    return 0;
}

int main(void)
{
    int failures = 0;

    if (strcmp(sc_build_version(), SC_VERSION) != 0) {
        (void)fprintf(stderr, "version mismatch\n");
        failures += 1;
    }

    failures += expect_int(
        "abi",
        (int)sc_build_abi_version(),
        (int)SC_ABI_VERSION
    );
    failures += expect_int(
        "runtime",
        sc_build_feature_enabled("SC_ENABLE_RUNTIME"),
        SC_ENABLE_RUNTIME
    );
    failures += expect_int(
        "gateway",
        sc_build_feature_enabled("SC_ENABLE_GATEWAY"),
        SC_ENABLE_GATEWAY
    );
    failures += expect_int(
        "mimalloc",
        sc_build_feature_enabled("SC_ENABLE_MIMALLOC"),
        SC_ENABLE_MIMALLOC
    );
    failures += expect_int(
        "jemalloc",
        sc_build_feature_enabled("SC_ENABLE_JEMALLOC"),
        SC_ENABLE_JEMALLOC
    );
    failures += expect_int(
        "unknown",
        sc_build_feature_enabled("SC_UNKNOWN_FEATURE"),
        0
    );
    failures += expect_int(
        "null",
        sc_build_feature_enabled(nullptr),
        0
    );

    return failures == 0 ? 0 : 1;
}
