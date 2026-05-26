#pragma once

#include "sc/result.h"
#include "sc/string.h"

int sc_test_expect_true(const char *label, bool condition);
int sc_test_expect_status(const char *label, sc_status status, sc_status_code expected);
int sc_test_expect_status_key(const char *label, sc_status status, sc_status_code expected, const char *error_key);

sc_status sc_test_make_temp_dir(const char *prefix, sc_string *out);
sc_status sc_test_path_join(sc_allocator *alloc, sc_str left, sc_str right, sc_string *out);
int sc_test_write_file(sc_str path, const char *text);
int sc_test_write_cstr_file(const char *path, const char *text);
sc_status sc_test_read_file(sc_allocator *alloc, sc_str path, sc_string *out);
