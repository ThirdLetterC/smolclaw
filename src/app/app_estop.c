#include "app/app_commands.h"
#include "app/app_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc/allocator.h"
#include "sc/security.h"

int sc_app_run_estop_command(const char *command)
{
    char state_dir[4096] = {0};
    char state_path[4096] = {0};
    sc_estop_state state = {0};
    sc_status status = sc_app_cli_estop_paths(state_dir, sizeof(state_dir), state_path, sizeof(state_path));

    if (command == nullptr || command[0] == '\0') {
        command = "status";
    }

    if (sc_status_is_ok(status) && strcmp(command, "trip") == 0) {
        const char *reason = getenv("SMOLCLAW_ESTOP_REASON");
        status = sc_app_ensure_cli_dir(state_dir);
        if (sc_status_is_ok(status)) {
            sc_estop_init(&state, sc_allocator_heap());
            status = sc_estop_trip(&state,
                                   sc_str_from_cstr(reason == nullptr || reason[0] == '\0'
                                                        ? "operator requested emergency stop"
                                                        : reason));
        }
        if (sc_status_is_ok(status)) {
            status = sc_estop_write_file(&state, sc_str_from_cstr(state_path));
        }
    } else if (sc_status_is_ok(status) && strcmp(command, "reset") == 0) {
        status = sc_app_ensure_cli_dir(state_dir);
        if (sc_status_is_ok(status)) {
            sc_estop_init(&state, sc_allocator_heap());
            sc_estop_reset(&state);
            status = sc_estop_write_file(&state, sc_str_from_cstr(state_path));
        }
    } else if (sc_status_is_ok(status) && strcmp(command, "status") == 0) {
        if (sc_app_file_exists(state_path)) {
            status = sc_estop_read_file(sc_allocator_heap(), sc_str_from_cstr(state_path), &state);
        } else {
            sc_estop_init(&state, sc_allocator_heap());
        }
    } else if (sc_status_is_ok(status)) {
        status = sc_status_invalid_argument("sc.cli.estop.unknown_command");
    }

    (void)fprintf(stdout, "SmolClaw emergency stop\n");
    (void)fprintf(stdout, "path: %s\n", state_path[0] == '\0' ? "(unresolved)" : state_path);
    if (sc_status_is_ok(status)) {
        (void)fprintf(stdout, "active: %s\n", state.active ? "true" : "false");
        if (state.reason.len > 0) {
            (void)fprintf(stdout, "reason: %s\n", state.reason.ptr);
        }
        (void)fprintf(stdout, "status: ok\n");
        sc_estop_clear(&state);
        return 0;
    }

    (void)fprintf(stdout, "status: failed\n");
    (void)fprintf(stdout, "error: %s\n", status.error_key == nullptr ? "sc.cli.estop.failed" : status.error_key);
    sc_estop_clear(&state);
    sc_status_clear(&status);
    return 1;
}
