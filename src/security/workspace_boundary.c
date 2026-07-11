#define _XOPEN_SOURCE 700

#include "security/security_internal.h"

#include <limits.h>
#include <fcntl.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static sc_status canonicalize_existing(sc_str input_path, sc_allocator *alloc, sc_string *out);
static sc_status canonicalize_for_write(sc_str input_path, sc_allocator *alloc, sc_string *out);
static sc_status make_absolute(sc_str input_path, sc_allocator *alloc, sc_string *out);
static sc_status parent_and_leaf(sc_allocator *alloc, sc_str path, sc_string *parent, sc_str *leaf);
static bool path_has_embedded_nul(sc_str path);

sc_status sc_workspace_open_parent(const sc_security_policy *policy,
                                   sc_str input_path,
                                   sc_allocator *alloc,
                                   int *out_parent_fd,
                                   sc_string *out_leaf)
{
    constexpr size_t max_segment_len = 255;
    sc_string resolved = {0};
    sc_string root = {0};
    sc_str relative = {0};
    size_t leaf_start = 0;
    size_t relative_offset = 0;
    int parent_fd = -1;
    sc_status status;

    if (policy == nullptr || out_parent_fd == nullptr || out_leaf == nullptr) {
        return sc_status_invalid_argument("sc.security.path_invalid_argument");
    }
    *out_parent_fd = -1;
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = sc_security_validate_path(policy, input_path, false);
    if (sc_status_is_ok(status)) {
        status = sc_workspace_resolve(policy, input_path, false, alloc, &resolved);
    }
    if (sc_status_is_ok(status)) {
        status = policy->workspace_only
                     ? canonicalize_existing(sc_string_as_str(&policy->workspace_root), alloc, &root)
                     : sc_string_from_cstr(alloc, "/", &root);
    }
    if (!sc_status_is_ok(status)) {
        goto cleanup;
    }
    relative_offset = root.len == 1U && root.ptr[0] == '/' ? 1U : root.len + 1U;
    if (!sc_security_path_has_prefix(sc_string_as_str(&resolved), sc_string_as_str(&root)) ||
        resolved.len <= relative_offset) {
        status = sc_status_security_denied("sc.security.path_escape");
        goto cleanup;
    }
    relative = sc_str_from_parts(resolved.ptr + relative_offset, resolved.len - relative_offset);
    for (size_t i = relative.len; i > 0; i -= 1) {
        if (relative.ptr[i - 1U] == '/') {
            leaf_start = i;
            break;
        }
    }
    status = sc_string_from_str(alloc,
                                sc_str_from_parts(relative.ptr + leaf_start, relative.len - leaf_start),
                                out_leaf);
    if (!sc_status_is_ok(status)) {
        goto cleanup;
    }
    parent_fd = open(root.ptr, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (parent_fd < 0) {
        status = sc_status_io("sc.security.workspace_open_failed");
        goto cleanup;
    }
    for (size_t start = 0; start < leaf_start;) {
        char segment[256] = {0};
        size_t end = start;
        size_t segment_len = 0;
        int next_fd = -1;

        while (end < leaf_start && relative.ptr[end] != '/') {
            end += 1;
        }
        segment_len = end - start;
        if (segment_len == 0 || segment_len > max_segment_len) {
            status = sc_status_security_denied("sc.security.path_escape");
            goto cleanup;
        }
        (void)memcpy(segment, relative.ptr + start, segment_len);
        next_fd = openat(parent_fd, segment, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (next_fd < 0) {
            status = errno == ELOOP || errno == ENOTDIR
                         ? sc_status_security_denied("sc.security.path_escape")
                         : sc_status_io("sc.security.path_open_failed");
            goto cleanup;
        }
        (void)close(parent_fd);
        parent_fd = next_fd;
        start = end + 1U;
    }
    *out_parent_fd = parent_fd;
    parent_fd = -1;

cleanup:
    if (parent_fd >= 0) {
        (void)close(parent_fd);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_clear(out_leaf);
    }
    sc_string_clear(&root);
    sc_string_clear(&resolved);
    return status;
}

sc_status sc_workspace_resolve(const sc_security_policy *policy,
                               sc_str input_path,
                               bool must_exist,
                               sc_allocator *alloc,
                               sc_string *out)
{
    sc_string resolved = {0};
    sc_string root = {0};
    sc_status status;

    if (policy == nullptr || out == nullptr || input_path.len == 0 || input_path.ptr == nullptr) {
        return sc_status_invalid_argument("sc.security.path_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;

    /*
     * Compare only canonical paths. A raw prefix check would let symlinks or
     * relative segments make an apparent child escape the workspace root.
     */
    if (policy->workspace_only || policy->workspace_root.len > 0) {
        status = canonicalize_existing(sc_string_as_str(&policy->workspace_root), alloc, &root);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    status = must_exist ? canonicalize_existing(input_path, alloc, &resolved)
                        : canonicalize_for_write(input_path, alloc, &resolved);
    if (sc_status_is_ok(status) && policy->workspace_only &&
        !sc_security_path_has_prefix(sc_string_as_str(&resolved), sc_string_as_str(&root))) {
        status = sc_status_security_denied("sc.security.path_escape");
    }
    sc_string_clear(&root);
    if (!sc_status_is_ok(status)) {
        sc_string_clear(&resolved);
        return status;
    }
    *out = resolved;
    return sc_status_ok();
}

sc_status sc_security_validate_path(const sc_security_policy *policy, sc_str input_path, bool must_exist)
{
    sc_string resolved = {0};
    sc_status status = sc_workspace_resolve(policy, input_path, must_exist, sc_allocator_heap(), &resolved);

    if (!sc_status_is_ok(status)) {
        return status;
    }
    for (size_t i = 0; i < policy->denied_paths.len; ++i) {
        const sc_string *item = sc_vec_at_const(&policy->denied_paths, i);
        sc_string denied = {0};
        status = canonicalize_for_write(sc_string_as_str(item), policy->alloc, &denied);
        if (sc_status_is_ok(status) &&
            sc_security_path_has_prefix(sc_string_as_str(&resolved), sc_string_as_str(&denied))) {
            status = sc_status_security_denied("sc.security.path_denied");
        }
        sc_string_clear(&denied);
        if (!sc_status_is_ok(status)) {
            sc_string_clear(&resolved);
            return status;
        }
    }
    if (policy->allowed_paths.len > 0) {
        bool allowed = false;
        for (size_t i = 0; i < policy->allowed_paths.len; ++i) {
            const sc_string *item = sc_vec_at_const(&policy->allowed_paths, i);
            sc_string allowed_path = {0};
            status = canonicalize_for_write(sc_string_as_str(item), policy->alloc, &allowed_path);
            if (sc_status_is_ok(status) &&
                sc_security_path_has_prefix(sc_string_as_str(&resolved), sc_string_as_str(&allowed_path))) {
                allowed = true;
            }
            sc_string_clear(&allowed_path);
            if (!sc_status_is_ok(status)) {
                sc_string_clear(&resolved);
                return status;
            }
        }
        if (!allowed) {
            status = sc_status_security_denied("sc.security.path_not_allowed");
        }
    }
    sc_string_clear(&resolved);
    return status;
}

static sc_status canonicalize_existing(sc_str input_path, sc_allocator *alloc, sc_string *out)
{
    char input[PATH_MAX] = {0};
    char resolved[PATH_MAX] = {0};

    if (input_path.len == 0 || input_path.ptr == nullptr || input_path.len >= sizeof(input) ||
        path_has_embedded_nul(input_path)) {
        return sc_status_invalid_argument("sc.security.path_invalid");
    }
    (void)memcpy(input, input_path.ptr, input_path.len);
    if (realpath(input, resolved) == nullptr) {
        return sc_status_io("sc.security.path_realpath_failed");
    }
    return sc_string_from_cstr(alloc, resolved, out);
}

static sc_status canonicalize_for_write(sc_str input_path, sc_allocator *alloc, sc_string *out)
{
    sc_string absolute = {0};
    sc_string parent = {0};
    sc_string real_parent = {0};
    sc_string_builder builder = {0};
    sc_str leaf = {0};
    sc_status status = make_absolute(input_path, alloc, &absolute);

    if (!sc_status_is_ok(status)) {
        return status;
    }
    /*
     * realpath cannot resolve a file that does not exist yet, so resolve the
     * existing parent and append the requested leaf after traversal checks.
     */
    status = parent_and_leaf(alloc, sc_string_as_str(&absolute), &parent, &leaf);
    if (sc_status_is_ok(status)) {
        status = canonicalize_existing(sc_string_as_str(&parent), alloc, &real_parent);
    }
    /* The leaf must name a concrete child; "." or ".." would undo the parent canonicalization. */
    if (sc_status_is_ok(status) && (leaf.len == 0 || sc_str_equal(leaf, sc_str_from_cstr(".")) ||
                                   sc_str_equal(leaf, sc_str_from_cstr("..")))) {
        status = sc_status_security_denied("sc.security.path_traversal");
    }
    if (sc_status_is_ok(status)) {
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append(&builder, sc_string_as_str(&real_parent));
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "/");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append(&builder, leaf);
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, out);
        } else {
            sc_string_builder_clear(&builder);
        }
    }
    sc_string_clear(&absolute);
    sc_string_clear(&parent);
    sc_string_clear(&real_parent);
    return status;
}

static sc_status make_absolute(sc_str input_path, sc_allocator *alloc, sc_string *out)
{
    char cwd[PATH_MAX] = {0};
    sc_string_builder builder = {0};
    sc_status status;

    if (input_path.len == 0 || input_path.ptr == nullptr || path_has_embedded_nul(input_path)) {
        return sc_status_invalid_argument("sc.security.path_invalid");
    }
    if (input_path.ptr[0] == '/') {
        return sc_string_from_str(alloc, input_path, out);
    }
    if (input_path.ptr[0] == '~' && (input_path.len == 1 || input_path.ptr[1] == '/')) {
        const char *home = getenv("HOME");
        if (home == nullptr || home[0] == '\0') {
            return sc_status_invalid_argument("sc.security.path_invalid");
        }
        sc_string_builder_init(&builder, alloc);
        status = sc_string_builder_append_cstr(&builder, home);
        if (sc_status_is_ok(status) && input_path.len > 1) {
            status = sc_string_builder_append(&builder, sc_str_from_parts(&input_path.ptr[1], input_path.len - 1));
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_finish(&builder, out);
        } else {
            sc_string_builder_clear(&builder);
        }
        return status;
    }
    if (getcwd(cwd, sizeof(cwd)) == nullptr) {
        return sc_status_io("sc.security.getcwd_failed");
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, cwd);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, input_path);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status parent_and_leaf(sc_allocator *alloc, sc_str path, sc_string *parent, sc_str *leaf)
{
    size_t slash = SIZE_MAX;

    for (size_t i = path.len; i > 0; --i) {
        if (path.ptr[i - 1] == '/') {
            slash = i - 1;
            break;
        }
    }
    if (slash == SIZE_MAX || slash + 1 >= path.len) {
        return sc_status_invalid_argument("sc.security.path_invalid");
    }
    *leaf = sc_str_from_parts(&path.ptr[slash + 1], path.len - slash - 1);
    return slash == 0 ? sc_string_from_cstr(alloc, "/", parent) :
                        sc_string_from_str(alloc, sc_str_from_parts(path.ptr, slash), parent);
}

static bool path_has_embedded_nul(sc_str path)
{
    return path.ptr != nullptr && memchr(path.ptr, '\0', path.len) != nullptr;
}
