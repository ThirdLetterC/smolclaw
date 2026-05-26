#include "sc/result.h"
#include "sc/log.h"

#include <string.h>

static sc_status status_with_key(sc_status_code code, const char *error_key);

sc_status sc_status_ok(void)
{
    return (sc_status){
        .code = SC_OK,
        .error_key = nullptr,
        .message = nullptr,
        .alloc = nullptr,
    };
}

sc_status sc_status_make(sc_status_code code, const char *error_key, const char *message)
{
    return (sc_status){
        .code = code,
        .error_key = error_key,
        .message = (char *)message,
        .alloc = nullptr,
    };
}

sc_status sc_status_make_owned(sc_allocator *alloc,
                               sc_status_code code,
                               const char *error_key,
                               const char *message)
{
    size_t len = 0;
    char *copy = nullptr;

    if (message == nullptr) {
        return sc_status_make(code, error_key, nullptr);
    }
    if (alloc == nullptr) {
        alloc = sc_allocator_heap();
    }

    len = strlen(message);
    copy = sc_alloc(alloc, len + 1, _Alignof(char));
    if (copy == nullptr) {
        return sc_status_no_memory();
    }

    (void)memcpy(copy, message, len + 1);
    return (sc_status){
        .code = code,
        .error_key = error_key,
        .message = copy,
        .alloc = alloc,
    };
}

bool sc_status_is_ok(sc_status status)
{
    return status.code == SC_OK;
}

void sc_status_clear(sc_status *status)
{
    if (status == nullptr) {
        return;
    }

    if (status->alloc != nullptr && status->message != nullptr) {
        sc_free(status->alloc, status->message, strlen(status->message) + 1, _Alignof(char));
    }

    *status = sc_status_ok();
}

sc_status sc_status_invalid_argument(const char *error_key)
{
    return status_with_key(SC_ERR_INVALID_ARGUMENT, error_key);
}

sc_status sc_status_no_memory(void)
{
    sc_log_write(SC_LOG_FATAL, "sc.core", "core.no_memory", nullptr, 0);
    return status_with_key(SC_ERR_NO_MEMORY, "sc.core.no_memory");
}

sc_status sc_status_io(const char *error_key)
{
    return status_with_key(SC_ERR_IO, error_key);
}

sc_status sc_status_json(const char *error_key)
{
    return status_with_key(SC_ERR_PARSE, error_key);
}

sc_status sc_status_parse(const char *error_key)
{
    return status_with_key(SC_ERR_PARSE, error_key);
}

sc_status sc_status_http(const char *error_key)
{
    return status_with_key(SC_ERR_HTTP, error_key);
}

sc_status sc_status_security_denied(const char *error_key)
{
    return status_with_key(SC_ERR_SECURITY_DENIED, error_key);
}

sc_status sc_status_unsupported(const char *error_key)
{
    return status_with_key(SC_ERR_UNSUPPORTED, error_key);
}

sc_status sc_status_timeout(const char *error_key)
{
    return status_with_key(SC_ERR_TIMEOUT, error_key);
}

sc_status sc_status_cancelled(const char *error_key)
{
    return status_with_key(SC_ERR_CANCELLED, error_key);
}

static sc_status status_with_key(sc_status_code code, const char *error_key)
{
    return sc_status_make(code, error_key, nullptr);
}
