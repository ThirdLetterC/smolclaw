#define _POSIX_C_SOURCE 200809L

#include "tools/process_runner.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

static sc_status make_cstr(sc_allocator *alloc, sc_str input, char **out);
static sc_status make_argv(sc_allocator *alloc,
                           const sc_tool_process_request *request,
                           char ***out,
                           size_t *out_count,
                           sc_sandbox_decision *decision);
static sc_status select_process_sandbox(sc_allocator *alloc,
                                        const sc_security_policy *policy,
                                        sc_sandbox **out);
static sc_status make_vec_views(sc_allocator *alloc, const sc_vec *strings, sc_str **out, size_t *out_count);
static sc_status make_env_views(sc_allocator *alloc, const sc_tool_process_request *request, sc_str **out, size_t *out_count);
static void free_views(sc_allocator *alloc, sc_str *views, size_t count);
static sc_status decision_to_argv(sc_allocator *alloc, const sc_sandbox_decision *decision, char ***out, size_t *out_count);
static sc_status make_envp(sc_allocator *alloc, const sc_tool_process_request *request, char ***out, size_t *out_count);
static sc_status append_env_passthrough(sc_allocator *alloc, char **envp, size_t capacity, size_t *count, sc_str name);
static sc_status make_env_assignment(sc_allocator *alloc, sc_str name, const char *value, char **out);
static void free_argv(sc_allocator *alloc, char **argv, size_t count);
static int64_t monotonic_ms();
static sc_status append_bytes(sc_allocator *alloc, sc_string *buffer, sc_str bytes, size_t max_bytes);
static bool env_name_secret(sc_str name);
static bool env_has_name(char **envp, size_t count, const char *name);
static void apply_process_limits(const sc_security_policy *policy);
static void apply_process_landlock(const sc_tool_process_request *request, const sc_sandbox_decision *decision);
static sc_status validate_process_network(const sc_tool_process_request *request);

sc_status sc_tool_process_run(sc_allocator *alloc,
                              const sc_tool_process_request *request,
                              sc_string *out)
{
    sc_tool_process_result result = {0};
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.process_runner.invalid_argument");
    }
    status = sc_tool_process_run_ex(alloc, request, &result);
    if (sc_status_is_ok(status)) {
        *out = result.output;
        result.output = (sc_string){0};
    }
    sc_tool_process_result_clear(&result);
    return status;
}

sc_status sc_tool_process_run_ex(sc_allocator *alloc,
                                 const sc_tool_process_request *request,
                                 sc_tool_process_result *out)
{
    int pipe_fds[2] = {-1, -1};
    char **argv = nullptr;
    char **envp = nullptr;
    size_t argv_count = 0;
    size_t envp_count = 0;
    sc_sandbox_decision sandbox_decision = {0};
    char *cwd = nullptr;
    pid_t pid = -1;
    pid_t process_group = -1;
    sc_string output = {0};
    int64_t started = 0;
    int status_code = 0;
    sc_status status;

    if (request == nullptr || out == nullptr || request->executable.ptr == nullptr || request->executable.len == 0) {
        return sc_status_invalid_argument("sc.process_runner.invalid_argument");
    }
    *out = (sc_tool_process_result){0};
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = validate_process_network(request);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = make_argv(alloc, request, &argv, &argv_count, &sandbox_decision);
    if (sc_status_is_ok(status)) {
        status = make_envp(alloc, request, &envp, &envp_count);
    }
    if (sc_status_is_ok(status) && request->cwd.ptr != nullptr && request->cwd.len > 0) {
        status = make_cstr(alloc, request->cwd, &cwd);
    }
    if (sc_status_is_ok(status) && pipe(pipe_fds) != 0) {
        status = sc_status_io("sc.process_runner.pipe_failed");
    }
    if (sc_status_is_ok(status) && fcntl(pipe_fds[0], F_SETFL, O_NONBLOCK) != 0) {
        status = sc_status_io("sc.process_runner.nonblock_failed");
    }
    if (sc_status_is_ok(status)) {
        pid_t child = fork();
        if (child < 0) {
            status = sc_status_io("sc.process_runner.spawn_failed");
        } else if (child == 0) {
            if (setpgid(0, 0) != 0) {
                _exit(126);
            }
            if (cwd != nullptr) {
                if (chdir(cwd) != 0) {
                    _exit(126);
                }
            }
            (void)close(pipe_fds[0]);
            (void)dup2(pipe_fds[1], STDOUT_FILENO);
            (void)dup2(pipe_fds[1], STDERR_FILENO);
            (void)close(pipe_fds[1]);
            apply_process_limits(request->policy);
            apply_process_landlock(request, &sandbox_decision);
            execve(argv[0], argv, envp);
            _exit(127);
        } else {
            pid = child;
            process_group = child;
            if (setpgid(child, child) != 0 && errno != EACCES) {
                status = sc_status_io("sc.process_runner.process_group_failed");
            }
            (void)close(pipe_fds[1]);
            pipe_fds[1] = -1;
        }
    }
    started = monotonic_ms();
    while (sc_status_is_ok(status)) {
        struct pollfd fd = {.fd = pipe_fds[0], .events = POLLIN};
        char chunk[1024] = {0};
        ssize_t read_count;
        int wait_result = 0;

        if (request->cancel_token != nullptr && request->cancel_token->cancel_requested) {
            status = sc_status_cancelled("sc.process_runner.cancelled");
            break;
        }
        if (request->timeout_ms > 0 && monotonic_ms() - started > request->timeout_ms) {
            status = sc_status_timeout("sc.process_runner.timeout");
            break;
        }
        wait_result = waitpid(pid, &status_code, WNOHANG);
        if (wait_result < 0) {
            status = sc_status_io("sc.process_runner.wait_failed");
            break;
        }
        if (poll(&fd, 1, 25) > 0 && (fd.revents & POLLIN) != 0) {
            read_count = read(pipe_fds[0], chunk, sizeof(chunk));
            if (read_count > 0) {
                status = append_bytes(alloc,
                                      &output,
                                      sc_str_from_parts(chunk, (size_t)read_count),
                                      request->max_output_bytes == 0 ? 4096 : request->max_output_bytes);
            }
        }
        if (wait_result == pid) {
            pid = -1;
            /* The child can exit before the pipe is drained, so keep reading until EOF. */
            while (sc_status_is_ok(status) && (read_count = read(pipe_fds[0], chunk, sizeof(chunk))) > 0) {
                status = append_bytes(alloc,
                                      &output,
                                      sc_str_from_parts(chunk, (size_t)read_count),
                                      request->max_output_bytes == 0 ? 4096 : request->max_output_bytes);
            }
            if (process_group > 0) {
                (void)kill(-process_group, SIGKILL);
            }
            if (sc_status_is_ok(status) && (!WIFEXITED(status_code) || WEXITSTATUS(status_code) != 0)) {
                out->exited = WIFEXITED(status_code);
                out->exit_code = WIFEXITED(status_code) ? WEXITSTATUS(status_code) : -1;
                status = sc_status_io("sc.process_runner.exit_failed");
            } else if (sc_status_is_ok(status)) {
                out->exited = true;
                out->exit_code = WEXITSTATUS(status_code);
            }
            break;
        }
    }
    if (!sc_status_is_ok(status) && pid > 0) {
        (void)kill(process_group > 0 ? -process_group : pid, SIGKILL);
        (void)waitpid(pid, nullptr, 0);
    }
    if (sc_status_is_ok(status) || status.code == SC_ERR_IO) {
        out->output = output;
        output = (sc_string){0};
    }
    sc_string_clear(&output);
    if (pipe_fds[0] >= 0) {
        (void)close(pipe_fds[0]);
    }
    if (pipe_fds[1] >= 0) {
        (void)close(pipe_fds[1]);
    }
    free_argv(alloc, argv, argv_count);
    free_argv(alloc, envp, envp_count);
    sc_sandbox_decision_clear(&sandbox_decision);
    sc_free(alloc, cwd, request->cwd.len + 1, _Alignof(char));
    return status;
}

void sc_tool_process_result_clear(sc_tool_process_result *result)
{
    if (result == nullptr) {
        return;
    }
    sc_string_clear(&result->output);
    *result = (sc_tool_process_result){0};
}

static sc_status make_cstr(sc_allocator *alloc, sc_str input, char **out)
{
    char *copy = nullptr;
    size_t size = 0;

    if (out == nullptr || input.ptr == nullptr || ckd_add(&size, input.len, 1)) {
        return sc_status_invalid_argument("sc.process_runner.string_invalid");
    }
    copy = sc_alloc(alloc, size, _Alignof(char));
    if (copy == nullptr) {
        return sc_status_no_memory();
    }
    (void)memcpy(copy, input.ptr, input.len);
    copy[input.len] = '\0';
    *out = copy;
    return sc_status_ok();
}

static sc_status make_argv(sc_allocator *alloc,
                           const sc_tool_process_request *request,
                           char ***out,
                           size_t *out_count,
                           sc_sandbox_decision *decision)
{
    sc_sandbox *sandbox = nullptr;
    sc_str *allowed_devices = nullptr;
    sc_str *env_passthrough = nullptr;
    size_t allowed_device_count = 0;
    size_t env_passthrough_count = 0;
    sc_sandbox_request sandbox_request = {0};
    sc_status status;

    if (request == nullptr || out == nullptr || out_count == nullptr || decision == nullptr) {
        return sc_status_invalid_argument("sc.process_runner.argv_invalid_argument");
    }
    status = select_process_sandbox(alloc, request->policy, &sandbox);
    if (sc_status_is_ok(status)) {
        status = make_vec_views(alloc,
                                request->policy == nullptr ? nullptr : &request->policy->sandbox_allow_devices,
                                &allowed_devices,
                                &allowed_device_count);
    }
    if (sc_status_is_ok(status)) {
        status = make_env_views(alloc, request, &env_passthrough, &env_passthrough_count);
    }
    if (sc_status_is_ok(status)) {
        sandbox_request = (sc_sandbox_request){
            .struct_size = sizeof(sandbox_request),
            .operation = sc_str_from_cstr("process.exec"),
            .subject = request->executable,
            .executable = request->executable,
            .args = request->args,
            .arg_count = request->arg_count,
            .cwd = request->cwd,
            .network = request->policy == nullptr ? SC_SANDBOX_NETWORK_FULL : request->policy->sandbox_network,
            .memory_limit_mb = request->policy == nullptr ? 0 : request->policy->sandbox_memory_limit_mb,
            .max_subprocesses = request->policy == nullptr ? 0 : request->policy->sandbox_max_subprocesses,
            .allowed_devices = allowed_devices,
            .allowed_device_count = allowed_device_count,
            .env_passthrough = env_passthrough,
            .env_passthrough_count = env_passthrough_count,
            .container_runtime = request->policy == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&request->policy->sandbox_container_runtime),
            .image_name = request->policy == nullptr ? sc_str_from_cstr("") : sc_string_as_str(&request->policy->sandbox_image_name),
        };
        status = sc_sandbox_check(sandbox, &sandbox_request, alloc, decision);
    }
    if (sc_status_is_ok(status)) {
        status = decision_to_argv(alloc, decision, out, out_count);
    }
    free_views(alloc, env_passthrough, env_passthrough_count);
    free_views(alloc, allowed_devices, allowed_device_count);
    sc_sandbox_destroy(sandbox);
    return status;
}

static sc_status select_process_sandbox(sc_allocator *alloc,
                                        const sc_security_policy *policy,
                                        sc_sandbox **out)
{
    sc_sandbox_backend requested = policy == nullptr ? SC_SANDBOX_BACKEND_NOOP : policy->sandbox_backend;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.process_runner.sandbox_invalid_argument");
    }
    *out = nullptr;
    if (requested == SC_SANDBOX_BACKEND_AUTO) {
        const sc_vec *fallback_order = policy == nullptr ? nullptr : &policy->sandbox_fallback_order;
        for (size_t i = 0; fallback_order != nullptr && i < fallback_order->len; i += 1) {
            const sc_sandbox_backend *backend = sc_vec_at_const(fallback_order, i);
            sc_status status = backend == nullptr ? sc_status_invalid_argument("sc.process_runner.sandbox_backend_invalid")
                                                  : sc_sandbox_select_backend(alloc, *backend, out, nullptr);
            if (sc_status_is_ok(status)) {
                return status;
            }
            sc_status_clear(&status);
        }
        if (policy != nullptr && policy->sandbox_allow_noop_fallback) {
            return sc_sandbox_noop_new(alloc, out);
        }
        return sc_status_unsupported("sc.process_runner.sandbox_backend_unavailable");
    }
    if (requested == SC_SANDBOX_BACKEND_CONTAINER) {
        if (policy == nullptr || policy->sandbox_container_runtime.len == 0) {
            return sc_status_invalid_argument("sc.process_runner.container_runtime_required");
        }
        return sc_sandbox_container_new(alloc, sc_string_as_str(&policy->sandbox_container_runtime), out);
    }
    if (requested == SC_SANDBOX_BACKEND_DOCKER && policy != nullptr && policy->sandbox_docker_path.len > 0) {
        return sc_sandbox_container_new(alloc, sc_string_as_str(&policy->sandbox_docker_path), out);
    }
    if (requested == SC_SANDBOX_BACKEND_PODMAN && policy != nullptr && policy->sandbox_podman_path.len > 0) {
        return sc_sandbox_container_new(alloc, sc_string_as_str(&policy->sandbox_podman_path), out);
    }
    return sc_sandbox_select_backend(alloc, requested, out, nullptr);
}

static sc_status make_vec_views(sc_allocator *alloc, const sc_vec *strings, sc_str **out, size_t *out_count)
{
    sc_str *views = nullptr;

    if (out == nullptr || out_count == nullptr) {
        return sc_status_invalid_argument("sc.process_runner.views_invalid_argument");
    }
    *out = nullptr;
    *out_count = 0;
    if (strings == nullptr || strings->len == 0) {
        return sc_status_ok();
    }
    views = sc_alloc(alloc, strings->len * sizeof(*views), _Alignof(sc_str));
    if (views == nullptr) {
        return sc_status_no_memory();
    }
    for (size_t i = 0; i < strings->len; i += 1) {
        const sc_string *string = sc_vec_at_const(strings, i);
        views[i] = string == nullptr ? sc_str_from_cstr("") : sc_string_as_str(string);
    }
    *out = views;
    *out_count = strings->len;
    return sc_status_ok();
}

static sc_status make_env_views(sc_allocator *alloc, const sc_tool_process_request *request, sc_str **out, size_t *out_count)
{
    const sc_security_policy *policy = request == nullptr ? nullptr : request->policy;
    size_t capacity = (policy == nullptr ? 0u : policy->shell_env_passthrough.len) +
                      (request == nullptr ? 0u : request->env_passthrough_count);
    sc_str *views = nullptr;
    size_t count = 0;

    if (out == nullptr || out_count == nullptr) {
        return sc_status_invalid_argument("sc.process_runner.views_invalid_argument");
    }
    *out = nullptr;
    *out_count = 0;
    if (capacity == 0) {
        return sc_status_ok();
    }
    views = sc_alloc(alloc, capacity * sizeof(*views), _Alignof(sc_str));
    if (views == nullptr) {
        return sc_status_no_memory();
    }
    memset(views, 0, capacity * sizeof(*views));
    /*
     * Secret-looking or malformed names stay as empty views. Sandbox builders
     * skip those entries, while the array size still matches the allocation.
     */
    if (policy != nullptr) {
        for (size_t i = 0; i < policy->shell_env_passthrough.len; i += 1) {
            const sc_string *name = sc_vec_at_const(&policy->shell_env_passthrough, i);
            if (name == nullptr || name->len == 0 || env_name_secret(sc_string_as_str(name))) {
                count += 1;
                continue;
            }
            views[count] = sc_string_as_str(name);
            count += 1;
        }
    }
    if (request != nullptr) {
        for (size_t i = 0; i < request->env_passthrough_count; i += 1) {
            if (request->env_passthrough[i].ptr == nullptr ||
                request->env_passthrough[i].len == 0 ||
                env_name_secret(request->env_passthrough[i])) {
                count += 1;
                continue;
            }
            views[count] = request->env_passthrough[i];
            count += 1;
        }
    }
    *out = views;
    *out_count = capacity;
    return sc_status_ok();
}

static void free_views(sc_allocator *alloc, sc_str *views, size_t count)
{
    if (views == nullptr) {
        return;
    }
    sc_free(alloc, views, count * sizeof(*views), _Alignof(sc_str));
}

static sc_status decision_to_argv(sc_allocator *alloc, const sc_sandbox_decision *decision, char ***out, size_t *out_count)
{
    char **argv = nullptr;
    sc_status status = sc_status_ok();

    if (decision == nullptr || out == nullptr || out_count == nullptr || !decision->allowed || decision->argv.len == 0) {
        return sc_status_invalid_argument("sc.process_runner.argv_invalid_argument");
    }
    argv = sc_alloc(alloc, (decision->argv.len + 1u) * sizeof(*argv), _Alignof(char *));
    if (argv == nullptr) {
        return sc_status_no_memory();
    }
    memset(argv, 0, (decision->argv.len + 1u) * sizeof(*argv));
    for (size_t i = 0; sc_status_is_ok(status) && i < decision->argv.len; i += 1) {
        const sc_string *arg = sc_vec_at_const(&decision->argv, i);
        status = make_cstr(alloc, arg == nullptr ? sc_str_from_cstr("") : sc_string_as_str(arg), &argv[i]);
    }
    if (!sc_status_is_ok(status)) {
        free_argv(alloc, argv, decision->argv.len + 1u);
        return status;
    }
    *out = argv;
    *out_count = decision->argv.len + 1u;
    return sc_status_ok();
}

static sc_status make_envp(sc_allocator *alloc, const sc_tool_process_request *request, char ***out, size_t *out_count)
{
    const sc_security_policy *policy = request == nullptr ? nullptr : request->policy;
    char **envp = nullptr;
    size_t capacity = (policy == nullptr ? 0u : policy->shell_env_passthrough.len) +
                      (request == nullptr ? 0u : request->env_passthrough_count) + 3u;
    size_t count = 0;
    sc_status status = sc_status_ok();

    if (out == nullptr || out_count == nullptr) {
        return sc_status_invalid_argument("sc.process_runner.env_invalid_argument");
    }
    envp = sc_alloc(alloc, capacity * sizeof(*envp), _Alignof(char *));
    if (envp == nullptr) {
        return sc_status_no_memory();
    }
    memset(envp, 0, capacity * sizeof(*envp));
    if (policy != nullptr) {
        for (size_t i = 0; sc_status_is_ok(status) && i < policy->shell_env_passthrough.len; i += 1) {
            const sc_string *name = sc_vec_at_const(&policy->shell_env_passthrough, i);
            if (name == nullptr || name->len == 0) {
                continue;
            }
            status = append_env_passthrough(alloc, envp, capacity, &count, sc_string_as_str(name));
        }
    }
    if (request != nullptr) {
        for (size_t i = 0; sc_status_is_ok(status) && i < request->env_passthrough_count; i += 1) {
            status = append_env_passthrough(alloc, envp, capacity, &count, request->env_passthrough[i]);
        }
    }
    if (sc_status_is_ok(status) && !env_has_name(envp, count, "PATH")) {
        status = make_env_assignment(alloc, sc_str_from_cstr("PATH"), "/usr/bin:/bin", &envp[count]);
        if (sc_status_is_ok(status)) {
            count += 1;
        }
    }
    if (sc_status_is_ok(status) && !env_has_name(envp, count, "LANG")) {
        status = make_env_assignment(alloc, sc_str_from_cstr("LANG"), "C", &envp[count]);
        if (sc_status_is_ok(status)) {
            count += 1;
        }
    }
    if (!sc_status_is_ok(status)) {
        free_argv(alloc, envp, capacity);
        return status;
    }
    *out = envp;
    *out_count = capacity;
    return sc_status_ok();
}

static sc_status append_env_passthrough(sc_allocator *alloc, char **envp, size_t capacity, size_t *count, sc_str name)
{
    char name_buf[128] = {0};
    const char *value = nullptr;

    if (envp == nullptr || count == nullptr || *count >= capacity || name.ptr == nullptr || name.len == 0 ||
        name.len >= sizeof(name_buf) || env_name_secret(name)) {
        return sc_status_ok();
    }
    /* Only explicit, non-secret names are copied from the host environment. */
    (void)memcpy(name_buf, name.ptr, name.len);
    value = getenv(name_buf);
    if (value == nullptr) {
        return sc_status_ok();
    }
    if (env_has_name(envp, *count, name_buf)) {
        return sc_status_ok();
    }
    sc_status status = make_env_assignment(alloc, name, value, &envp[*count]);
    if (sc_status_is_ok(status)) {
        *count += 1;
    }
    return status;
}

static sc_status make_env_assignment(sc_allocator *alloc, sc_str name, const char *value, char **out)
{
    size_t value_len = value == nullptr ? 0U : strlen(value);
    size_t total = 0;
    char *copy = nullptr;

    if (out == nullptr || name.ptr == nullptr || name.len == 0 || value == nullptr ||
        ckd_add(&total, name.len, value_len) || ckd_add(&total, total, 2)) {
        return sc_status_invalid_argument("sc.process_runner.env_invalid");
    }
    copy = sc_alloc(alloc, total, _Alignof(char));
    if (copy == nullptr) {
        return sc_status_no_memory();
    }
    (void)memcpy(copy, name.ptr, name.len);
    copy[name.len] = '=';
    (void)memcpy(copy + name.len + 1u, value, value_len);
    copy[total - 1u] = '\0';
    *out = copy;
    return sc_status_ok();
}

static sc_status validate_process_network(const sc_tool_process_request *request)
{
    const sc_security_policy *policy = request == nullptr ? nullptr : request->policy;

    if (policy == nullptr) {
        return sc_status_ok();
    }
    if (request->network_subject.len > 0) {
        return sc_security_validate_sandbox_network(policy, request->network_subject);
    }
    if (!request->may_use_network) {
        return sc_status_ok();
    }
    if (policy->sandbox_network == SC_SANDBOX_NETWORK_NONE) {
        return sc_status_security_denied("sc.security.sandbox_network_denied");
    }
    if (policy->sandbox_network == SC_SANDBOX_NETWORK_ALLOWED_DOMAINS) {
        return sc_status_security_denied("sc.security.sandbox_network_subject_required");
    }
    return sc_status_ok();
}

static void free_argv(sc_allocator *alloc, char **argv, size_t count)
{
    if (argv == nullptr) {
        return;
    }
    for (size_t i = 0; i < count; i += 1) {
        if (argv[i] != nullptr) {
            sc_free(alloc, argv[i], strlen(argv[i]) + 1, _Alignof(char));
        }
    }
    sc_free(alloc, argv, count * sizeof(*argv), _Alignof(char *));
}

static int64_t monotonic_ms()
{
    struct timespec ts = {0};
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0) {
        return 0;
    }
    return (int64_t)ts.tv_sec * 1000 + (int64_t)(ts.tv_nsec / 1000000);
}

static sc_status append_bytes(sc_allocator *alloc, sc_string *buffer, sc_str bytes, size_t max_bytes)
{
    sc_string_builder builder = {0};
    sc_string next = {0};
    size_t available = 0;
    bool truncated = false;
    sc_status status;

    if (buffer == nullptr || bytes.ptr == nullptr) {
        return sc_status_invalid_argument("sc.process_runner.append_invalid");
    }
    if (buffer->len >= max_bytes) {
        return sc_status_io("sc.process_runner.output_too_large");
    }
    available = max_bytes - buffer->len;
    if (bytes.len > available) {
        bytes.len = available;
        truncated = true;
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, sc_string_as_str(buffer));
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, bytes);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &next);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        sc_string_clear(buffer);
        *buffer = next;
    }
    if (sc_status_is_ok(status) && truncated) {
        return sc_status_io("sc.process_runner.output_too_large");
    }
    return status;
}

static bool env_name_secret(sc_str name)
{
    static const char *needles[] = {"KEY", "TOKEN", "SECRET", "PASSWORD", "AUTH", "COOKIE"};

    if (name.ptr == nullptr || name.len == 0) {
        return true;
    }
    for (size_t i = 0; i < name.len; i += 1) {
        char ch = name.ptr[i];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_')) {
            return true;
        }
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(needles); i += 1) {
        size_t needle_len = strlen(needles[i]);
        for (size_t j = 0; j + needle_len <= name.len; j += 1) {
            if (memcmp(name.ptr + j, needles[i], needle_len) == 0) {
                return true;
            }
        }
    }
    return false;
}

static bool env_has_name(char **envp, size_t count, const char *name)
{
    size_t name_len = name == nullptr ? 0U : strlen(name);

    if (envp == nullptr || name_len == 0) {
        return false;
    }
    for (size_t i = 0; i < count; i += 1) {
        if (envp[i] != nullptr && strncmp(envp[i], name, name_len) == 0 && envp[i][name_len] == '=') {
            return true;
        }
    }
    return false;
}

static void apply_process_limits(const sc_security_policy *policy)
{
    if (policy == nullptr) {
        return;
    }
    if (policy->sandbox_memory_limit_mb > 0) {
#ifdef RLIMIT_AS
        struct rlimit limit = {0};
        limit.rlim_cur = (rlim_t)policy->sandbox_memory_limit_mb * 1024u * 1024u;
        limit.rlim_max = limit.rlim_cur;
        (void)setrlimit(RLIMIT_AS, &limit);
#endif
    }
    if (policy->sandbox_max_subprocesses > 0) {
#ifdef RLIMIT_NPROC
        struct rlimit limit = {0};
        limit.rlim_cur = (rlim_t)policy->sandbox_max_subprocesses;
        limit.rlim_max = limit.rlim_cur;
        (void)setrlimit(RLIMIT_NPROC, &limit);
#endif
    }
}

static void apply_process_landlock(const sc_tool_process_request *request, const sc_sandbox_decision *decision)
{
    static const char *readonly_paths[] = {"/usr", "/bin", "/lib", "/lib64", "/etc"};
    sc_landlock_path_rule rules[1 + SC_ARRAY_LEN(readonly_paths)] = {0};
    size_t rule_count = 0;

    if (request == nullptr || decision == nullptr || !decision->apply_landlock) {
        return;
    }
    /*
     * This runs in the child after fork and before exec. Use _exit on failure
     * so the parent never continues with a process missing its filesystem gate.
     */
    if (request->cwd.ptr == nullptr || request->cwd.len == 0) {
        _exit(126);
    }
    rules[rule_count] = (sc_landlock_path_rule){
        .struct_size = sizeof(rules[rule_count]),
        .path = request->cwd,
        .allowed_access = sc_landlock_fs_read_write_access(),
    };
    rule_count += 1;
    for (size_t i = 0; i < SC_ARRAY_LEN(readonly_paths); i += 1) {
        if (access(readonly_paths[i], R_OK) != 0) {
            continue;
        }
        rules[rule_count] = (sc_landlock_path_rule){
            .struct_size = sizeof(rules[rule_count]),
            .path = sc_str_from_cstr(readonly_paths[i]),
            .allowed_access = sc_landlock_fs_read_access(),
        };
        rule_count += 1;
    }
    sc_landlock_ruleset ruleset = {
        .struct_size = sizeof(ruleset),
        .handled_access = sc_landlock_fs_read_write_access(),
        .rules = rules,
        .rule_count = rule_count,
    };
    sc_status status = sc_landlock_restrict_self(&ruleset);
    if (!sc_status_is_ok(status)) {
        sc_status_clear(&status);
        _exit(126);
    }
}
