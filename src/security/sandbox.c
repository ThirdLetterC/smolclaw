#include "security/sandbox_exec_internal.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

static sc_status noop_check(void *impl,
                            const sc_sandbox_request *request,
                            sc_allocator *alloc,
                            sc_sandbox_decision *out);
static void noop_destroy(void *impl);

static const sc_sandbox_vtab noop_vtab = {
    .struct_size = sizeof(sc_sandbox_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "noop-sandbox",
    .display_name = "No-op sandbox",
    .feature_flag = "SC_SANDBOX_NOOP",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .check = noop_check,
    .destroy = noop_destroy,
};

sc_status sc_sandbox_noop_new(sc_allocator *alloc, sc_sandbox **out)
{
    return sc_sandbox_new(alloc, &noop_vtab, nullptr, out);
}

sc_status sc_sandbox_seatbelt_new(sc_allocator *alloc, sc_sandbox **out)
{
    (void)alloc;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.invalid_argument");
    }
    *out = nullptr;
    return sc_status_unsupported("sc.sandbox.seatbelt.unsupported");
}

sc_status sc_sandbox_appcontainer_new(sc_allocator *alloc, sc_sandbox **out)
{
    (void)alloc;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.invalid_argument");
    }
    *out = nullptr;
    return sc_status_unsupported("sc.sandbox.appcontainer.unsupported");
}

sc_status sc_sandbox_select_baseline(sc_allocator *alloc, sc_sandbox **out, sc_audit_chain *audit)
{
    return sc_sandbox_select_backend(alloc, SC_SANDBOX_BACKEND_AUTO, out, audit);
}

sc_status sc_sandbox_select_backend(sc_allocator *alloc,
                                    sc_sandbox_backend backend,
                                    sc_sandbox **out,
                                    sc_audit_chain *audit)
{
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.invalid_argument");
    }
    *out = nullptr;

    if (backend == SC_SANDBOX_BACKEND_UNKNOWN) {
        return sc_status_invalid_argument("sc.sandbox.backend_unknown");
    }
    if (backend == SC_SANDBOX_BACKEND_NOOP) {
        status = sc_sandbox_noop_new(alloc, out);
        if (sc_status_is_ok(status) && audit != nullptr) {
            status = sc_audit_chain_append(audit,
                                           sc_str_from_cstr("sc.security.sandbox_selected"),
                                           sc_str_from_cstr("using noop sandbox"));
        }
        return status;
    }
    if (backend == SC_SANDBOX_BACKEND_LANDLOCK) {
        status = sc_sandbox_landlock_new(alloc, out);
        if (sc_status_is_ok(status) && audit != nullptr) {
            status = sc_audit_chain_append(audit,
                                           sc_str_from_cstr("sc.security.sandbox_selected"),
                                           sc_str_from_cstr("using Linux Landlock sandbox"));
        }
        return status;
    }
    if (backend == SC_SANDBOX_BACKEND_BUBBLEWRAP) {
        status = sc_sandbox_bubblewrap_new(alloc, out);
        if (sc_status_is_ok(status) && audit != nullptr) {
            status = sc_audit_chain_append(audit,
                                           sc_str_from_cstr("sc.security.sandbox_selected"),
                                           sc_str_from_cstr("using Linux Bubblewrap sandbox"));
        }
        return status;
    }
    if (backend == SC_SANDBOX_BACKEND_FIREJAIL) {
        status = sc_sandbox_firejail_new(alloc, out);
        if (sc_status_is_ok(status) && audit != nullptr) {
            status = sc_audit_chain_append(audit,
                                           sc_str_from_cstr("sc.security.sandbox_selected"),
                                           sc_str_from_cstr("using Linux Firejail sandbox"));
        }
        return status;
    }
    if (backend == SC_SANDBOX_BACKEND_DOCKER) {
        status = sc_sandbox_docker_new(alloc, out);
        if (sc_status_is_ok(status) && audit != nullptr) {
            status = sc_audit_chain_append(audit,
                                           sc_str_from_cstr("sc.security.sandbox_selected"),
                                           sc_str_from_cstr("using Docker tool-runner sandbox"));
        }
        return status;
    }
    if (backend == SC_SANDBOX_BACKEND_PODMAN) {
        status = sc_sandbox_podman_new(alloc, out);
        if (sc_status_is_ok(status) && audit != nullptr) {
            status = sc_audit_chain_append(audit,
                                           sc_str_from_cstr("sc.security.sandbox_selected"),
                                           sc_str_from_cstr("using Podman tool-runner sandbox"));
        }
        return status;
    }
    if (backend == SC_SANDBOX_BACKEND_CONTAINER) {
        if (audit != nullptr) {
            (void)sc_audit_chain_append(audit,
                                        sc_str_from_cstr("sc.security.sandbox_unsupported"),
                                        sc_str_from_cstr("container runtime unspecified"));
        }
        return sc_status_unsupported("sc.sandbox.container.runtime_required");
    }
    if (backend == SC_SANDBOX_BACKEND_SEATBELT) {
        status = sc_sandbox_seatbelt_new(alloc, out);
        if (audit != nullptr) {
            (void)sc_audit_chain_append(audit,
                                        sc_str_from_cstr("sc.security.sandbox_unsupported"),
                                        sc_str_from_cstr("seatbelt"));
        }
        return status;
    }
    if (backend == SC_SANDBOX_BACKEND_APPCONTAINER) {
        status = sc_sandbox_appcontainer_new(alloc, out);
        if (audit != nullptr) {
            (void)sc_audit_chain_append(audit,
                                        sc_str_from_cstr("sc.security.sandbox_unsupported"),
                                        sc_str_from_cstr("appcontainer"));
        }
        return status;
    }
    if (backend != SC_SANDBOX_BACKEND_AUTO) {
        if (audit != nullptr) {
            (void)sc_audit_chain_append(audit,
                                        sc_str_from_cstr("sc.security.sandbox_unsupported"),
                                        sc_sandbox_backend_to_str(backend));
        }
        return sc_status_unsupported("sc.sandbox.backend_unsupported");
    }

    status = sc_sandbox_landlock_new(alloc, out);
    if (sc_status_is_ok(status)) {
        if (audit != nullptr) {
            status = sc_audit_chain_append(audit,
                                           sc_str_from_cstr("sc.security.sandbox_selected"),
                                           sc_str_from_cstr("using Linux Landlock sandbox"));
        }
        return status;
    }
    sc_status_clear(&status);

    status = sc_sandbox_bubblewrap_new(alloc, out);
    if (sc_status_is_ok(status)) {
        if (audit != nullptr) {
            status = sc_audit_chain_append(audit,
                                           sc_str_from_cstr("sc.security.sandbox_selected"),
                                           sc_str_from_cstr("using Linux Bubblewrap sandbox"));
        }
        return status;
    }
    sc_status_clear(&status);

    status = sc_sandbox_firejail_new(alloc, out);
    if (sc_status_is_ok(status)) {
        if (audit != nullptr) {
            status = sc_audit_chain_append(audit,
                                           sc_str_from_cstr("sc.security.sandbox_selected"),
                                           sc_str_from_cstr("using Linux Firejail sandbox"));
        }
        return status;
    }
    sc_status_clear(&status);

    status = sc_sandbox_docker_new(alloc, out);
    if (sc_status_is_ok(status)) {
        if (audit != nullptr) {
            status = sc_audit_chain_append(audit,
                                           sc_str_from_cstr("sc.security.sandbox_selected"),
                                           sc_str_from_cstr("using Docker tool-runner sandbox"));
        }
        return status;
    }
    sc_status_clear(&status);

    status = sc_sandbox_podman_new(alloc, out);
    if (sc_status_is_ok(status)) {
        if (audit != nullptr) {
            status = sc_audit_chain_append(audit,
                                           sc_str_from_cstr("sc.security.sandbox_selected"),
                                           sc_str_from_cstr("using Podman tool-runner sandbox"));
        }
        return status;
    }
    sc_status_clear(&status);

    status = sc_sandbox_noop_new(alloc, out);
    if (sc_status_is_ok(status) && audit != nullptr) {
        status = sc_audit_chain_append(audit,
                                       sc_str_from_cstr("sc.security.sandbox_fallback"),
                                       sc_str_from_cstr("using noop sandbox"));
    }
    return status;
}

sc_sandbox_backend sc_sandbox_backend_from_str(sc_str value)
{
    if (sc_str_equal(value, sc_str_from_cstr("auto")) || value.len == 0) {
        return SC_SANDBOX_BACKEND_AUTO;
    }
    if (sc_str_equal(value, sc_str_from_cstr("landlock"))) {
        return SC_SANDBOX_BACKEND_LANDLOCK;
    }
    if (sc_str_equal(value, sc_str_from_cstr("bubblewrap"))) {
        return SC_SANDBOX_BACKEND_BUBBLEWRAP;
    }
    if (sc_str_equal(value, sc_str_from_cstr("firejail"))) {
        return SC_SANDBOX_BACKEND_FIREJAIL;
    }
    if (sc_str_equal(value, sc_str_from_cstr("docker"))) {
        return SC_SANDBOX_BACKEND_DOCKER;
    }
    if (sc_str_equal(value, sc_str_from_cstr("podman"))) {
        return SC_SANDBOX_BACKEND_PODMAN;
    }
    if (sc_str_equal(value, sc_str_from_cstr("container")) ||
        sc_str_equal(value, sc_str_from_cstr("oci"))) {
        return SC_SANDBOX_BACKEND_CONTAINER;
    }
    if (sc_str_equal(value, sc_str_from_cstr("seatbelt"))) {
        return SC_SANDBOX_BACKEND_SEATBELT;
    }
    if (sc_str_equal(value, sc_str_from_cstr("appcontainer"))) {
        return SC_SANDBOX_BACKEND_APPCONTAINER;
    }
    if (sc_str_equal(value, sc_str_from_cstr("noop"))) {
        return SC_SANDBOX_BACKEND_NOOP;
    }
    return SC_SANDBOX_BACKEND_UNKNOWN;
}

sc_str sc_sandbox_backend_to_str(sc_sandbox_backend backend)
{
    switch (backend) {
    case SC_SANDBOX_BACKEND_AUTO:
        return sc_str_from_cstr("auto");
    case SC_SANDBOX_BACKEND_LANDLOCK:
        return sc_str_from_cstr("landlock");
    case SC_SANDBOX_BACKEND_BUBBLEWRAP:
        return sc_str_from_cstr("bubblewrap");
    case SC_SANDBOX_BACKEND_FIREJAIL:
        return sc_str_from_cstr("firejail");
    case SC_SANDBOX_BACKEND_DOCKER:
        return sc_str_from_cstr("docker");
    case SC_SANDBOX_BACKEND_PODMAN:
        return sc_str_from_cstr("podman");
    case SC_SANDBOX_BACKEND_CONTAINER:
        return sc_str_from_cstr("container");
    case SC_SANDBOX_BACKEND_SEATBELT:
        return sc_str_from_cstr("seatbelt");
    case SC_SANDBOX_BACKEND_APPCONTAINER:
        return sc_str_from_cstr("appcontainer");
    case SC_SANDBOX_BACKEND_NOOP:
        return sc_str_from_cstr("noop");
    case SC_SANDBOX_BACKEND_UNKNOWN:
    default:
        return sc_str_from_cstr("unknown");
    }
}

sc_sandbox_network_policy sc_sandbox_network_policy_from_str(sc_str value)
{
    if (sc_str_equal(value, sc_str_from_cstr("allowed-domains"))) {
        return SC_SANDBOX_NETWORK_ALLOWED_DOMAINS;
    }
    if (sc_str_equal(value, sc_str_from_cstr("none"))) {
        return SC_SANDBOX_NETWORK_NONE;
    }
    return SC_SANDBOX_NETWORK_FULL;
}

sc_str sc_sandbox_network_policy_to_str(sc_sandbox_network_policy policy)
{
    switch (policy) {
    case SC_SANDBOX_NETWORK_ALLOWED_DOMAINS:
        return sc_str_from_cstr("allowed-domains");
    case SC_SANDBOX_NETWORK_NONE:
        return sc_str_from_cstr("none");
    case SC_SANDBOX_NETWORK_FULL:
    default:
        return sc_str_from_cstr("full");
    }
}

static sc_status noop_check(void *impl,
                            const sc_sandbox_request *request,
                            sc_allocator *alloc,
                            sc_sandbox_decision *out)
{
    (void)impl;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.invalid_argument");
    }
    sc_status status = sc_sandbox_decision_begin(out, alloc, SC_SANDBOX_BACKEND_NOOP, "noop sandbox allows request");
    if (sc_status_is_ok(status) && request != nullptr && request->executable.ptr != nullptr && request->executable.len > 0) {
        status = sc_sandbox_decision_add_original_command(out, alloc, request);
    }
    return status;
}

static void noop_destroy(void *impl)
{
    (void)impl;
}

void sc_sandbox_decision_clear(sc_sandbox_decision *decision)
{
    if (decision == nullptr) {
        return;
    }
    for (size_t i = 0; i < decision->argv.len; i += 1) {
        sc_string *arg = sc_vec_at(&decision->argv, i);
        sc_string_clear(arg);
    }
    sc_vec_clear(&decision->argv);
    sc_string_clear(&decision->reason);
    sc_string_clear(&decision->fallback_reason);
    *decision = (sc_sandbox_decision){0};
}

bool sc_sandbox_executable_available(const char *path)
{
    return path != nullptr && access(path, X_OK) == 0;
}

sc_status sc_sandbox_decision_begin(sc_sandbox_decision *decision,
                                    sc_allocator *alloc,
                                    sc_sandbox_backend backend,
                                    const char *reason)
{
    sc_status status;

    if (decision == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.invalid_argument");
    }
    sc_sandbox_decision_clear(decision);
    sc_vec_init(&decision->argv, alloc, sizeof(sc_string));
    decision->struct_size = sizeof(*decision);
    decision->allowed = true;
    decision->resolved_backend = backend;
    status = sc_string_from_cstr(alloc, reason == nullptr ? "" : reason, &decision->reason);
    if (!sc_status_is_ok(status)) {
        sc_sandbox_decision_clear(decision);
    }
    return status;
}

sc_status sc_sandbox_decision_add_arg(sc_sandbox_decision *decision, sc_allocator *alloc, sc_str value)
{
    sc_string copy = {0};
    sc_status status;

    if (decision == nullptr || value.ptr == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.argv_invalid");
    }
    status = sc_string_from_str(alloc, value, &copy);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&decision->argv, &copy);
    }
    if (sc_status_is_ok(status)) {
        copy = (sc_string){0};
    }
    sc_string_clear(&copy);
    return status;
}

sc_status sc_sandbox_decision_add_cstr(sc_sandbox_decision *decision, sc_allocator *alloc, const char *value)
{
    if (value == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.argv_invalid");
    }
    return sc_sandbox_decision_add_arg(decision, alloc, sc_str_from_cstr(value));
}

sc_status sc_sandbox_decision_add_joined(sc_sandbox_decision *decision,
                                         sc_allocator *alloc,
                                         const char *prefix,
                                         sc_str value)
{
    sc_string_builder builder = {0};
    sc_string joined = {0};
    sc_status status;

    if (prefix == nullptr || value.ptr == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.argv_invalid");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, prefix);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, value);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &joined);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_arg(decision, alloc, sc_string_as_str(&joined));
    }
    sc_string_clear(&joined);
    return status;
}

sc_status sc_sandbox_decision_add_i64_suffix(sc_sandbox_decision *decision,
                                             sc_allocator *alloc,
                                             int64_t value,
                                             const char *suffix)
{
    char text[64] = {0};
    int written = snprintf(text, sizeof(text), "%lld%s", (long long)value, suffix == nullptr ? "" : suffix);

    if (written < 0 || (size_t)written >= sizeof(text)) {
        return sc_status_invalid_argument("sc.sandbox.argv_invalid");
    }
    return sc_sandbox_decision_add_cstr(decision, alloc, text);
}

sc_status sc_sandbox_decision_add_original_command(sc_sandbox_decision *decision,
                                                   sc_allocator *alloc,
                                                   const sc_sandbox_request *request)
{
    sc_status status;

    if (decision == nullptr || request == nullptr || request->executable.ptr == nullptr || request->executable.len == 0) {
        return sc_status_invalid_argument("sc.sandbox.argv_invalid");
    }
    status = sc_sandbox_decision_add_arg(decision, alloc, request->executable);
    for (size_t i = 0; sc_status_is_ok(status) && i < request->arg_count; i += 1) {
        status = sc_sandbox_decision_add_arg(decision, alloc, request->args[i]);
    }
    return status;
}
