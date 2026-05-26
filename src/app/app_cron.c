#include "app/app_commands.h"
#include "app/app_internal.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "sc/autonomy.h"
#include "sc/result.h"

static sc_status cron_state_path(char *state_dir, size_t state_dir_capacity, char *state_path, size_t state_path_capacity);
static sc_status cron_load_store(sc_cron_job_store *store, char *state_path, size_t state_path_capacity);
static sc_status cron_save_store(const sc_cron_job_store *store, const char *state_path);
static bool cron_id_is_valid(const char *id);
static int cron_print_failure(const char *operation, const sc_status *status);
static int cron_run_add(int argc, char **argv);
static int cron_run_list(int argc, char **argv);
static int cron_run_remove(int argc, char **argv);

int sc_app_run_cron_command(int argc, char **argv)
{
    const char *command = argc > 2 ? argv[2] : "list";

    if (command == nullptr || strcmp(command, "list") == 0) {
        return cron_run_list(argc, argv);
    }
    if (strcmp(command, "add") == 0) {
        return cron_run_add(argc, argv);
    }
    if (strcmp(command, "remove") == 0) {
        return cron_run_remove(argc, argv);
    }

    (void)fprintf(stderr, "smolclaw: cron failed: sc.cli.cron.unknown_command\n");
    return 2;
}

static sc_status cron_state_path(char *state_dir, size_t state_dir_capacity, char *state_path, size_t state_path_capacity)
{
    const char *config_path = getenv("SMOLCLAW_CONFIG");
    char workspace[4096] = {0};
    sc_status status;

    if (state_dir == nullptr || state_path == nullptr || state_dir_capacity == 0U || state_path_capacity == 0U) {
        return sc_status_invalid_argument("sc.cli.cron.path_invalid_argument");
    }
    if (config_path == nullptr || config_path[0] == '\0') {
        config_path = "smolclaw.toml";
    }

    status = sc_app_cli_workspace_path(workspace, sizeof(workspace), config_path);
    if (sc_status_is_ok(status)) {
        status = sc_app_join_cstr_path(state_dir, state_dir_capacity, workspace, "state", "sc.cli.cron.state_dir_invalid");
    }
    if (sc_status_is_ok(status)) {
        status = sc_app_join_cstr_path(state_path,
                                       state_path_capacity,
                                       state_dir,
                                       "cron_jobs.state",
                                       "sc.cli.cron.state_path_invalid");
    }
    return status;
}

static sc_status cron_load_store(sc_cron_job_store *store, char *state_path, size_t state_path_capacity)
{
    char state_dir[4096] = {0};
    sc_status status;

    if (store == nullptr || state_path == nullptr || state_path_capacity == 0U) {
        return sc_status_invalid_argument("sc.cli.cron.load_invalid_argument");
    }

    sc_cron_job_store_init(store, sc_allocator_heap());
    status = cron_state_path(state_dir, sizeof(state_dir), state_path, state_path_capacity);
    if (sc_status_is_ok(status)) {
        status = sc_app_ensure_cli_dir(state_dir);
    }
    if (sc_status_is_ok(status)) {
        status = sc_cron_job_store_load_file(store, sc_str_from_cstr(state_path));
    }
    return status;
}

static sc_status cron_save_store(const sc_cron_job_store *store, const char *state_path)
{
    if (store == nullptr || state_path == nullptr || state_path[0] == '\0') {
        return sc_status_invalid_argument("sc.cli.cron.save_invalid_argument");
    }
    return sc_cron_job_store_save_file(store, sc_str_from_cstr(state_path));
}

static bool cron_id_is_valid(const char *id)
{
    size_t len = 0;

    if (id == nullptr || id[0] == '\0') {
        return false;
    }
    len = strlen(id);
    if (len > 128U) {
        return false;
    }
    for (size_t i = 0; i < len; i += 1) {
        unsigned char ch = (unsigned char)id[i];
        if (!(isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ':')) {
            return false;
        }
    }
    return true;
}

static int cron_print_failure(const char *operation, const sc_status *status)
{
    (void)fprintf(stderr,
                  "smolclaw: cron %s failed: %s\n",
                  operation == nullptr ? "command" : operation,
                  status == nullptr || status->error_key == nullptr ? "sc.cli.cron.failed" : status->error_key);
    return status != nullptr && status->code == SC_ERR_INVALID_ARGUMENT ? 2 : 1;
}

static int cron_run_add(int argc, char **argv)
{
    sc_cron_job_store store = {0};
    sc_cron_job job = {.struct_size = sizeof(job), .kind = SC_CRON_JOB_AGENT, .enabled = true};
    char state_path[4096] = {0};
    const char *target = argc > 6 ? argv[6] : "default";
    sc_status status;
    int exit_code = 0;

    if (argc != 6 && argc != 7) {
        (void)fprintf(stderr, "smolclaw: cron add failed: sc.cli.cron.add_usage\n");
        return 2;
    }
    if (!cron_id_is_valid(argv[3])) {
        status = sc_status_invalid_argument("sc.cli.cron.id_invalid");
        exit_code = cron_print_failure("add", &status);
        sc_status_clear(&status);
        return exit_code;
    }

    status = cron_load_store(&store, state_path, sizeof(state_path));
    if (sc_status_is_ok(status)) {
        status = sc_cron_schedule_parse(sc_str_from_cstr(argv[4]), &job.schedule);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(sc_allocator_heap(), argv[3], &job.id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(sc_allocator_heap(), argv[5], &job.command);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(sc_allocator_heap(), target, &job.delivery_target);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_cstr(sc_allocator_heap(), argv[4], &job.schedule_text);
    }
    if (sc_status_is_ok(status)) {
        status = sc_cron_job_store_put(&store, &job);
    }
    if (sc_status_is_ok(status)) {
        status = cron_save_store(&store, state_path);
    }

    if (!sc_status_is_ok(status)) {
        exit_code = cron_print_failure("add", &status);
    } else {
        (void)fprintf(stdout, "SmolClaw cron\n");
        (void)fprintf(stdout, "added: %s\n", argv[3]);
        (void)fprintf(stdout, "schedule: %s\n", argv[4]);
        (void)fprintf(stdout, "status: ok\n");
    }

    sc_cron_job_clear(&job);
    sc_cron_job_store_clear(&store);
    sc_status_clear(&status);
    return exit_code;
}

static int cron_run_list(int argc, char **argv)
{
    sc_cron_job_store store = {0};
    char state_path[4096] = {0};
    sc_status status;
    int exit_code = 0;

    (void)argv;
    if (argc != 2 && argc != 3) {
        (void)fprintf(stderr, "smolclaw: cron list failed: sc.cli.cron.list_usage\n");
        return 2;
    }

    status = cron_load_store(&store, state_path, sizeof(state_path));
    if (!sc_status_is_ok(status)) {
        exit_code = cron_print_failure("list", &status);
        goto cleanup;
    }

    (void)fprintf(stdout, "SmolClaw cron\n");
    (void)fprintf(stdout, "jobs:\n");
    if (store.jobs.len == 0) {
        (void)fprintf(stdout, "  (none)\n");
    }
    for (size_t i = 0; i < store.jobs.len; i += 1) {
        const sc_cron_job *job = sc_vec_at_const(&store.jobs, i);
        sc_string schedule_owner = {0};
        sc_str schedule = {0};

        if (job == nullptr) {
            status = sc_status_invalid_argument("sc.cli.cron.job_invalid");
            exit_code = cron_print_failure("list", &status);
            sc_status_clear(&status);
            break;
        }
        if (job->schedule_text.len > 0) {
            schedule = sc_string_as_str(&job->schedule_text);
        } else {
            status = sc_cron_schedule_format(&job->schedule, sc_allocator_heap(), &schedule_owner);
            if (!sc_status_is_ok(status)) {
                exit_code = cron_print_failure("list", &status);
                sc_status_clear(&status);
                sc_string_clear(&schedule_owner);
                break;
            }
            schedule = sc_string_as_str(&schedule_owner);
        }
        (void)fprintf(stdout,
                      "  %.*s schedule=\"%.*s\" enabled=%s prompt=\"%.*s\"\n",
                      (int)job->id.len,
                      job->id.ptr == nullptr ? "" : job->id.ptr,
                      (int)schedule.len,
                      schedule.ptr == nullptr ? "" : schedule.ptr,
                      job->enabled ? "true" : "false",
                      (int)job->command.len,
                      job->command.ptr == nullptr ? "" : job->command.ptr);
        sc_string_clear(&schedule_owner);
    }
    if (exit_code == 0) {
        (void)fprintf(stdout, "status: ok\n");
    }

cleanup:
    sc_cron_job_store_clear(&store);
    sc_status_clear(&status);
    return exit_code;
}

static int cron_run_remove(int argc, char **argv)
{
    sc_cron_job_store store = {0};
    char state_path[4096] = {0};
    sc_status status;
    int exit_code = 0;

    if (argc != 4) {
        (void)fprintf(stderr, "smolclaw: cron remove failed: sc.cli.cron.remove_usage\n");
        return 2;
    }
    if (!cron_id_is_valid(argv[3])) {
        status = sc_status_invalid_argument("sc.cli.cron.id_invalid");
        exit_code = cron_print_failure("remove", &status);
        sc_status_clear(&status);
        return exit_code;
    }

    status = cron_load_store(&store, state_path, sizeof(state_path));
    if (sc_status_is_ok(status)) {
        status = sc_cron_job_store_remove(&store, sc_str_from_cstr(argv[3]));
    }
    if (sc_status_is_ok(status)) {
        status = cron_save_store(&store, state_path);
    }

    if (!sc_status_is_ok(status)) {
        exit_code = cron_print_failure("remove", &status);
    } else {
        (void)fprintf(stdout, "SmolClaw cron\n");
        (void)fprintf(stdout, "removed: %s\n", argv[3]);
        (void)fprintf(stdout, "status: ok\n");
    }

    sc_cron_job_store_clear(&store);
    sc_status_clear(&status);
    return exit_code;
}
