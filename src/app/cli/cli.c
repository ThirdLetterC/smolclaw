#include "sc/cli.h"

#include <stdarg.h>
#include <string.h>

#include "sc/api.h"
#include "sc/i18n.h"

#ifdef SC_HAVE_CLAGS
#include "clags/clags.h"
#endif

static const sc_cli_command config_preset_children[] = {
    {.name = "default-posture", .summary = "cli.config.preset.default_posture.summary"},
    {.name = "yolo", .summary = "cli.config.preset.yolo.summary"},
};

static const sc_cli_command config_children[] = {
    {.name = "show", .summary = "cli.config.show.summary"},
    {.name = "set", .summary = "cli.config.set.summary"},
    {.name = "preset", .summary = "cli.config.preset.summary", .children = config_preset_children, .child_count = SC_ARRAY_LEN(config_preset_children)},
};

static const sc_cli_command gateway_children[] = {
    {.name = "serve", .summary = "cli.gateway.serve.summary"},
};

static const sc_cli_command cron_children[] = {
    {.name = "add", .summary = "cli.cron.add.summary"},
    {.name = "list", .summary = "cli.cron.list.summary"},
    {.name = "remove", .summary = "cli.cron.remove.summary"},
};

static const sc_cli_command service_children[] = {
    {.name = "status", .summary = "cli.service.status.summary"},
    {.name = "dry-run", .summary = "cli.service.dry_run.summary"},
    {.name = "install", .summary = "cli.service.install.summary"},
    {.name = "uninstall", .summary = "cli.service.uninstall.summary"},
    {.name = "start", .summary = "cli.service.start.summary"},
    {.name = "stop", .summary = "cli.service.stop.summary"},
    {.name = "restart", .summary = "cli.service.restart.summary"},
};

static const sc_cli_command estop_children[] = {
    {.name = "status", .summary = "cli.estop.status.summary"},
    {.name = "trip", .summary = "cli.estop.trip.summary"},
    {.name = "reset", .summary = "cli.estop.reset.summary"},
};

static const sc_cli_command root_children[] = {
    {.name = "acp", .summary = "cli.acp.summary"},
    {.name = "chat", .summary = "cli.chat.summary"},
    {.name = "daemon", .summary = "cli.daemon.summary"},
    {.name = "init-config", .summary = "cli.init_config.summary"},
    {.name = "onboard", .summary = "cli.onboard.summary"},
    {.name = "config", .summary = "cli.config.summary", .children = config_children, .child_count = SC_ARRAY_LEN(config_children)},
    {.name = "provider", .summary = "cli.provider.summary"},
    {.name = "memory", .summary = "cli.memory.summary"},
    {.name = "cron", .summary = "cli.cron.summary", .children = cron_children, .child_count = SC_ARRAY_LEN(cron_children)},
    {.name = "gateway", .summary = "cli.gateway.summary", .children = gateway_children, .child_count = SC_ARRAY_LEN(gateway_children)},
    {.name = "service", .summary = "cli.service.summary", .children = service_children, .child_count = SC_ARRAY_LEN(service_children)},
    {.name = "estop", .summary = "cli.estop.summary", .children = estop_children, .child_count = SC_ARRAY_LEN(estop_children)},
    {.name = "doctor", .summary = "cli.doctor.summary"},
};

static const sc_cli_command root = {
    .name = "smolclaw",
    .summary = "cli.root.summary",
    .children = root_children,
    .child_count = SC_ARRAY_LEN(root_children),
};

static bool command_accepts_hard_shutdown(const sc_cli_command *command)
{
    return command != nullptr &&
        (strcmp(command->name, "daemon") == 0 || strcmp(command->name, "serve") == 0 ||
         strcmp(command->name, "chat") == 0);
}

static bool command_accepts_gateway_bind(const sc_cli_command *command)
{
    return command != nullptr && strcmp(command->name, "serve") == 0;
}

static bool command_accepts_trailing_args(const sc_cli_command *command)
{
    return command != nullptr && (strcmp(command->name, "add") == 0 || strcmp(command->name, "remove") == 0);
}

#ifdef SC_HAVE_CLAGS
typedef struct sc_cli_clags_node {
    const sc_cli_command *command;
    clags_config_t config;
    clags_arg_t *args;
    size_t arg_count;
    clags_subcmd_t *subcommands;
    size_t subcommand_count;
    clags_subcmds_t subcmds;
    clags_subcmd_t *selected;
    bool onboard_providers_only;
    bool onboard_channels_only;
    bool onboard_dry_run;
    bool onboard_force;
    bool hard_shutdown;
    char *gateway_bind;
    clags_list_t trailing_args;
} sc_cli_clags_node;

typedef struct sc_cli_clags_tree {
    sc_allocator *alloc;
    sc_cli_clags_node *nodes;
    size_t node_count;
    size_t next_node;
    clags_config_t *help_config;
    bool show_version;
    bool show_features;
} sc_cli_clags_tree;

static sc_status parse_with_clags(const sc_cli_command *root_command,
                                  int argc,
                                  char **argv,
                                  sc_cli_parse_result *out);
static size_t count_command_tree(const sc_cli_command *command);
static sc_status build_clags_node(sc_cli_clags_tree *tree,
                                  const sc_cli_command *command,
                                  sc_cli_clags_node **out);
static sc_cli_clags_node *find_clags_node_for_config(sc_cli_clags_tree *tree, const clags_config_t *config);
static sc_cli_clags_node *find_clags_node_for_command(sc_cli_clags_tree *tree, const sc_cli_command *command);
static void clear_clags_tree(sc_cli_clags_tree *tree);
static clags_arg_t make_clags_help_flag(sc_cli_clags_tree *tree);
static clags_arg_t make_clags_version_flag(sc_cli_clags_tree *tree);
static clags_arg_t make_clags_features_flag(sc_cli_clags_tree *tree);
static void clags_quiet_log(clags_log_level_t level, const char *format, va_list args);
#else
static const sc_cli_command *find_child(const sc_cli_command *command, const char *name)
{
    if (command == nullptr || name == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < command->child_count; i += 1) {
        if (strcmp(command->children[i].name, name) == 0) {
            return &command->children[i];
        }
    }
    return nullptr;
}
#endif

static sc_string format_key(sc_i18n_catalog *catalog, const char *key)
{
    sc_string out = {0};
    if (!sc_status_is_ok(sc_i18n_format(catalog, sc_str_from_cstr(key), nullptr, 0, sc_allocator_heap(), &out))) {
        (void)sc_string_from_cstr(sc_allocator_heap(), key == nullptr ? "" : key, &out);
    }
    return out;
}

sc_status sc_cli_parse(const sc_cli_command *root_command,
                       int argc,
                       char **argv,
                       sc_cli_parse_result *out)
{
#ifdef SC_HAVE_CLAGS
    return parse_with_clags(root_command, argc, argv, out);
#else
    const sc_cli_command *current = root_command;

    if (root_command == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.cli.invalid_argument");
    }

    *out = (sc_cli_parse_result){
        .kind = SC_CLI_PARSE_HELP,
        .command = root_command,
        .error = nullptr,
        .exit_code = 0,
    };

    if (argc <= 1) {
        return sc_status_ok();
    }

    for (int i = 1; i < argc; i += 1) {
        const char *arg = argv[i];
        const sc_cli_command *child = nullptr;

        if (strcmp(arg, "--help") == 0 || strcmp(arg, "-h") == 0) {
            out->kind = SC_CLI_PARSE_HELP;
            out->command = current;
            return sc_status_ok();
        }
        if (strcmp(arg, "--version") == 0) {
            out->kind = SC_CLI_PARSE_VERSION;
            out->command = root_command;
            return sc_status_ok();
        }
        if (strcmp(arg, "--features") == 0) {
            out->kind = SC_CLI_PARSE_FEATURES;
            out->command = root_command;
            return sc_status_ok();
        }
        if (strcmp(arg, "--hard-shutdown") == 0 && command_accepts_hard_shutdown(current)) {
            out->hard_shutdown = true;
            continue;
        }
        if (command_accepts_gateway_bind(current) &&
            (strcmp(arg, "--bind") == 0 || strncmp(arg, "--bind=", strlen("--bind=")) == 0)) {
            if (strcmp(arg, "--bind") == 0) {
                if (i + 1 >= argc || argv[i + 1] == nullptr || argv[i + 1][0] == '-') {
                    out->error = "cli.error.unknown_argument";
                    out->exit_code = 2;
                    return sc_status_parse("sc.cli.missing_argument");
                }
                out->gateway_bind = argv[++i];
            } else {
                out->gateway_bind = arg + strlen("--bind=");
                if (out->gateway_bind[0] == '\0') {
                    out->error = "cli.error.unknown_argument";
                    out->exit_code = 2;
                    return sc_status_parse("sc.cli.missing_argument");
                }
            }
            continue;
        }
        if (current->child_count == 0 && strcmp(current->name, "onboard") == 0 && strncmp(arg, "--", 2) == 0) {
            out->kind = SC_CLI_PARSE_COMMAND;
            out->command = current;
            out->exit_code = 0;
            return sc_status_ok();
        }
        if (current->child_count == 0 && command_accepts_trailing_args(current)) {
            out->kind = SC_CLI_PARSE_COMMAND;
            out->command = current;
            out->exit_code = 0;
            return sc_status_ok();
        }
        if (arg[0] == '-') {
            out->error = "cli.error.unknown_argument";
            out->exit_code = 2;
            return sc_status_parse("sc.cli.unknown_argument");
        }

        child = find_child(current, arg);
        if (child == nullptr) {
            out->error = "cli.error.unknown_command";
            out->exit_code = 2;
            return sc_status_parse("sc.cli.unknown_command");
        }
        current = child;
    }

    out->kind = SC_CLI_PARSE_COMMAND;
    out->command = current;
    out->exit_code = 0;
    return sc_status_ok();
#endif
}

#ifdef SC_HAVE_CLAGS
static sc_status parse_with_clags(const sc_cli_command *root_command,
                                  int argc,
                                  char **argv,
                                  sc_cli_parse_result *out)
{
    sc_cli_clags_tree tree = {.alloc = sc_allocator_heap()};
    sc_cli_clags_node *root_node = nullptr;
    sc_cli_clags_node *current_node = nullptr;
    const clags_config_t *failed = nullptr;
    sc_status status;

    if (root_command == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.cli.invalid_argument");
    }

    *out = (sc_cli_parse_result){
        .kind = SC_CLI_PARSE_HELP,
        .command = root_command,
        .error = nullptr,
        .exit_code = 0,
    };

    if (argc <= 1) {
        return sc_status_ok();
    }

    for (int i = 1; i < argc; i += 1) {
        if (argv != nullptr && argv[i] != nullptr && strcmp(argv[i], "--") == 0) {
            out->error = "cli.error.unknown_argument";
            out->exit_code = 2;
            return sc_status_parse("sc.cli.unknown_argument");
        }
    }

    tree.node_count = count_command_tree(root_command);
    tree.nodes = sc_alloc(tree.alloc, tree.node_count * sizeof(*tree.nodes), _Alignof(sc_cli_clags_node));
    if (tree.nodes == nullptr) {
        return sc_status_no_memory();
    }
    for (size_t i = 0; i < tree.node_count; i += 1) {
        tree.nodes[i] = (sc_cli_clags_node){0};
    }

    status = build_clags_node(&tree, root_command, &root_node);
    if (!sc_status_is_ok(status)) {
        clear_clags_tree(&tree);
        return status;
    }

    failed = clags_parse(argc, argv, &root_node->config);
    if (tree.help_config != nullptr) {
        sc_cli_clags_node *help_node = find_clags_node_for_config(&tree, tree.help_config);
        out->kind = SC_CLI_PARSE_HELP;
        out->command = help_node == nullptr ? root_command : help_node->command;
        clear_clags_tree(&tree);
        return sc_status_ok();
    }
    if (tree.show_version) {
        out->kind = SC_CLI_PARSE_VERSION;
        out->command = root_command;
        clear_clags_tree(&tree);
        return sc_status_ok();
    }
    if (tree.show_features) {
        out->kind = SC_CLI_PARSE_FEATURES;
        out->command = root_command;
        clear_clags_tree(&tree);
        return sc_status_ok();
    }
    if (failed != nullptr) {
        out->exit_code = 2;
        if (failed->error == Clags_Error_InvalidOption) {
            out->error = "cli.error.unknown_argument";
            status = sc_status_parse("sc.cli.unknown_argument");
        } else {
            out->error = "cli.error.unknown_command";
            status = sc_status_parse("sc.cli.unknown_command");
        }
        clear_clags_tree(&tree);
        return status;
    }

    current_node = root_node;
    while (current_node != nullptr && current_node->selected != nullptr) {
        int index = clags_subcmd_index(&current_node->subcmds, current_node->selected);
        if (index < 0 || (size_t)index >= current_node->command->child_count) {
            break;
        }
        current_node = find_clags_node_for_command(&tree, &current_node->command->children[index]);
    }

    out->kind = SC_CLI_PARSE_COMMAND;
    out->command = current_node == nullptr ? root_command : current_node->command;
    out->exit_code = 0;
    out->hard_shutdown = current_node != nullptr && current_node->hard_shutdown;
    out->gateway_bind = current_node == nullptr ? nullptr : current_node->gateway_bind;
    clear_clags_tree(&tree);
    return sc_status_ok();
}

static size_t count_command_tree(const sc_cli_command *command)
{
    size_t count = 1;

    if (command == nullptr) {
        return 0;
    }
    for (size_t i = 0; i < command->child_count; i += 1) {
        count += count_command_tree(&command->children[i]);
    }
    return count;
}

static sc_status build_clags_node(sc_cli_clags_tree *tree,
                                  const sc_cli_command *command,
                                  sc_cli_clags_node **out)
{
    sc_cli_clags_node *node = nullptr;
    size_t arg_count = 3;

    if (tree == nullptr || command == nullptr || out == nullptr || tree->next_node >= tree->node_count) {
        return sc_status_invalid_argument("sc.cli.invalid_argument");
    }

    node = &tree->nodes[tree->next_node++];
    node->command = command;
    node->subcommand_count = command->child_count;
    if (node->subcommand_count > 0) {
        node->subcommands = sc_alloc(tree->alloc,
                                     node->subcommand_count * sizeof(*node->subcommands),
                                     _Alignof(clags_subcmd_t));
        if (node->subcommands == nullptr) {
            return sc_status_no_memory();
        }
        for (size_t i = 0; i < command->child_count; i += 1) {
            sc_cli_clags_node *child_node = nullptr;
            sc_status status = build_clags_node(tree, &command->children[i], &child_node);
            if (!sc_status_is_ok(status)) {
                return status;
            }
            node->subcommands[i] = (clags_subcmd_t){
                .name = command->children[i].name,
                .description = command->children[i].summary,
                .config = &child_node->config,
            };
        }
        node->subcmds = (clags_subcmds_t){.items = node->subcommands, .count = node->subcommand_count};
        arg_count += 1;
    }
    if (strcmp(command->name, "onboard") == 0) {
        arg_count += 4;
    }
    if (command_accepts_hard_shutdown(command)) {
        arg_count += 1;
    }
    if (command_accepts_gateway_bind(command)) {
        arg_count += 1;
    }
    if (command_accepts_trailing_args(command)) {
        arg_count += 1;
    }

    node->arg_count = arg_count;
    node->args = sc_alloc(tree->alloc, node->arg_count * sizeof(*node->args), _Alignof(clags_arg_t));
    if (node->args == nullptr) {
        return sc_status_no_memory();
    }
    node->args[0] = make_clags_help_flag(tree);
    node->args[1] = make_clags_version_flag(tree);
    node->args[2] = make_clags_features_flag(tree);
    if (node->subcommand_count > 0) {
        node->args[3] = (clags_arg_t){
            .type = Clags_Positional,
            .pos = {
                .variable = &node->selected,
                .arg_name = "command",
                .description = "command",
                .value_type = Clags_Subcmd,
                .optional = true,
                .subcmds = &node->subcmds,
            },
        };
    }
    {
        size_t index = node->subcommand_count > 0 ? 4U : 3U;
        if (strcmp(command->name, "onboard") == 0) {
            node->args[index++] = (clags_arg_t){.type = Clags_Flag,
                                                .flag = {.long_flag = "providers-only",
                                                         .variable = &node->onboard_providers_only,
                                                         .description = "configure providers only"}};
            node->args[index++] = (clags_arg_t){.type = Clags_Flag,
                                                .flag = {.long_flag = "channels-only",
                                                         .variable = &node->onboard_channels_only,
                                                         .description = "configure channels only"}};
            node->args[index++] = (clags_arg_t){.type = Clags_Flag,
                                                .flag = {.long_flag = "dry-run",
                                                         .variable = &node->onboard_dry_run,
                                                         .description = "print the generated config"}};
            node->args[index++] = (clags_arg_t){.type = Clags_Flag,
                                                .flag = {.long_flag = "force",
                                                         .variable = &node->onboard_force,
                                                         .description = "replace an existing config"}};
        }
        if (command_accepts_gateway_bind(command)) {
            node->args[index++] = (clags_arg_t){.type = Clags_Option,
                                                .opt = {.long_flag = "bind",
                                                        .variable = &node->gateway_bind,
                                                        .arg_name = "ADDR",
                                                        .value_type = Clags_String,
                                                        .description = "bind address for gateway listener"}};
        }
        if (command_accepts_hard_shutdown(command)) {
            node->args[index++] = (clags_arg_t){.type = Clags_Flag,
                                                .flag = {.long_flag = "hard-shutdown",
                                                         .variable = &node->hard_shutdown,
                                                         .description = "skip graceful shutdown drain"}};
        }
        if (command_accepts_trailing_args(command)) {
            node->trailing_args = (clags_list_t)clags_string_list();
            node->args[index++] = (clags_arg_t){
                .type = Clags_Positional,
                .pos = {
                    .variable = &node->trailing_args,
                    .arg_name = "args",
                    .description = "arguments",
                    .value_type = Clags_String,
                    .is_list = true,
                    .optional = true,
                },
            };
        }
    }

    node->config = (clags_config_t){
        .args = node->args,
        .args_count = node->arg_count,
        .options = {
            .print_no_notes = true,
            .log_handler = clags_quiet_log,
            .min_log_level = Clags_NoLogs,
            .description = command->summary,
        },
        .allocs = {.item_size = sizeof(char *)},
    };

    *out = node;
    return sc_status_ok();
}

static sc_cli_clags_node *find_clags_node_for_config(sc_cli_clags_tree *tree, const clags_config_t *config)
{
    if (tree == nullptr || config == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < tree->node_count; i += 1) {
        if (&tree->nodes[i].config == config) {
            return &tree->nodes[i];
        }
    }
    return nullptr;
}

static sc_cli_clags_node *find_clags_node_for_command(sc_cli_clags_tree *tree, const sc_cli_command *command)
{
    if (tree == nullptr || command == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < tree->node_count; i += 1) {
        if (tree->nodes[i].command == command) {
            return &tree->nodes[i];
        }
    }
    return nullptr;
}

static void clear_clags_tree(sc_cli_clags_tree *tree)
{
    if (tree == nullptr || tree->nodes == nullptr) {
        return;
    }
    for (size_t i = 0; i < tree->node_count; i += 1) {
        sc_cli_clags_node *node = &tree->nodes[i];
        sc_free(tree->alloc, node->args, node->arg_count * sizeof(*node->args), _Alignof(clags_arg_t));
        sc_free(tree->alloc,
                node->subcommands,
                node->subcommand_count * sizeof(*node->subcommands),
                _Alignof(clags_subcmd_t));
        clags_list_free(&node->trailing_args);
    }
    sc_free(tree->alloc, tree->nodes, tree->node_count * sizeof(*tree->nodes), _Alignof(sc_cli_clags_node));
    *tree = (sc_cli_clags_tree){0};
}

static clags_arg_t make_clags_help_flag(sc_cli_clags_tree *tree)
{
    return (clags_arg_t){
        .type = Clags_Flag,
        .flag = {
            .short_flag = 'h',
            .long_flag = "help",
            .variable = &tree->help_config,
            .description = "help",
            .exit = true,
            .type = Clags_ConfigFlag,
        },
    };
}

static clags_arg_t make_clags_version_flag(sc_cli_clags_tree *tree)
{
    return (clags_arg_t){
        .type = Clags_Flag,
        .flag = {
            .long_flag = "version",
            .variable = &tree->show_version,
            .description = "version",
            .exit = true,
        },
    };
}

static clags_arg_t make_clags_features_flag(sc_cli_clags_tree *tree)
{
    return (clags_arg_t){
        .type = Clags_Flag,
        .flag = {
            .long_flag = "features",
            .variable = &tree->show_features,
            .description = "features",
            .exit = true,
        },
    };
}

static void clags_quiet_log(clags_log_level_t level, const char *format, va_list args)
{
    (void)level;
    (void)format;
    (void)args;
}
#endif

void sc_cli_print_help(const sc_cli_command *command, const char *program_name, FILE *stream)
{
    sc_i18n_catalog catalog = {0};
    sc_i18n_arg program_arg = {0};
    sc_string usage = {0};
    sc_string summary = {0};
    sc_string commands = {0};
    sc_string options = {0};
    sc_string help = {0};
    sc_string version = {0};
    sc_string features = {0};
    sc_string hard_shutdown = {0};
    sc_string bind = {0};

    if (command == nullptr || stream == nullptr) {
        return;
    }
    if (program_name == nullptr) {
        program_name = command->name;
    }

    sc_i18n_catalog_init(&catalog, sc_allocator_heap(), sc_str_from_cstr("en"));
    (void)sc_i18n_catalog_load_default_en(&catalog);
    program_arg = (sc_i18n_arg){.name = sc_str_from_cstr("program"), .value = sc_str_from_cstr(program_name)};
    (void)sc_i18n_format(&catalog,
                         sc_str_from_cstr("cli.usage"),
                         &program_arg,
                         1,
                         sc_allocator_heap(),
                         &usage);
    summary = format_key(&catalog, command->summary == nullptr ? "" : command->summary);
    commands = format_key(&catalog, "cli.commands");
    options = format_key(&catalog, "cli.options");
    help = format_key(&catalog, "cli.option.help");
    version = format_key(&catalog, "cli.option.version");
    features = format_key(&catalog, "cli.option.features");
    hard_shutdown = format_key(&catalog, "cli.option.hard_shutdown");
    bind = format_key(&catalog, "cli.option.bind");

    (void)fprintf(stream, "%s", usage.ptr == nullptr ? "" : usage.ptr);
    if (command->child_count > 0) {
        (void)fprintf(stream, " <command>");
    }
    (void)fprintf(stream, "\n\n%s\n", summary.ptr == nullptr ? "" : summary.ptr);

    if (command->child_count > 0) {
        (void)fprintf(stream, "\n%s\n", commands.ptr == nullptr ? "" : commands.ptr);
        for (size_t i = 0; i < command->child_count; i += 1) {
            sc_string child_summary = format_key(&catalog,
                                                 command->children[i].summary == nullptr ? "" : command->children[i].summary);
            (void)fprintf(stream,
                          "  %-10s %s\n",
                          command->children[i].name,
                          child_summary.ptr == nullptr ? "" : child_summary.ptr);
            sc_string_clear(&child_summary);
        }
    }

    (void)fprintf(stream, "\n%s\n", options.ptr == nullptr ? "" : options.ptr);
    (void)fprintf(stream, "  --help       %s\n", help.ptr == nullptr ? "" : help.ptr);
    (void)fprintf(stream, "  --version    %s\n", version.ptr == nullptr ? "" : version.ptr);
    (void)fprintf(stream, "  --features   %s\n", features.ptr == nullptr ? "" : features.ptr);
    if (command_accepts_hard_shutdown(command)) {
        (void)fprintf(stream, "  --hard-shutdown %s\n", hard_shutdown.ptr == nullptr ? "" : hard_shutdown.ptr);
    }
    if (command_accepts_gateway_bind(command)) {
        (void)fprintf(stream, "  --bind ADDR  %s\n", bind.ptr == nullptr ? "" : bind.ptr);
    }

    sc_string_clear(&bind);
    sc_string_clear(&hard_shutdown);
    sc_string_clear(&features);
    sc_string_clear(&version);
    sc_string_clear(&help);
    sc_string_clear(&options);
    sc_string_clear(&commands);
    sc_string_clear(&summary);
    sc_string_clear(&usage);
    sc_i18n_catalog_clear(&catalog);
}

const sc_cli_command *sc_cli_default_root(void)
{
    return &root;
}
