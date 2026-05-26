#pragma once

#include "sc/allocator.h"
#include "sc/api.h"
#include "sc/result.h"
#include "sc/runtime.h"
#include "sc/security.h"
#include "sc/string.h"
#include "sc/time.h"
#include "sc/vector.h"

SC_BEGIN_DECLS

typedef struct sc_delivery_target sc_delivery_target;

typedef enum sc_delivery_kind {
    SC_DELIVERY_CLI = 0,
    SC_DELIVERY_GATEWAY,
    SC_DELIVERY_CHANNEL,
    SC_DELIVERY_SESSION_HISTORY,
    SC_DELIVERY_FAKE
} sc_delivery_kind;

typedef struct sc_delivery_message {
    size_t struct_size;
    sc_delivery_kind kind;
    sc_str target;
    sc_str content;
} sc_delivery_message;

typedef struct sc_delivery_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    sc_status (*deliver)(void *impl, const sc_delivery_message *message);
    void (*destroy)(void *impl);
} sc_delivery_vtab;

sc_status sc_delivery_target_new(sc_allocator *alloc,
                                 const sc_delivery_vtab *vtab,
                                 void *impl,
                                 sc_delivery_target **out);
sc_status sc_delivery_deliver(sc_delivery_target *target, const sc_delivery_message *message);
void sc_delivery_target_destroy(sc_delivery_target *target);
sc_status sc_delivery_stdout_new(sc_allocator *alloc, sc_delivery_target **out);

typedef struct sc_cron_schedule {
    size_t struct_size;
    int minute;
    int minute_step;
    int hour;
    int day_of_month;
    int month;
    int day_of_week;
    char nanocron_expression[513];
} sc_cron_schedule;

typedef enum sc_cron_job_kind {
    SC_CRON_JOB_AGENT = 0,
    SC_CRON_JOB_SHELL
} sc_cron_job_kind;

typedef struct sc_cron_job {
    size_t struct_size;
    sc_string id;
    sc_cron_schedule schedule;
    sc_cron_job_kind kind;
    sc_string command;
    sc_string delivery_target;
    bool enabled;
    bool once;
    bool cancel_requested;
    sc_wall_time last_run_at;
    sc_string schedule_text;
} sc_cron_job;

typedef struct sc_cron_run_record {
    size_t struct_size;
    sc_string job_id;
    sc_status_code status_code;
    sc_string output;
    sc_wall_time started_at;
    sc_wall_time ended_at;
} sc_cron_run_record;

typedef struct sc_cron_job_store {
    sc_allocator *alloc;
    sc_vec jobs;
} sc_cron_job_store;

typedef struct sc_cron_run_store {
    sc_allocator *alloc;
    sc_vec records;
} sc_cron_run_store;

sc_status sc_cron_schedule_parse(sc_str expression, sc_cron_schedule *out);
bool sc_cron_schedule_matches(const sc_cron_schedule *schedule, sc_wall_time when);
sc_status sc_cron_schedule_next_after(const sc_cron_schedule *schedule, sc_wall_time after, sc_wall_time *out);
sc_status sc_cron_schedule_format(const sc_cron_schedule *schedule, sc_allocator *alloc, sc_string *out);
void sc_cron_job_clear(sc_cron_job *job);
void sc_cron_run_record_clear(sc_cron_run_record *record);
void sc_cron_job_store_init(sc_cron_job_store *store, sc_allocator *alloc);
sc_status sc_cron_job_store_put(sc_cron_job_store *store, const sc_cron_job *job);
sc_cron_job *sc_cron_job_store_find(sc_cron_job_store *store, sc_str id);
sc_status sc_cron_job_store_remove(sc_cron_job_store *store, sc_str id);
size_t sc_cron_job_store_len(const sc_cron_job_store *store);
sc_status sc_cron_job_store_load_file(sc_cron_job_store *store, sc_str path);
sc_status sc_cron_job_store_save_file(const sc_cron_job_store *store, sc_str path);
void sc_cron_job_store_clear(sc_cron_job_store *store);
void sc_cron_run_store_init(sc_cron_run_store *store, sc_allocator *alloc);
sc_status sc_cron_run_store_append(sc_cron_run_store *store, const sc_cron_run_record *record);
size_t sc_cron_run_store_len(const sc_cron_run_store *store);
void sc_cron_run_store_clear(sc_cron_run_store *store);
bool sc_cron_job_is_due(const sc_cron_job *job, sc_wall_time now);
sc_status sc_cron_job_validate_shell(const sc_cron_job *job,
                                     const sc_security_policy *policy,
                                     const sc_estop_state *estop);
sc_status sc_cron_job_execute(sc_cron_job *job,
                              sc_agent *agent,
                              sc_delivery_target *delivery,
                              sc_cron_run_store *runs,
                              sc_allocator *alloc);

typedef enum sc_sop_step_action {
    SC_SOP_STEP_MANUAL = 0,
    SC_SOP_STEP_TOOL,
    SC_SOP_STEP_AGENT
} sc_sop_step_action;

typedef struct sc_sop_step {
    size_t struct_size;
    sc_string name;
    sc_sop_step_action action;
    sc_string target;
    sc_string args;
    bool requires_approval;
} sc_sop_step;

typedef struct sc_sop_document {
    sc_allocator *alloc;
    sc_string title;
    sc_vec steps;
} sc_sop_document;

typedef struct sc_sop_run_state {
    size_t struct_size;
    size_t current_step;
    bool waiting_approval;
    bool completed;
} sc_sop_run_state;

void sc_sop_document_init(sc_sop_document *document, sc_allocator *alloc);
sc_status sc_sop_markdown_parse(sc_allocator *alloc, sc_str markdown, sc_sop_document *out);
sc_status sc_sop_markdown_load(sc_allocator *alloc, sc_str path, sc_sop_document *out);
void sc_sop_step_clear(sc_sop_step *step);
void sc_sop_document_clear(sc_sop_document *document);
void sc_sop_run_start(sc_sop_run_state *run);
const sc_sop_step *sc_sop_run_current_step(const sc_sop_document *document, const sc_sop_run_state *run);
sc_status sc_sop_run_advance(sc_sop_run_state *run, const sc_sop_document *document, sc_audit_chain *audit);
sc_status sc_sop_run_approve_manual(sc_sop_run_state *run, const sc_sop_document *document, sc_audit_chain *audit);
bool sc_sop_condition_evaluate(sc_str expected, sc_str actual);

typedef struct sc_routine {
    size_t struct_size;
    sc_string id;
    sc_string event_name;
    sc_string dispatch_target;
    sc_string prompt;
    bool enabled;
    int64_t cooldown_ms;
    sc_wall_time last_matched_at;
} sc_routine;

typedef struct sc_routine_event {
    size_t struct_size;
    sc_str name;
    sc_str payload;
    sc_wall_time occurred_at;
} sc_routine_event;

void sc_routine_clear(sc_routine *routine);
sc_status sc_routine_init(sc_routine *routine,
                          sc_allocator *alloc,
                          sc_str id,
                          sc_str event_name,
                          sc_str dispatch_target,
                          sc_str prompt);
bool sc_routine_matches(sc_routine *routine, const sc_routine_event *event);
sc_status sc_routine_dispatch(const sc_routine *routine,
                              sc_delivery_target *delivery,
                              const sc_routine_event *event);

typedef struct sc_heartbeat_state {
    size_t struct_size;
    sc_allocator *alloc;
    sc_wall_time last_tick_at;
    uint64_t tick_count;
    bool healthy;
    sc_string message;
} sc_heartbeat_state;

void sc_heartbeat_state_init(sc_heartbeat_state *state, sc_allocator *alloc);
sc_status sc_heartbeat_tick(sc_heartbeat_state *state, sc_str message, sc_delivery_target *delivery);
sc_status sc_heartbeat_state_write(sc_str path, const sc_heartbeat_state *state);
sc_status sc_heartbeat_state_read(sc_allocator *alloc, sc_str path, sc_heartbeat_state *out);
void sc_heartbeat_state_clear(sc_heartbeat_state *state);

SC_END_DECLS
