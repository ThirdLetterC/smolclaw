#include "security/security_internal.h"

#include <stdio.h>
#include <string.h>

void sc_estop_init(sc_estop_state *estop, sc_allocator *alloc)
{
    if (estop == nullptr) {
        return;
    }
    *estop = (sc_estop_state){.alloc = alloc == nullptr ? sc_allocator_heap() : alloc};
}

sc_status sc_estop_trip(sc_estop_state *estop, sc_str reason)
{
    if (estop == nullptr) {
        return sc_status_invalid_argument("sc.estop.invalid_argument");
    }
    if (estop->alloc == nullptr) {
        estop->alloc = sc_allocator_heap();
    }
    sc_string_clear(&estop->reason);
    estop->active = true;
    return sc_string_from_str(estop->alloc, reason, &estop->reason);
}

void sc_estop_reset(sc_estop_state *estop)
{
    if (estop == nullptr) {
        return;
    }
    estop->active = false;
    sc_string_clear(&estop->reason);
}

sc_status sc_estop_check(const sc_estop_state *estop)
{
    if (estop != nullptr && estop->active) {
        return sc_status_security_denied("sc.estop.active");
    }
    return sc_status_ok();
}

sc_status sc_estop_write_file(const sc_estop_state *estop, sc_str path)
{
    char path_buf[4096] = {0};
    FILE *file = nullptr;
    sc_status status = sc_status_ok();

    if (estop == nullptr || path.len == 0 || path.ptr == nullptr || path.len >= sizeof(path_buf)) {
        return sc_status_invalid_argument("sc.estop.invalid_argument");
    }
    (void)memcpy(path_buf, path.ptr, path.len);
    file = fopen(path_buf, "wb");
    if (file == nullptr) {
        status = sc_status_io("sc.estop.write_open_failed");
        goto cleanup;
    }
    if (fprintf(file,
                "active=%s\nreason=%s\n",
                estop->active ? "true" : "false",
                estop->reason.ptr == nullptr ? "" : estop->reason.ptr) < 0) {
        status = sc_status_io("sc.estop.write_failed");
        goto cleanup;
    }
    if (fclose(file) != 0) {
        file = nullptr;
        status = sc_status_io("sc.estop.write_close_failed");
        goto cleanup;
    }
    file = nullptr;

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    return status;
}

sc_status sc_estop_read_file(sc_allocator *alloc, sc_str path, sc_estop_state *out)
{
    char path_buf[4096] = {0};
    char line[512] = {0};
    FILE *file = nullptr;
    sc_status status = sc_status_ok();

    if (out == nullptr || path.len == 0 || path.ptr == nullptr || path.len >= sizeof(path_buf)) {
        return sc_status_invalid_argument("sc.estop.invalid_argument");
    }
    (void)memcpy(path_buf, path.ptr, path.len);
    sc_estop_init(out, alloc);
    file = fopen(path_buf, "rb");
    if (file == nullptr) {
        status = sc_status_io("sc.estop.read_open_failed");
        goto cleanup;
    }
    while (fgets(line, sizeof(line), file) != nullptr) {
        if (strncmp(line, "active=true", strlen("active=true")) == 0) {
            out->active = true;
        } else if (strncmp(line, "reason=", strlen("reason=")) == 0) {
            size_t len = strlen(line);
            if (len > 0 && line[len - 1] == '\n') {
                line[len - 1] = '\0';
            }
            status = sc_string_from_cstr(out->alloc, &line[strlen("reason=")], &out->reason);
            if (!sc_status_is_ok(status)) {
                break;
            }
        }
    }
    if (fclose(file) != 0 && sc_status_is_ok(status)) {
        file = nullptr;
        status = sc_status_io("sc.estop.read_close_failed");
    } else {
        file = nullptr;
    }
    if (!sc_status_is_ok(status)) {
        sc_estop_clear(out);
    }

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    return status;
}

void sc_estop_clear(sc_estop_state *estop)
{
    if (estop == nullptr) {
        return;
    }
    sc_string_clear(&estop->reason);
    *estop = (sc_estop_state){0};
}
