#include "security/sandbox_exec_internal.h"

#include "sc/api.h"

static sc_status bubblewrap_check(void *impl,
                                  const sc_sandbox_request *request,
                                  sc_allocator *alloc,
                                  sc_sandbox_decision *out);
static void bubblewrap_destroy(void *impl);

static const sc_sandbox_vtab bubblewrap_vtab = {
    .struct_size = sizeof(sc_sandbox_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "bubblewrap-sandbox",
    .display_name = "Linux Bubblewrap sandbox",
    .feature_flag = "SC_SANDBOX_BUBBLEWRAP",
    .capabilities = SC_CONTRACT_CAP_SECURE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .check = bubblewrap_check,
    .destroy = bubblewrap_destroy,
};

sc_status sc_sandbox_bubblewrap_new(sc_allocator *alloc, sc_sandbox **out)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.invalid_argument");
    }
    *out = nullptr;
    if (!sc_sandbox_executable_available("/usr/bin/bwrap")) {
        return sc_status_unsupported("sc.sandbox.bubblewrap.unavailable");
    }
    return sc_sandbox_new(alloc, &bubblewrap_vtab, nullptr, out);
}

static sc_status bubblewrap_check(void *impl,
                                  const sc_sandbox_request *request,
                                  sc_allocator *alloc,
                                  sc_sandbox_decision *out)
{
    static const char *readonly_paths[] = {"/usr", "/bin", "/lib", "/lib64", "/etc"};
    sc_status status;

    (void)impl;
    if (request == nullptr || out == nullptr || request->executable.ptr == nullptr || request->executable.len == 0) {
        return sc_status_invalid_argument("sc.sandbox.bubblewrap.invalid_argument");
    }
    status = sc_sandbox_decision_begin(out, alloc, SC_SANDBOX_BACKEND_BUBBLEWRAP, "bubblewrap sandbox plan");
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "/usr/bin/bwrap");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--die-with-parent");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--new-session");
    }
    if (sc_status_is_ok(status) && request->network == SC_SANDBOX_NETWORK_NONE) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--unshare-net");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--proc");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "/proc");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--dev");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "/dev");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--tmpfs");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "/tmp");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(readonly_paths); i += 1) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--ro-bind-try");
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_cstr(out, alloc, readonly_paths[i]);
        }
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_cstr(out, alloc, readonly_paths[i]);
        }
    }
    if (sc_status_is_ok(status) && request->cwd.ptr != nullptr && request->cwd.len > 0) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--bind");
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_arg(out, alloc, request->cwd);
        }
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_arg(out, alloc, request->cwd);
        }
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_cstr(out, alloc, "--chdir");
        }
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_arg(out, alloc, request->cwd);
        }
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < request->allowed_device_count; i += 1) {
        if (request->allowed_devices[i].ptr == nullptr ||
            request->allowed_devices[i].len == 0 ||
            sc_str_equal(request->allowed_devices[i], sc_str_from_cstr("*"))) {
            continue;
        }
        status = sc_sandbox_decision_add_cstr(out, alloc, "--dev-bind");
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_arg(out, alloc, request->allowed_devices[i]);
        }
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_arg(out, alloc, request->allowed_devices[i]);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_original_command(out, alloc, request);
    }
    if (!sc_status_is_ok(status)) {
        sc_sandbox_decision_clear(out);
    }
    return status;
}

static void bubblewrap_destroy(void *impl)
{
    (void)impl;
}
