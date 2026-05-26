#include "security/sandbox_exec_internal.h"

#include <string.h>

typedef struct container_runtime {
    sc_allocator *alloc;
    sc_string path;
    sc_sandbox_backend backend;
    const char *name;
    const char *display_name;
} container_runtime;

static sc_status container_check(void *impl,
                                 const sc_sandbox_request *request,
                                 sc_allocator *alloc,
                                 sc_sandbox_decision *out);
static void container_destroy(void *impl);
static sc_status container_new(sc_allocator *alloc,
                               sc_str runtime,
                               sc_sandbox_backend backend,
                               const char *name,
                               const char *display_name,
                               sc_sandbox **out);

static const sc_sandbox_vtab container_vtab = {
    .struct_size = sizeof(sc_sandbox_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "container-sandbox",
    .display_name = "Container-compatible tool-runner sandbox",
    .feature_flag = "SC_SANDBOX_CONTAINER",
    .capabilities = SC_CONTRACT_CAP_SECURE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .check = container_check,
    .destroy = container_destroy,
};

sc_status sc_sandbox_docker_new(sc_allocator *alloc, sc_sandbox **out)
{
    return container_new(alloc,
                         sc_str_from_cstr("/usr/bin/docker"),
                         SC_SANDBOX_BACKEND_DOCKER,
                         "docker",
                         "Docker tool-runner sandbox",
                         out);
}

sc_status sc_sandbox_podman_new(sc_allocator *alloc, sc_sandbox **out)
{
    return container_new(alloc,
                         sc_str_from_cstr("/usr/bin/podman"),
                         SC_SANDBOX_BACKEND_PODMAN,
                         "podman",
                         "Podman tool-runner sandbox",
                         out);
}

sc_status sc_sandbox_container_new(sc_allocator *alloc, sc_str runtime, sc_sandbox **out)
{
    return container_new(alloc,
                         runtime,
                         SC_SANDBOX_BACKEND_CONTAINER,
                         "container",
                         "Container-compatible tool-runner sandbox",
                         out);
}

static sc_status container_new(sc_allocator *alloc,
                               sc_str runtime,
                               sc_sandbox_backend backend,
                               const char *name,
                               const char *display_name,
                               sc_sandbox **out)
{
    container_runtime *container = nullptr;
    sc_status status;

    if (out == nullptr || runtime.ptr == nullptr || runtime.len == 0) {
        return sc_status_invalid_argument("sc.sandbox.container.invalid_argument");
    }
    *out = nullptr;
    if (runtime.len >= 4096) {
        return sc_status_invalid_argument("sc.sandbox.container.runtime_invalid");
    }
    char runtime_path[4096] = {0};
    (void)memcpy(runtime_path, runtime.ptr, runtime.len);
    if (!sc_sandbox_executable_available(runtime_path)) {
        return sc_status_unsupported("sc.sandbox.container.unavailable");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    container = sc_alloc(alloc, sizeof(*container), _Alignof(container_runtime));
    if (container == nullptr) {
        return sc_status_no_memory();
    }
    *container = (container_runtime){
        .alloc = alloc,
        .backend = backend,
        .name = name,
        .display_name = display_name,
    };
    status = sc_string_from_str(alloc, runtime, &container->path);
    if (!sc_status_is_ok(status)) {
        sc_free(alloc, container, sizeof(*container), _Alignof(container_runtime));
        return status;
    }
    status = sc_sandbox_new(alloc, &container_vtab, container, out);
    if (!sc_status_is_ok(status)) {
        container_destroy(container);
    }
    return status;
}

static sc_status container_check(void *impl,
                                 const sc_sandbox_request *request,
                                 sc_allocator *alloc,
                                 sc_sandbox_decision *out)
{
    static const char *base_args[] = {"run", "--rm", "--init", "--pull=never"};
    const container_runtime *container = impl;
    sc_str image_name = sc_str_from_cstr("alpine:3.23.4");
    sc_status status;

    if (container == nullptr ||
        request == nullptr ||
        out == nullptr ||
        request->executable.ptr == nullptr ||
        request->executable.len == 0) {
        return sc_status_invalid_argument("sc.sandbox.container.invalid_argument");
    }
    if (request->cwd.ptr == nullptr || request->cwd.len == 0) {
        return sc_status_invalid_argument("sc.sandbox.container.workspace_required");
    }
    if (request->image_name.ptr != nullptr && request->image_name.len > 0) {
        image_name = request->image_name;
    }
    status = sc_sandbox_decision_begin(out, alloc, container->backend, "container sandbox plan");
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_arg(out, alloc, sc_string_as_str(&container->path));
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < (sizeof(base_args) / sizeof(base_args[0])); i += 1) {
        status = sc_sandbox_decision_add_cstr(out, alloc, base_args[i]);
    }
    if (sc_status_is_ok(status) && request->network == SC_SANDBOX_NETWORK_NONE) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--network");
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_cstr(out, alloc, "none");
        }
    }
    if (sc_status_is_ok(status) && request->memory_limit_mb > 0) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--memory");
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_i64_suffix(out, alloc, request->memory_limit_mb, "m");
        }
    }
    if (sc_status_is_ok(status) && request->max_subprocesses > 0) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--pids-limit");
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_i64_suffix(out, alloc, request->max_subprocesses, "");
        }
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < request->env_passthrough_count; i += 1) {
        if (request->env_passthrough[i].ptr == nullptr || request->env_passthrough[i].len == 0) {
            continue;
        }
        status = sc_sandbox_decision_add_cstr(out, alloc, "--env");
        if (sc_status_is_ok(status)) {
            status = sc_sandbox_decision_add_arg(out, alloc, request->env_passthrough[i]);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--volume");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_joined(out, alloc, "", request->cwd);
    }
    if (sc_status_is_ok(status)) {
        sc_string *volume = sc_vec_at(&out->argv, out->argv.len - 1u);
        sc_string_builder builder = {0};
        sc_string next = {0};

        if (volume == nullptr) {
            status = sc_status_invalid_argument("sc.sandbox.container.volume_invalid");
        } else {
            sc_string_builder_init(&builder, alloc);
            status = sc_string_builder_append(&builder, sc_string_as_str(volume));
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_append_cstr(&builder, ":/workspace:rw");
            }
            if (sc_status_is_ok(status)) {
                status = sc_string_builder_finish(&builder, &next);
            } else {
                sc_string_builder_clear(&builder);
            }
            if (sc_status_is_ok(status)) {
                sc_string_clear(volume);
                *volume = next;
                next = (sc_string){0};
            }
            sc_string_clear(&next);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "--workdir");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_cstr(out, alloc, "/workspace");
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_arg(out, alloc, image_name);
    }
    if (sc_status_is_ok(status)) {
        status = sc_sandbox_decision_add_original_command(out, alloc, request);
    }
    if (!sc_status_is_ok(status)) {
        sc_sandbox_decision_clear(out);
    }
    return status;
}

static void container_destroy(void *impl)
{
    container_runtime *container = impl;
    sc_allocator *alloc = nullptr;

    if (container == nullptr) {
        return;
    }
    alloc = container->alloc == nullptr ? sc_allocator_heap() : container->alloc;
    sc_string_clear(&container->path);
    sc_free(alloc, container, sizeof(*container), _Alignof(container_runtime));
}
