#define _XOPEN_SOURCE 700

#include "sc/autonomy.h"
#include "sc/bootstrap.h"
#include "sc/channel.h"
#include "sc/provider.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct fake_delivery {
    int calls;
    sc_string last;
} fake_delivery;

static sc_status fake_deliver(void *impl, const sc_delivery_message *message);
static void fake_destroy(void *impl);
static sc_status fake_delivery_new(fake_delivery *fake, sc_delivery_target **out);
static sc_status set_string(sc_string *out, const char *value);
static sc_status make_temp_path(const char *suffix, sc_string *out);
static sc_status temp_child_path(sc_str parent, const char *child, sc_string *out);
static int write_file(sc_str path, const char *text);
static int test_cron_parse_store_and_due(void);
static int test_cron_store_file_persistence(void);
static int test_shell_denied_and_agent_execution(void);
static int test_sop_parsing_and_manual_approval(void);
static int test_routine_match_and_dispatch(void);
static int test_heartbeat_persistence(void);
static int test_bootstrap_config_driven_runtime_assembly(void);
static int test_session_sqlite_disables_jsonl_writes(void);

static const sc_delivery_vtab fake_delivery_vtab = {
    .struct_size = sizeof(sc_delivery_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "fake-delivery",
    .deliver = fake_deliver,
    .destroy = fake_destroy,
};

int main(void)
{
    int failures = 0;

    failures += test_cron_parse_store_and_due();
    failures += test_cron_store_file_persistence();
    failures += test_shell_denied_and_agent_execution();
    failures += test_sop_parsing_and_manual_approval();
    failures += test_routine_match_and_dispatch();
    failures += test_heartbeat_persistence();
    failures += test_bootstrap_config_driven_runtime_assembly();
    failures += test_session_sqlite_disables_jsonl_writes();

    return failures == 0 ? 0 : 1;
}

static int test_cron_parse_store_and_due(void)
{
    int failures = 0;
    sc_cron_schedule schedule = {0};
    sc_cron_schedule invalid = {0};
    sc_wall_time next = {0};
    sc_cron_job_store store = {0};
    sc_cron_job job = {.struct_size = sizeof(job), .kind = SC_CRON_JOB_AGENT, .enabled = true};
    sc_cron_job once = {.struct_size = sizeof(once), .kind = SC_CRON_JOB_AGENT, .enabled = true, .once = true};
    sc_cron_run_store runs = {0};

    failures += sc_test_expect_status("cron parse", sc_cron_schedule_parse(sc_str_from_cstr("*/15 * * * *"), &schedule), SC_OK);
    failures += sc_test_expect_status("cron next", sc_cron_schedule_next_after(&schedule, (sc_wall_time){.unix_ns = 0}, &next), SC_OK);
    failures += sc_test_expect_true("cron next 15 minutes", next.unix_ns == 900LL * 1000000000LL);
    failures += sc_test_expect_status("cron invalid", sc_cron_schedule_parse(sc_str_from_cstr("61 * * * *"), &invalid), SC_ERR_PARSE);
#ifdef SC_HAVE_NANOCRON
    failures += sc_test_expect_status("nanocron parse",
                              sc_cron_schedule_parse(sc_str_from_cstr("250000000 5 */10 * * * *"), &schedule),
                              SC_OK);
    failures += sc_test_expect_status("nanocron next", sc_cron_schedule_next_after(&schedule, (sc_wall_time){.unix_ns = 0}, &next), SC_OK);
    failures += sc_test_expect_true("nanocron next precise", next.unix_ns == 5250000000LL);
    failures += sc_test_expect_true("nanocron matches precise", sc_cron_schedule_matches(&schedule, next));
#endif

    sc_cron_job_store_init(&store, sc_allocator_heap());
    sc_cron_run_store_init(&runs, sc_allocator_heap());
    job.schedule = schedule;
    failures += sc_test_expect_status("job id", set_string(&job.id, "job-a"), SC_OK);
    failures += sc_test_expect_status("job command", set_string(&job.command, "hello"), SC_OK);
    failures += sc_test_expect_status("job target", set_string(&job.delivery_target, "session"), SC_OK);
    failures += sc_test_expect_status("job put", sc_cron_job_store_put(&store, &job), SC_OK);
    failures += sc_test_expect_true("job store len", sc_cron_job_store_len(&store) == 1);
    failures += sc_test_expect_true("job find", sc_cron_job_store_find(&store, sc_str_from_cstr("job-a")) != nullptr);
    failures += sc_test_expect_true("job due", sc_cron_job_is_due(&job, next));
    job.enabled = false;
    failures += sc_test_expect_true("disabled skipped", !sc_cron_job_is_due(&job, next));
    failures += sc_test_expect_status("disabled execute", sc_cron_job_execute(&job, nullptr, nullptr, &runs, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_true("disabled no record", sc_cron_run_store_len(&runs) == 0);
    once.schedule = schedule;
    once.last_run_at = next;
    failures += sc_test_expect_true("once skipped after run", !sc_cron_job_is_due(&once, next));

    sc_cron_run_store_clear(&runs);
    sc_cron_job_store_clear(&store);
    sc_cron_job_clear(&job);
    sc_cron_job_clear(&once);
    return failures;
}

static int test_cron_store_file_persistence(void)
{
    int failures = 0;
    sc_string path = {0};
    sc_string schedule_text = {0};
    sc_cron_job_store store = {0};
    sc_cron_job_store loaded = {0};
    sc_cron_job job = {.struct_size = sizeof(job), .kind = SC_CRON_JOB_AGENT, .enabled = true};
    sc_cron_job *loaded_job = nullptr;

    sc_cron_job_store_init(&store, sc_allocator_heap());
    sc_cron_job_store_init(&loaded, sc_allocator_heap());
    failures += sc_test_expect_status("cron state path", make_temp_path("cron-jobs.state", &path), SC_OK);
    failures += sc_test_expect_status("cron state schedule", sc_cron_schedule_parse(sc_str_from_cstr("0 9 * * *"), &job.schedule), SC_OK);
    failures += sc_test_expect_status("cron state id", set_string(&job.id, "daily-report"), SC_OK);
    failures += sc_test_expect_status("cron state command", set_string(&job.command, "Send daily summary | with newline\nredacted"), SC_OK);
    failures += sc_test_expect_status("cron state target", set_string(&job.delivery_target, "default"), SC_OK);
    failures += sc_test_expect_status("cron state schedule text", set_string(&job.schedule_text, "0 9 * * *"), SC_OK);
    failures += sc_test_expect_status("cron state put", sc_cron_job_store_put(&store, &job), SC_OK);
    failures += sc_test_expect_status("cron state save", sc_cron_job_store_save_file(&store, sc_string_as_str(&path)), SC_OK);
    failures += sc_test_expect_status("cron state load", sc_cron_job_store_load_file(&loaded, sc_string_as_str(&path)), SC_OK);
    failures += sc_test_expect_true("cron state loaded len", sc_cron_job_store_len(&loaded) == 1);
    loaded_job = sc_cron_job_store_find(&loaded, sc_str_from_cstr("daily-report"));
    failures += sc_test_expect_true("cron state find", loaded_job != nullptr);
    if (loaded_job != nullptr) {
        failures += sc_test_expect_true("cron state prompt",
                                strcmp(loaded_job->command.ptr, "Send daily summary | with newline\nredacted") == 0);
        failures += sc_test_expect_status("cron state format",
                                  sc_cron_schedule_format(&loaded_job->schedule, sc_allocator_heap(), &schedule_text),
                                  SC_OK);
        failures += sc_test_expect_true("cron state schedule text",
                                sc_str_equal(sc_string_as_str(&loaded_job->schedule_text), sc_str_from_cstr("0 9 * * *")));
    }

    sc_string_clear(&schedule_text);
    sc_cron_job_clear(&job);
    sc_cron_job_store_clear(&loaded);
    sc_cron_job_store_clear(&store);
    sc_string_clear(&path);
    return failures;
}

static int test_shell_denied_and_agent_execution(void)
{
    int failures = 0;
    sc_security_policy policy = {0};
    sc_cron_schedule schedule = {0};
    sc_cron_job shell = {.struct_size = sizeof(shell), .kind = SC_CRON_JOB_SHELL, .enabled = true};
    sc_cron_job agent_job = {.struct_size = sizeof(agent_job), .kind = SC_CRON_JOB_AGENT, .enabled = true};
    sc_cron_job cancelled_job = {.struct_size = sizeof(cancelled_job), .kind = SC_CRON_JOB_AGENT, .enabled = true, .cancel_requested = true};
    sc_cron_run_store runs = {0};
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    fake_delivery fake = {0};
    sc_delivery_target *delivery = nullptr;

    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("schedule", sc_cron_schedule_parse(sc_str_from_cstr("* * * * *"), &schedule), SC_OK);
    shell.schedule = schedule;
    failures += sc_test_expect_status("shell id", set_string(&shell.id, "shell"), SC_OK);
    failures += sc_test_expect_status("shell command", set_string(&shell.command, "rm -rf /tmp/nope"), SC_OK);
    failures += sc_test_expect_status("shell denied", sc_cron_job_validate_shell(&shell, &policy, nullptr), SC_ERR_SECURITY_DENIED);

    sc_cron_run_store_init(&runs, sc_allocator_heap());
    failures += sc_test_expect_status("provider",
                              sc_provider_mock_new(sc_allocator_heap(),
                                                   SC_PROVIDER_MOCK_TEXT,
                                                   sc_str_from_cstr("cron-output"),
                                                   &provider),
                              SC_OK);
    failures += sc_test_expect_status("agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){.struct_size = sizeof(sc_agent_options), .provider = provider},
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("fake delivery", fake_delivery_new(&fake, &delivery), SC_OK);
    agent_job.schedule = schedule;
    failures += sc_test_expect_status("agent id", set_string(&agent_job.id, "agent"), SC_OK);
    failures += sc_test_expect_status("agent command", set_string(&agent_job.command, "say hi"), SC_OK);
    failures += sc_test_expect_status("agent target", set_string(&agent_job.delivery_target, "session"), SC_OK);
    failures += sc_test_expect_status("agent execute",
                              sc_cron_job_execute(&agent_job, agent, delivery, &runs, sc_allocator_heap()),
                              SC_OK);
    failures += sc_test_expect_true("delivery called", fake.calls == 1 && strcmp(fake.last.ptr, "cron-output") == 0);
    failures += sc_test_expect_true("run recorded", sc_cron_run_store_len(&runs) == 1);

    cancelled_job.schedule = schedule;
    failures += sc_test_expect_status("cancel id", set_string(&cancelled_job.id, "cancel"), SC_OK);
    failures += sc_test_expect_status("cancel command", set_string(&cancelled_job.command, "stop"), SC_OK);
    failures += sc_test_expect_status("cancel execute",
                              sc_cron_job_execute(&cancelled_job, agent, delivery, &runs, sc_allocator_heap()),
                              SC_ERR_CANCELLED);
    failures += sc_test_expect_true("cancel recorded", sc_cron_run_store_len(&runs) == 2);

    sc_delivery_target_destroy(delivery);
    sc_string_clear(&fake.last);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_cron_run_store_clear(&runs);
    sc_cron_job_clear(&shell);
    sc_cron_job_clear(&agent_job);
    sc_cron_job_clear(&cancelled_job);
    sc_security_policy_clear(&policy);
    return failures;
}

static int test_sop_parsing_and_manual_approval(void)
{
    int failures = 0;
    const char *markdown =
        "# Recovery SOP\n"
        "## Step: Confirm\n"
        "action: manual\n"
        "requires_approval: true\n"
        "## Step: Notify\n"
        "action: agent\n"
        "prompt: status update\n";
    sc_sop_document document = {0};
    sc_sop_run_state run = {0};
    sc_audit_chain audit = {0};
    sc_string path = {0};
    sc_sop_document loaded = {0};

    sc_audit_chain_init(&audit, sc_allocator_heap());
    failures += sc_test_expect_status("parse sop",
                              sc_sop_markdown_parse(sc_allocator_heap(), sc_str_from_cstr(markdown), &document),
                              SC_OK);
    failures += sc_test_expect_true("sop title", strcmp(document.title.ptr, "Recovery SOP") == 0);
    failures += sc_test_expect_true("sop steps", document.steps.len == 2);
    sc_sop_run_start(&run);
    failures += sc_test_expect_status("approval required", sc_sop_run_advance(&run, &document, &audit), SC_ERR_CANCELLED);
    failures += sc_test_expect_true("waiting approval", run.waiting_approval);
    failures += sc_test_expect_status("approve", sc_sop_run_approve_manual(&run, &document, &audit), SC_OK);
    failures += sc_test_expect_status("advance final", sc_sop_run_advance(&run, &document, &audit), SC_OK);
    failures += sc_test_expect_true("sop completed", run.completed);
    failures += sc_test_expect_true("audit valid", sc_audit_chain_verify(&audit));
    failures += sc_test_expect_true("condition", sc_sop_condition_evaluate(sc_str_from_cstr("ready"), sc_str_from_cstr("ready")));

    failures += sc_test_expect_status("sop temp path", make_temp_path("sop.md", &path), SC_OK);
    failures += sc_test_expect_true("write sop", write_file(sc_string_as_str(&path), markdown) == 0);
    failures += sc_test_expect_status("load sop", sc_sop_markdown_load(sc_allocator_heap(), sc_string_as_str(&path), &loaded), SC_OK);
    failures += sc_test_expect_true("loaded steps", loaded.steps.len == 2);

    sc_sop_document_clear(&loaded);
    sc_string_clear(&path);
    sc_audit_chain_clear(&audit);
    sc_sop_document_clear(&document);
    return failures;
}

static int test_routine_match_and_dispatch(void)
{
    int failures = 0;
    sc_routine routine = {0};
    fake_delivery fake = {0};
    sc_delivery_target *delivery = nullptr;
    sc_routine_event event = {
        .struct_size = sizeof(event),
        .name = sc_str_from_cstr("message.created"),
        .payload = sc_str_from_cstr("{}"),
        .occurred_at = {.unix_ns = 1000LL * 1000000000LL},
    };

    failures += sc_test_expect_status("routine init",
                              sc_routine_init(&routine,
                                              sc_allocator_heap(),
                                              sc_str_from_cstr("r1"),
                                              sc_str_from_cstr("message.created"),
                                              sc_str_from_cstr("session"),
                                              sc_str_from_cstr("respond")),
                              SC_OK);
    routine.cooldown_ms = 5000;
    failures += sc_test_expect_true("routine match first", sc_routine_matches(&routine, &event));
    event.occurred_at.unix_ns += 1000LL * 1000000LL;
    failures += sc_test_expect_true("routine cooldown", !sc_routine_matches(&routine, &event));
    event.occurred_at.unix_ns += 5000LL * 1000000LL;
    failures += sc_test_expect_true("routine match after cooldown", sc_routine_matches(&routine, &event));
    failures += sc_test_expect_status("routine delivery", fake_delivery_new(&fake, &delivery), SC_OK);
    failures += sc_test_expect_status("routine dispatch", sc_routine_dispatch(&routine, delivery, &event), SC_OK);
    failures += sc_test_expect_true("routine delivered", fake.calls == 1 && strcmp(fake.last.ptr, "respond") == 0);

    sc_delivery_target_destroy(delivery);
    sc_string_clear(&fake.last);
    sc_routine_clear(&routine);
    return failures;
}

static int test_heartbeat_persistence(void)
{
    int failures = 0;
    sc_heartbeat_state state = {0};
    sc_heartbeat_state loaded = {0};
    fake_delivery fake = {0};
    sc_delivery_target *delivery = nullptr;
    sc_string path = {0};

    sc_heartbeat_state_init(&state, sc_allocator_heap());
    failures += sc_test_expect_status("fake delivery", fake_delivery_new(&fake, &delivery), SC_OK);
    failures += sc_test_expect_status("heartbeat tick",
                              sc_heartbeat_tick(&state, sc_str_from_cstr("healthy"), delivery),
                              SC_OK);
    failures += sc_test_expect_true("heartbeat ticked", state.tick_count == 1 && state.healthy);
    failures += sc_test_expect_true("heartbeat delivered", fake.calls == 1 && strcmp(fake.last.ptr, "healthy") == 0);
    failures += sc_test_expect_status("heartbeat temp", make_temp_path("heartbeat.state", &path), SC_OK);
    failures += sc_test_expect_status("heartbeat write", sc_heartbeat_state_write(sc_string_as_str(&path), &state), SC_OK);
    failures += sc_test_expect_status("heartbeat read", sc_heartbeat_state_read(sc_allocator_heap(), sc_string_as_str(&path), &loaded), SC_OK);
    failures += sc_test_expect_true("heartbeat persisted",
                            loaded.tick_count == 1 &&
                                loaded.healthy &&
                                strcmp(loaded.message.ptr, "healthy") == 0);

    sc_string_clear(&path);
    sc_heartbeat_state_clear(&loaded);
    sc_delivery_target_destroy(delivery);
    sc_string_clear(&fake.last);
    sc_heartbeat_state_clear(&state);
    return failures;
}

static int test_bootstrap_config_driven_runtime_assembly(void)
{
    int failures = 0;
    sc_string config_path = {0};
    sc_string workspace = {0};
    sc_string heartbeat_path = {0};
    sc_string memory_dir = {0};
    sc_string memory_db = {0};
    sc_string session_dir = {0};
    sc_string session_db = {0};
    sc_string receipts_dir = {0};
    sc_string cache_dir = {0};
    sc_string state_dir = {0};
    sc_string estop_path = {0};
    sc_status boot_status = {0};
    const char *config_template =
        "schema_version = 2\n"
        "[provider]\n"
        "default = \"mock\"\n"
        "default_model = \"mock\"\n"
        "[memory]\n"
        "backend = \"sqlite\"\n"
        "[channels]\n"
        "session_persistence = true\n"
        "session_backend = \"sqlite\"\n"
        "[channels.webhook]\n"
        "enabled = true\n"
        "port = 18081\n"
        "[gateway]\n"
        "enabled = true\n"
        "bind = \"127.0.0.1\"\n"
        "port = 8081\n"
        "[cron]\n"
        "enabled = true\n"
        "id = \"boot-cron\"\n"
        "schedule = \"* * * * *\"\n"
        "prompt = \"boot cron\"\n"
        "once = true\n"
        "[heartbeat]\n"
        "enabled = true\n"
        "state_path = \"%s\"\n";
    char body[2048] = {0};
    int written = 0;

    failures += sc_test_expect_status("boot config path", make_temp_path("bootstrap.toml", &config_path), SC_OK);
    failures += sc_test_expect_status("boot workspace", make_temp_path("workspace", &workspace), SC_OK);
    failures += sc_test_expect_status("boot heartbeat path", make_temp_path("bootstrap-heartbeat.state", &heartbeat_path), SC_OK);
    written = snprintf(body, sizeof(body), config_template, heartbeat_path.ptr == nullptr ? "" : heartbeat_path.ptr);
    failures += sc_test_expect_true("boot config format", written > 0 && (size_t)written < sizeof(body));
    failures += sc_test_expect_true("boot config write", write_file(sc_string_as_str(&config_path), body) == 0);
    boot_status = sc_runtime_boot(sc_allocator_heap(),
                                  &(sc_boot_options){
                                      .struct_size = sizeof(sc_boot_options),
                                      .config_path = sc_string_as_str(&config_path),
                                      .workspace_path = sc_string_as_str(&workspace),
                                      .once = true,
                                      .max_polls = 1,
                                  });
    if (boot_status.code == SC_ERR_UNSUPPORTED) {
        sc_status_clear(&boot_status);
    } else {
        failures += sc_test_expect_status("runtime boot", boot_status, SC_OK);
        failures += sc_test_expect_status("memory dir path", temp_child_path(sc_string_as_str(&workspace), "memory", &memory_dir), SC_OK);
        failures += sc_test_expect_status("memory db path", temp_child_path(sc_string_as_str(&memory_dir), "brain.db", &memory_db), SC_OK);
        failures += sc_test_expect_status("session dir path", temp_child_path(sc_string_as_str(&workspace), "sessions", &session_dir), SC_OK);
        failures += sc_test_expect_status("session db path", temp_child_path(sc_string_as_str(&session_dir), "sessions.db", &session_db), SC_OK);
        failures += sc_test_expect_status("receipts dir path", temp_child_path(sc_string_as_str(&workspace), "receipts", &receipts_dir), SC_OK);
        failures += sc_test_expect_status("cache dir path", temp_child_path(sc_string_as_str(&workspace), "cache", &cache_dir), SC_OK);
        failures += sc_test_expect_status("state dir path", temp_child_path(sc_string_as_str(&workspace), "state", &state_dir), SC_OK);
        failures += sc_test_expect_status("estop path", temp_child_path(sc_string_as_str(&state_dir), "emergency_stop.state", &estop_path), SC_OK);
        failures += sc_test_expect_true("heartbeat written", access(heartbeat_path.ptr, F_OK) == 0);
        failures += sc_test_expect_true("memory dir created", access(memory_dir.ptr, F_OK) == 0);
        failures += sc_test_expect_true("memory sqlite created", access(memory_db.ptr, F_OK) == 0);
        failures += sc_test_expect_true("sessions dir created", access(session_dir.ptr, F_OK) == 0);
        failures += sc_test_expect_true("session sqlite created", access(session_db.ptr, F_OK) == 0);
        failures += sc_test_expect_true("receipts dir created", access(receipts_dir.ptr, F_OK) == 0);
        failures += sc_test_expect_true("cache dir created", access(cache_dir.ptr, F_OK) == 0);
        failures += sc_test_expect_true("state dir created", access(state_dir.ptr, F_OK) == 0);
        failures += sc_test_expect_true("estop file created", access(estop_path.ptr, F_OK) == 0);
    }

    sc_string_clear(&estop_path);
    sc_string_clear(&state_dir);
    sc_string_clear(&cache_dir);
    sc_string_clear(&receipts_dir);
    sc_string_clear(&session_db);
    sc_string_clear(&session_dir);
    sc_string_clear(&memory_db);
    sc_string_clear(&memory_dir);
    sc_string_clear(&heartbeat_path);
    sc_string_clear(&workspace);
    sc_string_clear(&config_path);
    return failures;
}

static int test_session_sqlite_disables_jsonl_writes(void)
{
    int failures = 0;
    sc_provider *provider = nullptr;
    sc_agent *agent = nullptr;
    sc_channel *channel = nullptr;
    sc_channel_orchestrator *orchestrator = nullptr;
    sc_channel_process_result result = {0};
    sc_string dir = {0};
    sc_string session_db = {0};
    sc_string jsonl_dir = {0};
    sc_string jsonl_path = {0};
    sc_status status = {0};
    sc_channel_inbound inbound = {
        .struct_size = sizeof(inbound),
        .message_id = sc_str_from_cstr("m1"),
        .channel_name = sc_str_from_cstr("fake"),
        .conversation_id = sc_str_from_cstr("chat"),
        .thread_id = sc_str_from_cstr("thread"),
        .sender_id = sc_str_from_cstr("sender"),
        .text = sc_str_from_cstr("hello"),
    };

    failures += sc_test_expect_status("session temp dir", make_temp_path("session-sqlite", &dir), SC_OK);
    failures += sc_test_expect_status("session db path", temp_child_path(sc_string_as_str(&dir), "sessions.db", &session_db), SC_OK);
    failures += sc_test_expect_status("jsonl dir path", temp_child_path(sc_string_as_str(&dir), "jsonl", &jsonl_dir), SC_OK);
    failures += sc_test_expect_status("jsonl file path",
                              temp_child_path(sc_string_as_str(&jsonl_dir), "fake_chat_sender.jsonl", &jsonl_path),
                              SC_OK);
    (void)mkdir(dir.ptr, 0700);
    (void)mkdir(jsonl_dir.ptr, 0700);

    failures += sc_test_expect_status("session provider",
                              sc_provider_mock_new(sc_allocator_heap(), SC_PROVIDER_MOCK_TEXT, sc_str_from_cstr("reply"), &provider),
                              SC_OK);
    failures += sc_test_expect_status("session agent",
                              sc_agent_new(sc_allocator_heap(),
                                           &(sc_agent_options){.struct_size = sizeof(sc_agent_options), .provider = provider},
                                           &agent),
                              SC_OK);
    failures += sc_test_expect_status("session fake channel", sc_channel_fake_new(sc_allocator_heap(), 0, &channel), SC_OK);
    failures += sc_test_expect_status("session orchestrator",
                              sc_channel_orchestrator_new(sc_allocator_heap(),
                                                          &(sc_channel_orchestrator_options){
                                                              .struct_size = sizeof(sc_channel_orchestrator_options),
                                                              .agent = agent,
                                                              .channels = &channel,
                                                              .channel_count = 1,
                                                              .session_persistence = true,
                                                              .session_db_path = sc_string_as_str(&session_db),
                                                              .session_jsonl_dir = sc_str_from_cstr(""),
                                                          },
                                                          &orchestrator),
                              SC_OK);
    status = sc_channel_orchestrator_process(orchestrator, channel, &inbound, sc_allocator_heap(), &result);
    if (status.code == SC_ERR_UNSUPPORTED) {
        sc_status_clear(&status);
    } else {
        failures += sc_test_expect_status("session process", status, SC_OK);
        failures += sc_test_expect_true("session sqlite written", access(session_db.ptr, F_OK) == 0);
        failures += sc_test_expect_true("session jsonl skipped", access(jsonl_path.ptr, F_OK) != 0);
    }

    sc_channel_process_result_clear(&result);
    sc_channel_orchestrator_destroy(orchestrator);
    sc_channel_destroy(channel);
    sc_agent_destroy(agent);
    sc_provider_destroy(provider);
    sc_string_clear(&jsonl_path);
    sc_string_clear(&jsonl_dir);
    sc_string_clear(&session_db);
    sc_string_clear(&dir);
    return failures;
}

static sc_status fake_deliver(void *impl, const sc_delivery_message *message)
{
    fake_delivery *fake = impl;
    if (fake == nullptr || message == nullptr) {
        return sc_status_invalid_argument("sc.test_delivery.invalid_argument");
    }
    fake->calls += 1;
    sc_string_clear(&fake->last);
    return sc_string_from_str(sc_allocator_heap(), message->content, &fake->last);
}

static void fake_destroy(void *impl)
{
    (void)impl;
}

static sc_status fake_delivery_new(fake_delivery *fake, sc_delivery_target **out)
{
    if (fake == nullptr) {
        return sc_status_invalid_argument("sc.test_delivery.invalid_argument");
    }
    *fake = (fake_delivery){0};
    return sc_delivery_target_new(sc_allocator_heap(), &fake_delivery_vtab, fake, out);
}

static sc_status set_string(sc_string *out, const char *value)
{
    return sc_string_from_cstr(sc_allocator_heap(), value, out);
}

static sc_status make_temp_path(const char *suffix, sc_string *out)
{
    char path[256] = {0};
    static unsigned counter = 0;
    int written = 0;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.test.temp_invalid_argument");
    }
    counter += 1;
    written = snprintf(path, sizeof(path), "/tmp/sc-automation-%ld-%u-%s", (long)getpid(), counter, suffix);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return sc_status_io("sc.test.path_failed");
    }
    return sc_string_from_cstr(sc_allocator_heap(), path, out);
}

static sc_status temp_child_path(sc_str parent, const char *child, sc_string *out)
{
    char path[512] = {0};
    int written = 0;

    if (parent.ptr == nullptr || child == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.test.path_invalid_argument");
    }
    written = snprintf(path, sizeof(path), "%.*s/%s", (int)parent.len, parent.ptr, child);
    if (written <= 0 || (size_t)written >= sizeof(path)) {
        return sc_status_io("sc.test.path_failed");
    }
    return sc_string_from_cstr(sc_allocator_heap(), path, out);
}

static int write_file(sc_str path, const char *text)
{
    FILE *file = fopen(path.ptr, "wb");
    if (file == nullptr) {
        return -1;
    }
    if (fputs(text, file) < 0) {
        (void)fclose(file);
        return -1;
    }
    return fclose(file);
}
