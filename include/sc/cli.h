#pragma once

#include <stddef.h>
#include <stdio.h>

#include "sc/result.h"

typedef struct sc_cli_command sc_cli_command;

typedef enum sc_cli_parse_kind {
    SC_CLI_PARSE_COMMAND = 0,
    SC_CLI_PARSE_HELP,
    SC_CLI_PARSE_VERSION,
    SC_CLI_PARSE_FEATURES
} sc_cli_parse_kind;

struct sc_cli_command {
    const char *name;
    const char *summary;
    const sc_cli_command *children;
    size_t child_count;
};

typedef struct sc_cli_parse_result {
    sc_cli_parse_kind kind;
    const sc_cli_command *command;
    const char *error;
    int exit_code;
    bool hard_shutdown;
    const char *gateway_bind;
} sc_cli_parse_result;

sc_status sc_cli_parse(const sc_cli_command *root,
                       int argc,
                       char **argv,
                       sc_cli_parse_result *out);
void sc_cli_print_help(const sc_cli_command *command, const char *program_name, FILE *stream);
const sc_cli_command *sc_cli_default_root(void);
