#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include "security/sandbox_exec_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#if defined(__linux__) && __has_include(<linux/landlock.h>)
#include <linux/landlock.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#define SC_HAVE_LINUX_LANDLOCK_HEADERS 1
#endif

static sc_status landlock_check(void *impl,
                                const sc_sandbox_request *request,
                                sc_allocator *alloc,
                                sc_sandbox_decision *out);
static void landlock_destroy(void *impl);

static const sc_sandbox_vtab landlock_vtab = {
    .struct_size = sizeof(sc_sandbox_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "landlock-sandbox",
    .display_name = "Linux Landlock sandbox",
    .feature_flag = "SC_SANDBOX_LANDLOCK",
    .capabilities = SC_CONTRACT_CAP_SECURE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .check = landlock_check,
    .destroy = landlock_destroy,
};

uint64_t sc_landlock_fs_read_access()
{
    return (uint64_t)SC_LANDLOCK_FS_READ_FILE |
           (uint64_t)SC_LANDLOCK_FS_READ_DIR;
}

uint64_t sc_landlock_fs_read_write_access()
{
    return sc_landlock_fs_read_access() |
           (uint64_t)SC_LANDLOCK_FS_WRITE_FILE |
           (uint64_t)SC_LANDLOCK_FS_REMOVE_DIR |
           (uint64_t)SC_LANDLOCK_FS_REMOVE_FILE |
           (uint64_t)SC_LANDLOCK_FS_MAKE_CHAR |
           (uint64_t)SC_LANDLOCK_FS_MAKE_DIR |
           (uint64_t)SC_LANDLOCK_FS_MAKE_REG |
           (uint64_t)SC_LANDLOCK_FS_MAKE_SOCK |
           (uint64_t)SC_LANDLOCK_FS_MAKE_FIFO |
           (uint64_t)SC_LANDLOCK_FS_MAKE_BLOCK |
           (uint64_t)SC_LANDLOCK_FS_MAKE_SYM |
           (uint64_t)SC_LANDLOCK_FS_REFER |
           (uint64_t)SC_LANDLOCK_FS_TRUNCATE;
}

#if defined(SC_HAVE_LINUX_LANDLOCK_HEADERS) && defined(SYS_landlock_create_ruleset) && defined(SYS_landlock_add_rule) && defined(SYS_landlock_restrict_self)
static uint64_t landlock_access_for_abi(uint32_t abi);
static sc_status landlock_status_from_errno(int err);
static int landlock_create_ruleset_sys(const struct landlock_ruleset_attr *attr, size_t size, uint32_t flags);
static int landlock_add_rule_sys(int ruleset_fd,
                                 enum landlock_rule_type type,
                                 const struct landlock_path_beneath_attr *attr,
                                 uint32_t flags);
static int landlock_restrict_self_sys(int ruleset_fd, uint32_t flags);
static sc_status validate_ruleset(const sc_landlock_ruleset *ruleset);

uint32_t sc_landlock_abi_version()
{
    errno = 0;
    auto abi = landlock_create_ruleset_sys(nullptr, 0, LANDLOCK_CREATE_RULESET_VERSION);
    if (abi <= 0) {
        return 0;
    }
    return (uint32_t)abi;
}

uint64_t sc_landlock_supported_fs_access()
{
    return landlock_access_for_abi(sc_landlock_abi_version());
}

bool sc_landlock_available()
{
    return sc_landlock_supported_fs_access() != 0;
}

sc_status sc_landlock_restrict_self(const sc_landlock_ruleset *ruleset)
{
    struct landlock_ruleset_attr ruleset_attr = {0};
    int ruleset_fd = -1;
    uint64_t supported_access = 0;
    char *path = nullptr;
    sc_status status = validate_ruleset(ruleset);

    if (!sc_status_is_ok(status)) {
        return status;
    }

    supported_access = sc_landlock_supported_fs_access();
    if (supported_access == 0) {
        return sc_status_unsupported("sc.sandbox.landlock.unavailable");
    }

    ruleset_attr.handled_access_fs = ruleset->handled_access & supported_access;
    if (ruleset_attr.handled_access_fs == 0) {
        return sc_status_invalid_argument("sc.sandbox.landlock.empty_access");
    }

    ruleset_fd = landlock_create_ruleset_sys(&ruleset_attr, sizeof(ruleset_attr), 0);
    if (ruleset_fd < 0) {
        return landlock_status_from_errno(errno);
    }

    for (size_t i = 0; i < ruleset->rule_count; i += 1) {
        const sc_landlock_path_rule *rule = &ruleset->rules[i];
        struct landlock_path_beneath_attr path_attr = {0};
        int open_flags = O_RDONLY | O_CLOEXEC;
        int path_fd = -1;

#ifdef O_PATH
        open_flags = O_PATH | O_CLOEXEC;
#endif

        if (rule->path.ptr == nullptr || rule->path.len == 0 || rule->path.len == SIZE_MAX) {
            status = sc_status_invalid_argument("sc.sandbox.landlock.invalid_path");
            goto cleanup;
        }

        path = sc_alloc(sc_allocator_heap(), rule->path.len + 1, _Alignof(char));
        if (path == nullptr) {
            status = sc_status_no_memory();
            goto cleanup;
        }
        (void)memcpy(path, rule->path.ptr, rule->path.len);
        path[rule->path.len] = '\0';

        path_fd = open(path, open_flags);
        sc_free(sc_allocator_heap(), path, rule->path.len + 1, _Alignof(char));
        path = nullptr;
        if (path_fd < 0) {
            status = landlock_status_from_errno(errno);
            goto cleanup;
        }

        path_attr.allowed_access = rule->allowed_access & ruleset_attr.handled_access_fs;
        path_attr.parent_fd = path_fd;
        if (path_attr.allowed_access == 0) {
            (void)close(path_fd);
            status = sc_status_invalid_argument("sc.sandbox.landlock.empty_rule_access");
            goto cleanup;
        }
        if (landlock_add_rule_sys(ruleset_fd, LANDLOCK_RULE_PATH_BENEATH, &path_attr, 0) != 0) {
            auto saved_errno = errno;
            (void)close(path_fd);
            status = landlock_status_from_errno(saved_errno);
            goto cleanup;
        }
        if (close(path_fd) != 0) {
            status = sc_status_io("sc.sandbox.landlock.close_failed");
            goto cleanup;
        }
    }

    if (prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0) != 0) {
        status = landlock_status_from_errno(errno);
        goto cleanup;
    }
    if (landlock_restrict_self_sys(ruleset_fd, 0) != 0) {
        status = landlock_status_from_errno(errno);
        goto cleanup;
    }

    status = sc_status_ok();

cleanup:
    if (path != nullptr) {
        sc_free(sc_allocator_heap(), path, strlen(path) + 1, _Alignof(char));
    }
    if (ruleset_fd >= 0 && close(ruleset_fd) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.sandbox.landlock.close_failed");
    }
    return status;
}

static uint64_t landlock_access_for_abi(uint32_t abi)
{
    uint64_t access = 0;

    if (abi == 0) {
        return 0;
    }

    access = (uint64_t)SC_LANDLOCK_FS_EXECUTE |
             (uint64_t)SC_LANDLOCK_FS_WRITE_FILE |
             (uint64_t)SC_LANDLOCK_FS_READ_FILE |
             (uint64_t)SC_LANDLOCK_FS_READ_DIR |
             (uint64_t)SC_LANDLOCK_FS_REMOVE_DIR |
             (uint64_t)SC_LANDLOCK_FS_REMOVE_FILE |
             (uint64_t)SC_LANDLOCK_FS_MAKE_CHAR |
             (uint64_t)SC_LANDLOCK_FS_MAKE_DIR |
             (uint64_t)SC_LANDLOCK_FS_MAKE_REG |
             (uint64_t)SC_LANDLOCK_FS_MAKE_SOCK |
             (uint64_t)SC_LANDLOCK_FS_MAKE_FIFO |
             (uint64_t)SC_LANDLOCK_FS_MAKE_BLOCK |
             (uint64_t)SC_LANDLOCK_FS_MAKE_SYM;

    if (abi >= 2) {
        access |= (uint64_t)SC_LANDLOCK_FS_REFER;
    }
    if (abi >= 3) {
        access |= (uint64_t)SC_LANDLOCK_FS_TRUNCATE;
    }

    return access;
}

static sc_status landlock_status_from_errno(int err)
{
    switch (err) {
    case E2BIG:
    case EINVAL:
        return sc_status_invalid_argument("sc.sandbox.landlock.invalid_argument");
    case ENOMSG:
    case ENOSYS:
    case EOPNOTSUPP:
        return sc_status_unsupported("sc.sandbox.landlock.unavailable");
    case ENOMEM:
        return sc_status_no_memory();
    case EACCES:
    case EPERM:
        return sc_status_security_denied("sc.sandbox.landlock.denied");
    default:
        return sc_status_io("sc.sandbox.landlock.io");
    }
}

static int landlock_create_ruleset_sys(const struct landlock_ruleset_attr *attr, size_t size, uint32_t flags)
{
    return (int)syscall(SYS_landlock_create_ruleset, attr, size, flags);
}

static int landlock_add_rule_sys(int ruleset_fd,
                                 enum landlock_rule_type type,
                                 const struct landlock_path_beneath_attr *attr,
                                 uint32_t flags)
{
    return (int)syscall(SYS_landlock_add_rule, ruleset_fd, type, attr, flags);
}

static int landlock_restrict_self_sys(int ruleset_fd, uint32_t flags)
{
    return (int)syscall(SYS_landlock_restrict_self, ruleset_fd, flags);
}

static sc_status validate_ruleset(const sc_landlock_ruleset *ruleset)
{
    if (ruleset == nullptr ||
        ruleset->struct_size < sizeof(*ruleset) ||
        ruleset->rules == nullptr ||
        ruleset->rule_count == 0) {
        return sc_status_invalid_argument("sc.sandbox.landlock.invalid_ruleset");
    }

    for (size_t i = 0; i < ruleset->rule_count; i += 1) {
        const sc_landlock_path_rule *rule = &ruleset->rules[i];

        if (rule->struct_size < sizeof(*rule) ||
            rule->path.ptr == nullptr ||
            rule->path.len == 0 ||
            rule->allowed_access == 0) {
            return sc_status_invalid_argument("sc.sandbox.landlock.invalid_rule");
        }
    }

    return sc_status_ok();
}

#else

uint32_t sc_landlock_abi_version()
{
    return 0;
}

uint64_t sc_landlock_supported_fs_access()
{
    return 0;
}

bool sc_landlock_available()
{
    return false;
}

sc_status sc_landlock_restrict_self(const sc_landlock_ruleset *ruleset)
{
    (void)ruleset;
    return sc_status_unsupported("sc.sandbox.landlock.unavailable");
}

#endif

sc_status sc_sandbox_landlock_new(sc_allocator *alloc, sc_sandbox **out)
{
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.invalid_argument");
    }
    // cppcheck-suppress knownConditionTrueFalse
    if (!sc_landlock_available()) {
        *out = nullptr;
        return sc_status_unsupported("sc.sandbox.landlock.unavailable");
    }
    return sc_sandbox_new(alloc, &landlock_vtab, nullptr, out);
}

static sc_status landlock_check(void *impl,
                                const sc_sandbox_request *request,
                                sc_allocator *alloc,
                                sc_sandbox_decision *out)
{
    (void)impl;
    if (out == nullptr || request == nullptr) {
        return sc_status_invalid_argument("sc.sandbox.invalid_argument");
    }
    // cppcheck-suppress knownConditionTrueFalse
    if (!sc_landlock_available()) {
        return sc_status_unsupported("sc.sandbox.landlock.unavailable");
    }
    sc_status status = sc_sandbox_decision_begin(out, alloc, SC_SANDBOX_BACKEND_LANDLOCK, "Linux Landlock sandbox plan");
    if (sc_status_is_ok(status)) {
        out->apply_landlock = true;
    }
    if (sc_status_is_ok(status) && request->executable.ptr != nullptr && request->executable.len > 0) {
        status = sc_sandbox_decision_add_original_command(out, alloc, request);
    }
    if (!sc_status_is_ok(status)) {
        sc_sandbox_decision_clear(out);
    }
    return status;
}

static void landlock_destroy(void *impl)
{
    (void)impl;
}
