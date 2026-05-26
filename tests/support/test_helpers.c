#define _XOPEN_SOURCE 700

#include "test_helpers.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int sc_test_expect_true(const char *label, bool condition)
{
    if (!condition) {
        (void)fprintf(stderr, "%s: expected true\n", label);
        return 1;
    }
    return 0;
}

int sc_test_expect_status(const char *label, sc_status status, sc_status_code expected)
{
    if (status.code != expected) {
        (void)fprintf(stderr,
                      "%s: expected status %d, got %d key %s\n",
                      label,
                      expected,
                      status.code,
                      status.error_key == nullptr ? "" : status.error_key);
        sc_status_clear(&status);
        return 1;
    }
    sc_status_clear(&status);
    return 0;
}

int sc_test_expect_status_key(const char *label, sc_status status, sc_status_code expected, const char *error_key)
{
    bool matches = status.code == expected;

    if (matches && error_key != nullptr) {
        matches = status.error_key != nullptr && strcmp(status.error_key, error_key) == 0;
    }
    if (!matches) {
        (void)fprintf(stderr,
                      "%s: expected status %d key %s, got %d key %s\n",
                      label,
                      expected,
                      error_key == nullptr ? "" : error_key,
                      status.code,
                      status.error_key == nullptr ? "" : status.error_key);
        sc_status_clear(&status);
        return 1;
    }
    sc_status_clear(&status);
    return 0;
}

sc_status sc_test_make_temp_dir(const char *prefix, sc_string *out)
{
    char template[128] = {0};
    int written = 0;
    char *path = nullptr;

    if (prefix == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.test.temp_dir.invalid_argument");
    }
    written = snprintf(template, sizeof(template), "/tmp/%s-XXXXXX", prefix);
    if (written < 0 || (size_t)written >= sizeof(template)) {
        return sc_status_invalid_argument("sc.test.temp_dir.prefix_too_long");
    }
    path = mkdtemp(template);
    if (path == nullptr) {
        return sc_status_io("sc.test.temp_dir.mkdtemp_failed");
    }
    return sc_string_from_cstr(sc_allocator_heap(), path, out);
}

sc_status sc_test_path_join(sc_allocator *alloc, sc_str left, sc_str right, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    if (alloc == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.test.path_join.invalid_argument");
    }

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append(&builder, left);
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "/");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(&builder, right);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    sc_string_builder_clear(&builder);
    return status;
}

int sc_test_write_file(sc_str path, const char *text)
{
    char path_buf[4096] = {0};

    if (path.ptr == nullptr || path.len >= sizeof(path_buf)) {
        return -1;
    }
    (void)memcpy(path_buf, path.ptr, path.len);
    return sc_test_write_cstr_file(path_buf, text);
}

int sc_test_write_cstr_file(const char *path, const char *text)
{
    FILE *file = nullptr;
    size_t len = text == nullptr ? 0 : strlen(text);
    int rc = 0;

    if (path == nullptr) {
        return -1;
    }

    file = fopen(path, "wb");
    if (file == nullptr) {
        return -1;
    }
    if (len > 0 && fwrite(text, 1, len, file) != len) {
        rc = -1;
    }
    if (fclose(file) != 0) {
        rc = -1;
    }
    return rc;
}

sc_status sc_test_read_file(sc_allocator *alloc, sc_str path, sc_string *out)
{
    char path_buf[4096] = {0};
    FILE *file = nullptr;
    long size = 0;
    sc_string text = {0};

    if (alloc == nullptr || path.ptr == nullptr || out == nullptr || path.len >= sizeof(path_buf)) {
        return sc_status_invalid_argument("sc.test.read_file.invalid_argument");
    }
    (void)memcpy(path_buf, path.ptr, path.len);

    file = fopen(path_buf, "rb");
    if (file == nullptr) {
        return sc_status_io("sc.test.read_file.open_failed");
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        (void)fclose(file);
        return sc_status_io("sc.test.read_file.seek_failed");
    }
    size = ftell(file);
    if (size < 0) {
        (void)fclose(file);
        return sc_status_io("sc.test.read_file.tell_failed");
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        (void)fclose(file);
        return sc_status_io("sc.test.read_file.seek_failed");
    }
    text.ptr = sc_alloc(alloc, (size_t)size + 1, _Alignof(char));
    if (text.ptr == nullptr) {
        (void)fclose(file);
        return sc_status_no_memory();
    }
    text.len = (size_t)size;
    text.alloc = alloc;
    if (text.len > 0 && fread(text.ptr, 1, text.len, file) != text.len) {
        (void)fclose(file);
        sc_string_clear(&text);
        return sc_status_io("sc.test.read_file.read_failed");
    }
    text.ptr[text.len] = '\0';
    if (fclose(file) != 0) {
        sc_string_clear(&text);
        return sc_status_io("sc.test.read_file.close_failed");
    }

    *out = text;
    return sc_status_ok();
}
