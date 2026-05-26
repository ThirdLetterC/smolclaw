#pragma once

#include <stdio.h>

#include "sc/i18n.h"
#include "sc/result.h"
#include "sc/string.h"

sc_string sc_app_format_key(sc_i18n_catalog *catalog,
                            const char *key,
                            const sc_i18n_arg *args,
                            size_t arg_count);
bool sc_app_file_exists(const char *path);
bool sc_app_env_truthy(const char *value);
sc_status sc_app_copy_cstr_to_buffer(char *out, size_t capacity, const char *value, const char *error_key);
sc_status sc_app_join_cstr_path(char *out,
                                size_t capacity,
                                const char *left,
                                const char *right,
                                const char *error_key);
sc_status sc_app_cli_workspace_path(char *out, size_t capacity, const char *config_path);
sc_status sc_app_cli_estop_paths(char *state_dir,
                                 size_t state_dir_capacity,
                                 char *state_path,
                                 size_t state_path_capacity);
sc_status sc_app_ensure_cli_dir(const char *path);
sc_status sc_app_read_text_file(sc_allocator *alloc, const char *path, sc_string *out);
sc_status sc_app_write_text_file(const char *path,
                                 sc_str body,
                                 const char *open_error_key,
                                 const char *write_error_key,
                                 const char *close_error_key);
