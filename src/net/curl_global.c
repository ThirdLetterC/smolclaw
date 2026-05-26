#include "net/curl_global.h"

#include <stdlib.h>

#ifdef SC_HAVE_LIBCURL
#include <curl/curl.h>
#endif

static void sc_curl_global_cleanup(void);

sc_status sc_curl_global_init(const char *error_key)
{
#ifdef SC_HAVE_LIBCURL
    static bool initialized = false;

    if (initialized) {
        return sc_status_ok();
    }
    if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
        return sc_status_io(error_key);
    }
    if (atexit(sc_curl_global_cleanup) != 0) {
        curl_global_cleanup();
        return sc_status_io(error_key);
    }
    initialized = true;
    return sc_status_ok();
#else
    (void)error_key;
    return sc_status_unsupported("sc.curl.unavailable");
#endif
}

static void sc_curl_global_cleanup(void)
{
#ifdef SC_HAVE_LIBCURL
    curl_global_cleanup();
#endif
}
