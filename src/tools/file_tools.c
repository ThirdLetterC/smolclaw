#include "tools/tool_internal.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef enum file_tool_kind {
    FILE_TOOL_WRITE = 0,
    FILE_TOOL_LIST
} file_tool_kind;

typedef struct file_tool {
    sc_tool_impl_context base;
    file_tool_kind kind;
} file_tool;

static sc_status file_tool_spec(void *impl, sc_tool_spec *out);
static sc_status file_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void file_tool_destroy(void *impl);
static sc_status file_tool_new(sc_allocator *alloc,
                               const sc_tool_context *context,
                               file_tool_kind kind,
                               sc_tool **out);
static sc_status invoke_file_write(file_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status invoke_file_list(file_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static sc_status write_all_at(int parent_fd, sc_str leaf, sc_str content, bool append);
static sc_status write_all_fd(int fd, sc_str content);
static sc_status write_atomic_at(sc_allocator *alloc,
                                 int parent_fd,
                                 sc_str leaf,
                                 sc_str content,
                                 bool fail_if_exists);
static bool name_matches(sc_str name, sc_str pattern);

static const sc_tool_vtab file_write_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "file_write",
    .display_name = "File write",
    .feature_flag = "SC_TOOL_FILE_WRITE",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = file_tool_spec,
    .invoke = file_tool_invoke,
    .destroy = file_tool_destroy,
};

static const sc_tool_vtab file_list_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "file_list",
    .display_name = "File list",
    .feature_flag = "SC_TOOL_FILE_LIST",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = file_tool_spec,
    .invoke = file_tool_invoke,
    .destroy = file_tool_destroy,
};

sc_status sc_tool_file_write_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return file_tool_new(alloc, context, FILE_TOOL_WRITE, out);
}

sc_status sc_tool_file_list_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out)
{
    return file_tool_new(alloc, context, FILE_TOOL_LIST, out);
}

static sc_status file_tool_spec(void *impl, sc_tool_spec *out)
{
    const file_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.file_tool.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = tool->kind == FILE_TOOL_WRITE ? sc_str_from_cstr("file_write") : sc_str_from_cstr("file_list"),
        .description = tool->kind == FILE_TOOL_WRITE ? sc_str_from_cstr("tool.file_write.description") :
                                                        sc_str_from_cstr("tool.file_list.description"),
        .input_schema = tool->base.schema,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = tool->kind == FILE_TOOL_WRITE ? SC_TOOL_RISK_SIDE_EFFECT : SC_TOOL_RISK_READONLY,
        .output_schema = tool->base.output_schema,
        .capability_category = SC_TOOL_CAPABILITY_FILESYSTEM,
        .side_effect = tool->kind == FILE_TOOL_WRITE ? SC_TOOL_SIDE_EFFECT_WRITE : SC_TOOL_SIDE_EFFECT_READ,
        .default_autonomy = tool->kind == FILE_TOOL_WRITE ? SC_AUTONOMY_SUPERVISED : SC_AUTONOMY_AUTONOMOUS,
        .catalog_metadata_key = tool->kind == FILE_TOOL_WRITE ? sc_str_from_cstr("tool.file_write.catalog") :
                                                                sc_str_from_cstr("tool.file_list.catalog"),
    };
    return sc_status_ok();
}

static sc_status file_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    file_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.file_tool.invalid_argument");
    }
    return tool->kind == FILE_TOOL_WRITE ? invoke_file_write(tool, call, alloc, out) :
                                           invoke_file_list(tool, call, alloc, out);
}

static void file_tool_destroy(void *impl)
{
    file_tool *tool = impl;
    sc_allocator *alloc = nullptr;
    if (tool == nullptr) {
        return;
    }
    alloc = tool->base.alloc;
    sc_tool_impl_context_clear(&tool->base);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(file_tool));
}

static sc_status file_tool_new(sc_allocator *alloc,
                               const sc_tool_context *context,
                               file_tool_kind kind,
                               sc_tool **out)
{
    file_tool *tool = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.file_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    tool = sc_alloc(alloc, sizeof(*tool), _Alignof(file_tool));
    if (tool == nullptr) {
        return sc_status_no_memory();
    }
    *tool = (file_tool){.kind = kind};
    status = sc_tool_context_copy(alloc, context, &tool->base);
    if (sc_status_is_ok(status) && kind == FILE_TOOL_WRITE) {
        status = sc_tool_schema_three_strings(alloc,
                                              sc_str_from_cstr("path"),
                                              true,
                                              sc_str_from_cstr("content"),
                                              true,
                                              sc_str_from_cstr("mode"),
                                              false,
                                              &tool->base.schema);
    } else if (sc_status_is_ok(status)) {
        status = sc_tool_schema_two_strings(alloc,
                                            sc_str_from_cstr("path"),
                                            true,
                                            sc_str_from_cstr("pattern"),
                                            false,
                                            &tool->base.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_output_schema_text(alloc, &tool->base.output_schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, kind == FILE_TOOL_WRITE ? &file_write_vtab : &file_list_vtab, tool, out);
    }
    if (!sc_status_is_ok(status)) {
        file_tool_destroy(tool);
    }
    return status;
}

static sc_status invoke_file_write(file_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str path = {0};
    sc_str content = {0};
    sc_str mode = sc_str_from_cstr("overwrite");
    sc_str otp = {0};
    sc_string leaf = {0};
    int parent_fd = -1;
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("path"), &path);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("content"), &content);
    }
    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("mode"), &mode);
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("otp"), &otp);
        if (mode.len == 0) {
            mode = sc_str_from_cstr("overwrite");
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check_ex(&tool->base,
                                           sc_str_from_cstr("file_write"),
                                           SC_TOOL_RISK_SIDE_EFFECT,
                                           path,
                                           false,
                                           sc_str_from_cstr(""),
                                           sc_str_from_cstr(""),
                                           sc_str_from_cstr(""),
                                           otp);
    }
    if (sc_status_is_ok(status)) {
        status = sc_workspace_open_parent(tool->base.context.policy, path, alloc, &parent_fd, &leaf);
    }
    if (sc_status_is_ok(status) && sc_str_equal(mode, sc_str_from_cstr("append"))) {
        status = write_all_at(parent_fd, sc_string_as_str(&leaf), content, true);
    } else if (sc_status_is_ok(status) && sc_str_equal(mode, sc_str_from_cstr("create"))) {
        status = write_atomic_at(alloc, parent_fd, sc_string_as_str(&leaf), content, true);
    } else if (sc_status_is_ok(status) && sc_str_equal(mode, sc_str_from_cstr("overwrite"))) {
        status = write_atomic_at(alloc, parent_fd, sc_string_as_str(&leaf), content, false);
    } else if (sc_status_is_ok(status)) {
        status = sc_status_invalid_argument("sc.file_write.invalid_mode");
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc, &tool->base, out, sc_str_from_cstr("written"), true);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("file_write"), path, sc_str_from_cstr("written"), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("file_write"),
                                            path,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    if (parent_fd >= 0) {
        (void)close(parent_fd);
    }
    sc_string_clear(&leaf);
    return status;
}

static sc_status invoke_file_list(file_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_str path = {0};
    sc_str pattern = {0};
    sc_string resolved = {0};
    sc_string_builder builder = {0};
    DIR *dir = nullptr;
    sc_status status = sc_tool_check_cancelled(&tool->base, call);

    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("path"), &path);
    }
    if (sc_status_is_ok(status)) {
        (void)sc_tool_get_optional_string_arg(call, sc_str_from_cstr("pattern"), &pattern);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check(&tool->base,
                                        sc_str_from_cstr("file_list"),
                                        SC_TOOL_RISK_READONLY,
                                        path,
                                        true,
                                        sc_str_from_cstr(""),
                                        sc_str_from_cstr(""));
    }
    if (sc_status_is_ok(status)) {
        status = sc_workspace_resolve(tool->base.context.policy, path, true, alloc, &resolved);
    }
    if (sc_status_is_ok(status)) {
        dir = opendir(resolved.ptr);
        if (dir == nullptr) {
            status = sc_status_io("sc.file_list.open_failed");
        }
    }
    sc_string_builder_init(&builder, alloc);
    while (sc_status_is_ok(status)) {
        struct dirent *entry = readdir(dir);
        if (entry == nullptr) {
            break;
        }
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0 ||
            !name_matches(sc_str_from_cstr(entry->d_name), pattern)) {
            continue;
        }
        status = sc_string_builder_append_cstr(&builder, entry->d_name);
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(&builder, "\n");
        }
    }
    if (dir != nullptr && closedir(dir) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.file_list.close_failed");
    }
    if (sc_status_is_ok(status)) {
        sc_string listing = {0};
        status = sc_string_builder_finish(&builder, &listing);
        if (sc_status_is_ok(status)) {
            status = sc_tool_set_output(alloc, &tool->base, out, sc_string_as_str(&listing), true);
            sc_string_clear(&listing);
        }
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_record_receipt(&tool->base, sc_str_from_cstr("file_list"), path, sc_string_as_str(&out->output), true);
    } else {
        (void)sc_tool_record_receipt_status(&tool->base,
                                            sc_str_from_cstr("file_list"),
                                            path,
                                            sc_str_from_cstr("error"),
                                            false,
                                            status);
    }
    sc_string_clear(&resolved);
    return status;
}

static sc_status write_all_at(int parent_fd, sc_str leaf, sc_str content, bool append)
{
    int flags = O_WRONLY | O_CREAT | O_CLOEXEC | O_NOFOLLOW | (append ? O_APPEND : O_TRUNC);
    int fd = -1;
    struct stat st = {0};

    if (parent_fd < 0 || leaf.ptr == nullptr || leaf.len == 0) {
        return sc_status_invalid_argument("sc.file_write.path_invalid");
    }
    fd = openat(parent_fd, leaf.ptr, flags, 0600);
    if (fd < 0) {
        return errno == ELOOP ? sc_status_security_denied("sc.file_write.symlink_denied")
                              : sc_status_io("sc.file_write.open_failed");
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        (void)close(fd);
        return sc_status_security_denied("sc.file_write.non_regular_denied");
    }
    sc_status status = write_all_fd(fd, content);
    if (close(fd) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.file_write.close_failed");
    }
    return status;
}

static sc_status write_atomic_at(sc_allocator *alloc,
                                 int parent_fd,
                                 sc_str leaf,
                                 sc_str content,
                                 bool fail_if_exists)
{
    sc_string_builder builder = {0};
    sc_string tmp = {0};
    sc_status status;
    int fd = -1;
    char suffix[64] = {0};
    int written = 0;

    written = snprintf(suffix, sizeof(suffix), ".tmp.%ld", (long)getpid());
    if (written <= 0 || (size_t)written >= sizeof(suffix)) {
        return sc_status_invalid_argument("sc.file_write.temp_name_failed");
    }
    sc_string_builder_init(&builder, alloc);
    if (parent_fd < 0 || leaf.ptr == nullptr || leaf.len == 0) {
        return sc_status_invalid_argument("sc.file_write.path_invalid");
    }
    status = sc_string_builder_append(&builder, leaf);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, suffix);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, &tmp);
    } else {
        sc_string_builder_clear(&builder);
    }
    if (sc_status_is_ok(status)) {
        int flags = O_WRONLY | O_CREAT | O_EXCL;
#ifdef O_NOFOLLOW
        flags |= O_NOFOLLOW;
#endif
        fd = openat(parent_fd, tmp.ptr, flags | O_CLOEXEC, 0600);
        if (fd < 0) {
            status = errno == EEXIST ? sc_status_invalid_argument("sc.file_write.temp_exists") :
                                       sc_status_io("sc.file_write.temp_open_failed");
        }
    }
    if (sc_status_is_ok(status)) {
        status = write_all_fd(fd, content);
        if (close(fd) != 0 && sc_status_is_ok(status)) {
            status = sc_status_io("sc.file_write.close_failed");
        }
        fd = -1;
    }
    if (sc_status_is_ok(status) && fail_if_exists) {
        if (linkat(parent_fd, tmp.ptr, parent_fd, leaf.ptr, 0) != 0) {
            status = errno == EEXIST ? sc_status_invalid_argument("sc.file_write.exists") :
                                       sc_status_io("sc.file_write.link_failed");
        }
        (void)unlinkat(parent_fd, tmp.ptr, 0);
    } else if (sc_status_is_ok(status) && renameat(parent_fd, tmp.ptr, parent_fd, leaf.ptr) != 0) {
        status = sc_status_io("sc.file_write.rename_failed");
    }
    if (!sc_status_is_ok(status) && tmp.ptr != nullptr) {
        if (fd >= 0) {
            (void)close(fd);
        }
        (void)unlinkat(parent_fd, tmp.ptr, 0);
    }
    sc_string_clear(&tmp);
    return status;
}

static sc_status write_all_fd(int fd, sc_str content)
{
    size_t offset = 0;

    while (offset < content.len) {
        ssize_t written = write(fd, content.ptr + offset, content.len - offset);
        if (written < 0) {
            if (errno == EINTR) {
                continue;
            }
            return sc_status_io("sc.file_write.write_failed");
        }
        if (written == 0) {
            return sc_status_io("sc.file_write.write_incomplete");
        }
        offset += (size_t)written;
    }
    return sc_status_ok();
}

static bool name_matches(sc_str name, sc_str pattern)
{
    if (pattern.ptr == nullptr || pattern.len == 0 || sc_str_equal(pattern, sc_str_from_cstr("*"))) {
        return true;
    }
    if (name.len < pattern.len) {
        return false;
    }
    for (size_t i = 0; i + pattern.len <= name.len; i += 1) {
        if (memcmp(&name.ptr[i], pattern.ptr, pattern.len) == 0) {
            return true;
        }
    }
    return false;
}
