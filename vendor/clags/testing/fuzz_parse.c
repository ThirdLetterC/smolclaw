#include <ctype.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "clags/clags.h"

enum {
  CLAGS_FUZZ_MAX_INPUT = 4'096,
  CLAGS_FUZZ_MAX_ARGV = 64,
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  if (data == nullptr) {
    return 0;
  }

  if (size > CLAGS_FUZZ_MAX_INPUT) {
    size = CLAGS_FUZZ_MAX_INPUT;
  }

  char *buffer = calloc(size + 1, sizeof(*buffer));
  if (buffer == nullptr) {
    return 0;
  }

  for (size_t i = 0; i < size; ++i) {
    unsigned char byte = data[i];
    buffer[i] = isspace(byte) || byte == '\0' ? ' ' : (char)byte;
  }

  char *argv[CLAGS_FUZZ_MAX_ARGV] = {"prog"};
  int argc = 1;
  bool in_token = false;
  for (size_t i = 0; i < size && argc < CLAGS_FUZZ_MAX_ARGV; ++i) {
    if (buffer[i] == ' ') {
      buffer[i] = '\0';
      in_token = false;
      continue;
    }
    if (!in_token) {
      argv[argc++] = &buffer[i];
      in_token = true;
    }
  }

  bool help = false;
  int32_t count = 0;
  uint64_t limit = 0;
  clags_time_t delay = 0;
  clags_list_t extras = clags_string_list();

  clags_choice_t modes[] = {
      {.value = "fast", .description = ""},
      {.value = "safe", .description = ""},
  };
  clags_choices_t mode_choices =
      clags_choices(modes, .case_insensitive = true, .print_no_details = true);
  clags_choice_t *mode = nullptr;

  clags_arg_t child_args[] = {
      clags_option('c', "count", &count, "COUNT", "parsed integer value",
                   .value_type = Clags_Int32),
      clags_option('l', "limit", &limit, "LIMIT", "parsed unsigned value",
                   .value_type = Clags_UInt64),
      clags_option('d', "delay", &delay, "DELAY", "parsed time value",
                   .value_type = Clags_TimeNS),
      clags_option('m', "mode", &mode, "MODE", "parsed choice value",
                   .value_type = Clags_Choice, .choices = &mode_choices),
      clags_positional(&extras, "extras", "extra string values", .is_list = true),
      clags_flag_help(&help),
  };

  clags_config_t child_config =
      clags_config(child_args, .duplicate_strings = true,
                   .allow_option_parsing_toggle = true,
                   .ignore_prefix = "!", .list_terminator = "::",
                   .min_log_level = Clags_NoLogs);

  clags_subcmd_t subcommands[] = {
      {.name = "run", .description = "run parser", .config = &child_config},
      {.name = "check", .description = "check parser", .config = &child_config},
  };
  clags_subcmds_t subcmds = clags_subcmds(subcommands);
  clags_subcmd_t *selected_subcmd = nullptr;

  clags_arg_t root_args[] = {
      clags_positional(&selected_subcmd, "command", "subcommand to execute",
                       .value_type = Clags_Subcmd, .subcmds = &subcmds),
      clags_flag_help(&help),
  };

  clags_config_t root_config =
      clags_config(root_args, .duplicate_strings = true,
                   .min_log_level = Clags_NoLogs);

  (void)clags_parse(argc, argv, &root_config);

  clags_config_free(&child_config);
  clags_config_free(&root_config);
  free(buffer);
  return 0;
}
