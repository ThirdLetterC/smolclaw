#include "security/sandbox_exec_internal.h"

#include "sc/api.h"

static sc_status firejail_check(void *impl,
                                const sc_sandbox_request *request,
                                sc_allocator *alloc,
                                sc_sandbox_decision *out);
static void firejail_destroy(void *impl);

static const sc_sandbox_vtab firejail_vtab = {
    .struct_size = sizeof(sc_sandbox_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "firejail-sandbox",
    .display_name = "Linux Firejail sandbox",
    .feature_flag = "SC_SANDBOX_FIREJAIL",
    .capabilities = SC_CONTRACT_CAP_SECURE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .check = firejail_check,
    .destroy = firejail_destroy,
};

sc_status sc_sandbox_firejail_new(sc_allocator *alloc, sc_sandbox **out)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.invalid_argument");
    }
    *out = nullptr;
    if (!sc_sandbox_executable_available("/usr/bin/firejail")) {
        return sc_status_unsupported("sc.sandbox.firejail.unavailable");
    }
    return sc_sandbox_new(alloc, &firejail_vtab, nullptr, out);
}

static sc_status firejail_check(void *impl,
                                const sc_sandbox_request *request,
                                sc_allocator *alloc,
                                sc_sandbox_decision *out)
{
    static const char *flags[] = {"--quiet", "--noprofile", "--private-tmp", "--caps.drop=all", "--nonewprivs", "--noroot"};
    sc_status status;

    (void)impl;
    if (request == nullptr || out == nullptr || request->executable.ptr == nullptr || request->executable.len == 0) {
        return sc_status_invalid_argument("sc.sandbox.firejail.invalid_argument");
    }
    status = sc_sandbox_decision_begin(out, alloc, SC_SANDBOX_BACKEND_FIREJAIL, "firejail sandbox plan");
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "/usr/bin/firejail");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < SC_ARRAY_LEN(flags); i += 1) {
        status = sc_sandbox_decision_add_cstr(out, alloc, flags[i]);
    }
    if (sc_status_is_ok(status) && request->network == SC_SANDBOX_NETWORK_NONE) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--net=none");
    }
    if (sc_status_is_ok(status) && request->cwd.ptr != nullptr && request->cwd.len > 0) {
        status = sc_sandbox_decision_add_joined(out, alloc, "--private=", request->cwd);
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_original_command(out, alloc, request);
    }
    if (!sc_status_is_ok(status)) {
        sc_sandbox_decision_clear(out);
    }
    return status;
}

static void firejail_destroy(void *impl)
{
    (void)impl;
}
