#pragma once

#include "sc/allocator.h"

typedef enum sc_status_code {
    SC_OK = 0,
    SC_ERR_INVALID_ARGUMENT,
    SC_ERR_NO_MEMORY,
    SC_ERR_IO,
    SC_ERR_PARSE,
    SC_ERR_HTTP,
    SC_ERR_SECURITY_DENIED,
    SC_ERR_UNSUPPORTED,
    SC_ERR_TIMEOUT,
    SC_ERR_CANCELLED
} sc_status_code;

typedef struct [[nodiscard]] sc_status {
    sc_status_code code;
    const char *error_key;
    char *message;
    sc_allocator *alloc;
} sc_status;

sc_status sc_status_ok(void);
sc_status sc_status_make(sc_status_code code, const char *error_key, const char *message);
sc_status sc_status_make_owned(sc_allocator *alloc,
                               sc_status_code code,
                               const char *error_key,
                               const char *message);
bool sc_status_is_ok(sc_status status);
void sc_status_clear(sc_status *status);

sc_status sc_status_invalid_argument(const char *error_key);
sc_status sc_status_no_memory(void);
sc_status sc_status_io(const char *error_key);
sc_status sc_status_json(const char *error_key);
sc_status sc_status_parse(const char *error_key);
sc_status sc_status_http(const char *error_key);
sc_status sc_status_security_denied(const char *error_key);
sc_status sc_status_unsupported(const char *error_key);
sc_status sc_status_timeout(const char *error_key);
sc_status sc_status_cancelled(const char *error_key);
