#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "clags/clags.h"

clags_options_t global_options = {
    .min_log_level = Clags_NoLogs,
};

// 1. Integer option
void test_int_option() {
  int num = 0;

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Option,
                               .opt = {.long_flag = "num",
                                       .value_type = Clags_Int32,
                                       .variable = &num}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "--num", "123"};
  int argc = 3;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == nullptr);
  assert(num == 123);
  assert(config.error == Clags_Error_Ok);
}

// 2. Float option
void test_float_option() {
  double fval = 0.0f;

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Option,
                               .opt = {.long_flag = "value",
                                       .value_type = Clags_Double,
                                       .variable = &fval}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "--value", "3.14"};
  int argc = 3;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == nullptr);
  assert(fabs(fval - 3.14) < 1e-6);
  assert(config.error == Clags_Error_Ok);
}

// 3. Boolean option
void test_bool_option() {
  bool flag = false;

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Option,
                               .opt = {.long_flag = "enable",
                                       .value_type = Clags_Bool,
                                       .variable = &flag}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "--enable", "yes"};
  int argc = 3;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == nullptr);
  assert(flag == true);
  assert(config.error == Clags_Error_Ok);
}

// 4. Short flag
void test_short_flag() {
  bool verbose = false;

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Flag,
                               .flag = {.short_flag = 'v',
                                        .type = Clags_BoolFlag,
                                        .variable = &verbose}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "-v"};
  int argc = 2;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == nullptr);
  assert(verbose == true);
  assert(config.error == Clags_Error_Ok);
}

// 5. Positional argument
void test_positional() {
  char *file = nullptr;

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Positional,
                               .pos = {.arg_name = "file",
                                       .value_type = Clags_String,
                                       .variable = &file,
                                       .optional = false}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "input.txt"};
  int argc = 2;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == nullptr);
  assert(file && strcmp(file, "input.txt") == 0);
  assert(config.error == Clags_Error_Ok);
}

// 6. Positional list
void test_positional_list() {
  clags_list_t files = clags_list();

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Positional,
                               .pos = {.arg_name = "files",
                                       .value_type = Clags_String,
                                       .variable = &files,
                                       .is_list = true}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "a.txt", "b.txt", "c.txt"};
  int argc = 4;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == nullptr);
  assert(files.count == 3);
  assert(strcmp(((char **)files.items)[0], "a.txt") == 0);
  assert(strcmp(((char **)files.items)[2], "c.txt") == 0);
  assert(config.error == Clags_Error_Ok);

  clags_config_free(&config);
}

// 7. Invalid value
void test_invalid_value() {
  int num = 0;

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Option,
                               .opt = {.long_flag = "num",
                                       .value_type = Clags_Int32,
                                       .variable = &num}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "--num", "abc"};
  int argc = 3;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == &config);
  assert(config.error == Clags_Error_InvalidValue);
}

// 8. Subcommand
void test_subcommand() {
  clags_config_t init_config = {0};
  clags_subcmd_t subcmds[] = {{.name = "init", .config = &init_config}};
  clags_subcmds_t list = {.items = subcmds, .count = 1};

  clags_config_t config = {
      .args =
          (clags_arg_t[]){{.type = Clags_Positional,
                           .pos = {.arg_name = "command",
                                   .value_type = Clags_Subcmd,
                                   .subcmds = &list,
                                   .variable = &(clags_subcmd_t *){nullptr}}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "init"};
  int argc = 2;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == nullptr);
  assert(config.error == Clags_Error_Ok);
}

// 9. Unsigned option rejects signed value with leading whitespace
void test_uint64_rejects_signed_whitespace() {
  uint64_t size = 0;

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Option,
                               .opt = {.long_flag = "size",
                                       .value_type = Clags_UInt64,
                                       .variable = &size}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "--size", " -1"};
  int argc = 3;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == &config);
  assert(config.error == Clags_Error_InvalidValue);
  assert(size == 0);
}

// 10. Empty integer values are invalid
void test_empty_integer_rejected() {
  int32_t num = 0;

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Option,
                               .opt = {.long_flag = "num",
                                       .value_type = Clags_Int32,
                                       .variable = &num}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "--num", ""};
  int argc = 3;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == &config);
  assert(config.error == Clags_Error_InvalidValue);
  assert(num == 0);
}

// 11. Non-finite time values are invalid
void test_time_nan_rejected() {
  clags_time_t duration = 0;

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Option,
                               .opt = {.long_flag = "duration",
                                       .value_type = Clags_TimeNS,
                                       .variable = &duration}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "--duration", "nan"};
  int argc = 3;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == &config);
  assert(config.error == Clags_Error_InvalidValue);
  assert(duration == 0);
}

// 12. Misconfigured list item sizes fail safely
void test_list_item_size_mismatch_rejected() {
  clags_list_t ints = clags_custom_list(sizeof(uint8_t));

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Positional,
                               .pos = {.arg_name = "ints",
                                       .value_type = Clags_Int32,
                                       .variable = &ints,
                                       .is_list = true}}},
      .args_count = 1,
      .options = global_options,
  };

  char *argv[] = {"prog", "1"};
  int argc = 2;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == &config);
  assert(config.error == Clags_Error_InvalidValue);
  assert(ints.count == 0);
  clags_list_free(&ints);
}

// 13. Empty ignore prefixes are rejected as invalid config
void test_empty_ignore_prefix_rejected() {
  char *value = nullptr;

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Positional,
                               .pos = {.arg_name = "value",
                                       .value_type = Clags_String,
                                       .variable = &value}}},
      .args_count = 1,
      .options =
          {
              .ignore_prefix = "",
              .min_log_level = Clags_NoLogs,
          },
  };

  char *argv[] = {"prog", "value"};
  int argc = 2;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == &config);
  assert(config.error == Clags_Error_InvalidConfig);
  assert(value == nullptr);
}

// 14. Empty list terminators are rejected as invalid config
void test_empty_list_terminator_rejected() {
  clags_list_t values = clags_list();

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Positional,
                               .pos = {.arg_name = "values",
                                       .value_type = Clags_String,
                                       .variable = &values,
                                       .is_list = true}}},
      .args_count = 1,
      .options =
          {
              .list_terminator = "",
              .min_log_level = Clags_NoLogs,
          },
  };

  char *argv[] = {"prog", "value"};
  int argc = 2;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == &config);
  assert(config.error == Clags_Error_InvalidConfig);
  assert(values.count == 0);
  clags_list_free(&values);
}

// 15. Recursive config cycles are rejected before descent
void test_subcommand_cycle_rejected() {
  clags_subcmd_t *root_selected = nullptr;
  clags_subcmd_t *child_selected = nullptr;

  clags_subcmd_t root_subcmds[1] = {0};
  clags_subcmd_t child_subcmds[1] = {0};
  clags_subcmds_t root_subcmd_list = {.items = root_subcmds, .count = 1};
  clags_subcmds_t child_subcmd_list = {.items = child_subcmds, .count = 1};

  clags_config_t root_config = {
      .args =
          (clags_arg_t[]){{.type = Clags_Positional,
                           .pos = {.arg_name = "root",
                                   .value_type = Clags_Subcmd,
                                   .subcmds = &root_subcmd_list,
                                   .variable = &root_selected}}},
      .args_count = 1,
      .options = global_options,
  };
  clags_config_t child_config = {
      .args =
          (clags_arg_t[]){{.type = Clags_Positional,
                           .pos = {.arg_name = "child",
                                   .value_type = Clags_Subcmd,
                                   .subcmds = &child_subcmd_list,
                                   .variable = &child_selected}}},
      .args_count = 1,
      .options = global_options,
  };

  root_subcmds[0] = (clags_subcmd_t){.name = "child", .config = &child_config};
  child_subcmds[0] = (clags_subcmd_t){.name = "root", .config = &root_config};

  char *argv[] = {"prog", "child", "root"};
  int argc = 3;

  clags_config_t *parse_result = clags_parse(argc, argv, &root_config);
  assert(parse_result == &child_config);
  assert(child_config.error == Clags_Error_InvalidOption);
}

// 16. Freeing duplicated strings clears internal owned config names
void test_duplicate_string_cleanup_clears_name() {
  char *value = nullptr;

  clags_config_t config = {
      .args = (clags_arg_t[]){{.type = Clags_Positional,
                               .pos = {.arg_name = "value",
                                       .value_type = Clags_String,
                                       .variable = &value}}},
      .args_count = 1,
      .options =
          {
              .duplicate_strings = true,
              .min_log_level = Clags_NoLogs,
          },
  };

  char *argv[] = {"prog", "value"};
  int argc = 2;

  clags_config_t *parse_result = clags_parse(argc, argv, &config);
  assert(parse_result == nullptr);
  assert(config.name != nullptr);
  clags_config_free_allocs(&config);
  assert(config.name == nullptr);
}

int main() {
  test_int_option();
  printf("- Test 'int option' passed!\n");
  test_float_option();
  printf("- Test 'float option' passed!\n");
  test_bool_option();
  printf("- Test 'bool option' passed!\n");
  test_short_flag();
  printf("- Test 'short flag' passed!\n");
  test_positional();
  printf("- Test 'positional' passed!\n");
  test_positional_list();
  printf("- Test 'positional list' passed!\n");
  test_invalid_value();
  printf("- Test 'invalid value' passed!\n");
  test_subcommand();
  printf("- Test 'subcommand' passed!\n");
  test_uint64_rejects_signed_whitespace();
  printf("- Test 'unsigned whitespace sign rejection' passed!\n");
  test_empty_integer_rejected();
  printf("- Test 'empty integer rejection' passed!\n");
  test_time_nan_rejected();
  printf("- Test 'time nan rejection' passed!\n");
  test_list_item_size_mismatch_rejected();
  printf("- Test 'list item-size mismatch rejection' passed!\n");
  test_empty_ignore_prefix_rejected();
  printf("- Test 'empty ignore-prefix rejection' passed!\n");
  test_empty_list_terminator_rejected();
  printf("- Test 'empty list-terminator rejection' passed!\n");
  test_subcommand_cycle_rejected();
  printf("- Test 'subcommand cycle rejection' passed!\n");
  test_duplicate_string_cleanup_clears_name();
  printf("- Test 'duplicate-string cleanup name reset' passed!\n");

  printf("\nAll tests passed!\n");
  return 0;
}
