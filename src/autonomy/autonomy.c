#include "sc/autonomy.h"

#include "sc/contract.h"

#ifdef SC_HAVE_NANOCRON
#include "nanocron/nanocron.h"
#endif

#include <ctype.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

enum {
    SC_CRON_STATE_MAX_BYTES = 1'048'576
};

struct sc_delivery_target {
    sc_allocator *alloc;
    const sc_delivery_vtab *vtab;
    void *impl;
};

static bool delivery_vtab_valid(const sc_delivery_vtab *vtab);
static sc_str trim(sc_str input);
static bool str_starts_with(sc_str value, const char *prefix);
static sc_status copy_string(sc_allocator *alloc, sc_str value, sc_string *out);
static sc_status replace_string(sc_string *target, sc_allocator *alloc, sc_str value);
static sc_status parse_field(sc_str field, int min, int max, int *out);
static sc_status parse_minute_field(sc_str field, int *minute, int *step);
static sc_status cron_schedule_parse_legacy(sc_str expression, sc_cron_schedule *out);
static bool field_matches(int expected, int actual);
static bool minute_matches(const sc_cron_schedule *schedule, int minute);
#ifdef SC_HAVE_NANOCRON
static sc_status cron_schedule_normalize(sc_str expression, char out[sizeof(((sc_cron_schedule *)0)->nanocron_expression)]);
static size_t cron_schedule_count_fields(sc_str expression);
static bool cron_schedule_to_timespec(sc_wall_time time, struct timespec *out);
static sc_wall_time cron_schedule_from_timespec(const struct timespec *time);
static void cron_noop_callback(void *user_data, const struct timespec *trigger_time);
static void cron_match_callback(void *user_data, const struct timespec *trigger_time);
static bool cron_schedule_nanocron_matches(const sc_cron_schedule *schedule, sc_wall_time when);
static sc_status cron_schedule_nanocron_next_after(const sc_cron_schedule *schedule, sc_wall_time after, sc_wall_time *out);
#endif
static void cron_job_move_assign(sc_cron_job *dst, sc_cron_job *src);
static sc_status cron_job_copy(sc_allocator *alloc, const sc_cron_job *job, sc_cron_job *out);
static sc_status cron_run_record_copy(sc_allocator *alloc, const sc_cron_run_record *record, sc_cron_run_record *out);
static sc_status cron_store_read_all(sc_allocator *alloc, FILE *file, sc_string *out);
static sc_status cron_store_parse_body(sc_cron_job_store *store, sc_str body);
static sc_status cron_store_parse_job_line(sc_cron_job_store *store, sc_str line);
static sc_status cron_store_write_escaped(FILE *file, sc_str value);
static sc_status cron_store_decode_escaped(sc_allocator *alloc, sc_str value, sc_string *out);
static bool cron_store_parse_bool(sc_str value, bool *out);
static bool cron_store_parse_i64(sc_allocator *alloc, sc_str value, int64_t *out);
static int cron_store_hex_value(char ch);
static sc_status cron_run_store_append_status(sc_cron_run_store *store,
                                              sc_str job_id,
                                              sc_status_code code,
                                              sc_str output,
                                              sc_wall_time started,
                                              sc_wall_time ended);
static sc_status stdout_deliver(void *impl, const sc_delivery_message *message);
static void stdout_destroy(void *impl);
static sc_status sop_step_copy(sc_allocator *alloc, const sc_sop_step *step, sc_sop_step *out);
static sc_status sop_document_push_step(sc_sop_document *document, const sc_sop_step *step);
static sc_status sop_parse_key_value(sc_sop_step *step, sc_allocator *alloc, sc_str key, sc_str value);
static sc_sop_step_action sop_action_from_str(sc_str value);
static sc_status heartbeat_message_replace(sc_heartbeat_state *state, sc_str message);
static sc_status path_to_cstr(sc_allocator *alloc, sc_str path, sc_string *out);

static const sc_delivery_vtab stdout_delivery_vtab = {
    .struct_size = sizeof(sc_delivery_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "stdout",
    .deliver = stdout_deliver,
    .destroy = stdout_destroy,
};

sc_status sc_delivery_target_new(sc_allocator *alloc,
                                 const sc_delivery_vtab *vtab,
                                 void *impl,
                                 sc_delivery_target **out)
{
    sc_delivery_target *target = nullptr;

    if (out == nullptr || !delivery_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.delivery.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    target = sc_alloc(alloc, sizeof(*target), _Alignof(sc_delivery_target));
    if (target == nullptr) {
        return sc_status_no_memory();
    }
    *target = (sc_delivery_target){.alloc = alloc, .vtab = vtab, .impl = impl};
    *out = target;
    return sc_status_ok();
}

sc_status sc_delivery_deliver(sc_delivery_target *target, const sc_delivery_message *message)
{
    if (target == nullptr || message == nullptr || target->vtab == nullptr || target->vtab->deliver == nullptr) {
        return sc_status_invalid_argument("sc.delivery.invalid_argument");
    }
    return target->vtab->deliver(target->impl, message);
}

void sc_delivery_target_destroy(sc_delivery_target *target)
{
    sc_allocator *alloc = nullptr;
    if (target == nullptr) {
        return;
    }
    alloc = target->alloc == nullptr ? sc_allocator_heap() : target->alloc;
    if (target->vtab != nullptr && target->vtab->destroy != nullptr) {
        target->vtab->destroy(target->impl);
    }
    sc_free(alloc, target, sizeof(*target), _Alignof(sc_delivery_target));
}

sc_status sc_delivery_stdout_new(sc_allocator *alloc, sc_delivery_target **out)
{
    return sc_delivery_target_new(alloc, &stdout_delivery_vtab, nullptr, out);
}

sc_status sc_cron_schedule_parse(sc_str expression, sc_cron_schedule *out)
{
#ifdef SC_HAVE_NANOCRON
    sc_cron_schedule parsed = {0};
    char normalized[sizeof(parsed.nanocron_expression)] = {0};
    cron_ctx_t *ctx = nullptr;
    const cron_job_t *job = nullptr;
    sc_status status;
    size_t field_count = 0;

    if (out == nullptr || expression.ptr == nullptr || expression.len == 0) {
        return sc_status_invalid_argument("sc.cron_schedule.invalid_argument");
    }

    status = cron_schedule_normalize(expression, normalized);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    ctx = cron_create();
    if (ctx == nullptr) {
        return sc_status_no_memory();
    }
    job = cron_add(ctx, normalized, cron_noop_callback, nullptr);
    if (job == nullptr) {
        cron_destroy(ctx);
        return sc_status_parse("sc.cron_schedule.parse");
    }
    cron_destroy(ctx);

    parsed = (sc_cron_schedule){
        .struct_size = sizeof(parsed),
        .minute = -1,
        .minute_step = 0,
        .hour = -1,
        .day_of_month = -1,
        .month = -1,
        .day_of_week = -1,
    };
    field_count = cron_schedule_count_fields(expression);
    if (field_count == 5) {
        status = cron_schedule_parse_legacy(expression, &parsed);
        if (!sc_status_is_ok(status)) {
            return status;
        }
    }
    (void)snprintf(parsed.nanocron_expression, sizeof(parsed.nanocron_expression), "%s", normalized);
    *out = parsed;
    return sc_status_ok();
#else
    return cron_schedule_parse_legacy(expression, out);
#endif
}

static sc_status cron_schedule_parse_legacy(sc_str expression, sc_cron_schedule *out)
{
    sc_allocator *alloc = sc_allocator_heap();
    sc_string copy = {0};
    sc_str original = {0};
    char *save = nullptr;
    char *fields[5] = {0};
    sc_status status;

    if (out == nullptr || expression.ptr == nullptr || expression.len == 0) {
        return sc_status_invalid_argument("sc.cron_schedule.invalid_argument");
    }
    original = trim(expression);
    status = sc_string_from_str(alloc, expression, &copy);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    fields[0] = strtok_r(copy.ptr, " \t", &save);
    for (size_t i = 1; i < 5 && fields[i - 1] != nullptr; i += 1) {
        fields[i] = strtok_r(nullptr, " \t", &save);
    }
    if (fields[0] == nullptr || fields[1] == nullptr || fields[2] == nullptr || fields[3] == nullptr || fields[4] == nullptr ||
        strtok_r(nullptr, " \t", &save) != nullptr) {
        sc_string_clear(&copy);
        return sc_status_parse("sc.cron_schedule.field_count");
    }

    *out = (sc_cron_schedule){.struct_size = sizeof(*out), .minute = -1, .minute_step = 0, .hour = -1, .day_of_month = -1, .month = -1, .day_of_week = -1};
    status = parse_minute_field(sc_str_from_cstr(fields[0]), &out->minute, &out->minute_step);
    if (sc_status_is_ok(status)) {
        status = parse_field(sc_str_from_cstr(fields[1]), 0, 23, &out->hour);
    }
    if (sc_status_is_ok(status)) {
        status = parse_field(sc_str_from_cstr(fields[2]), 1, 31, &out->day_of_month);
    }
    if (sc_status_is_ok(status)) {
        status = parse_field(sc_str_from_cstr(fields[3]), 1, 12, &out->month);
    }
    if (sc_status_is_ok(status)) {
        status = parse_field(sc_str_from_cstr(fields[4]), 0, 6, &out->day_of_week);
    }
    if (sc_status_is_ok(status)) {
        int written = 0;
        if (original.len >= sizeof(out->nanocron_expression)) {
            status = sc_status_parse("sc.cron_schedule.too_long");
        }
        if (sc_status_is_ok(status)) {
            written = snprintf(out->nanocron_expression,
                               sizeof(out->nanocron_expression),
                               "%.*s",
                               (int)original.len,
                               original.ptr);
        }
        if (written < 0 || (size_t)written >= sizeof(out->nanocron_expression)) {
            status = sc_status_parse("sc.cron_schedule.too_long");
        }
    }
    sc_string_clear(&copy);
    return status;
}

bool sc_cron_schedule_matches(const sc_cron_schedule *schedule, sc_wall_time when)
{
    time_t seconds = (time_t)(when.unix_ns / 1000000000);
    struct tm tm = {0};

#ifdef SC_HAVE_NANOCRON
    if (schedule != nullptr && schedule->nanocron_expression[0] != '\0') {
        return cron_schedule_nanocron_matches(schedule, when);
    }
#endif

    if (schedule == nullptr || gmtime_r(&seconds, &tm) == nullptr) {
        return false;
    }
    return minute_matches(schedule, tm.tm_min) &&
           field_matches(schedule->hour, tm.tm_hour) &&
           field_matches(schedule->day_of_month, tm.tm_mday) &&
           field_matches(schedule->month, tm.tm_mon + 1) &&
           field_matches(schedule->day_of_week, tm.tm_wday);
}

sc_status sc_cron_schedule_next_after(const sc_cron_schedule *schedule, sc_wall_time after, sc_wall_time *out)
{
    int64_t minute_ns = 60LL * 1000000000LL;
    int64_t cursor = ((after.unix_ns / minute_ns) + 1) * minute_ns;
    int64_t limit = cursor + (366LL * 24LL * 60LL * minute_ns);

    if (schedule == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.cron_schedule.invalid_argument");
    }
#ifdef SC_HAVE_NANOCRON
    if (schedule->nanocron_expression[0] != '\0') {
        return cron_schedule_nanocron_next_after(schedule, after, out);
    }
#endif
    while (cursor <= limit) {
        sc_wall_time candidate = {.unix_ns = cursor};
        if (sc_cron_schedule_matches(schedule, candidate)) {
            *out = candidate;
            return sc_status_ok();
        }
        cursor += minute_ns;
    }
    return sc_status_timeout("sc.cron_schedule.next_not_found");
}

sc_status sc_cron_schedule_format(const sc_cron_schedule *schedule, sc_allocator *alloc, sc_string *out)
{
    char text[640] = {0};
    char minute[32] = {0};
    char hour[32] = {0};
    char day_of_month[32] = {0};
    char month[32] = {0};
    char day_of_week[32] = {0};
    int written = 0;

    if (schedule == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.cron_schedule.format_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    if (schedule->nanocron_expression[0] != '\0') {
        return sc_string_from_cstr(alloc, schedule->nanocron_expression, out);
    }

    if (schedule->minute_step > 0) {
        written = snprintf(minute, sizeof(minute), "*/%d", schedule->minute_step);
    } else if (schedule->minute < 0) {
        written = snprintf(minute, sizeof(minute), "*");
    } else {
        written = snprintf(minute, sizeof(minute), "%d", schedule->minute);
    }
    if (written < 0 || (size_t)written >= sizeof(minute)) {
        return sc_status_parse("sc.cron_schedule.format_failed");
    }
    if (schedule->hour < 0) {
        written = snprintf(hour, sizeof(hour), "*");
    } else {
        written = snprintf(hour, sizeof(hour), "%d", schedule->hour);
    }
    if (written < 0 || (size_t)written >= sizeof(hour)) {
        return sc_status_parse("sc.cron_schedule.format_failed");
    }
    if (schedule->day_of_month < 0) {
        written = snprintf(day_of_month, sizeof(day_of_month), "*");
    } else {
        written = snprintf(day_of_month, sizeof(day_of_month), "%d", schedule->day_of_month);
    }
    if (written < 0 || (size_t)written >= sizeof(day_of_month)) {
        return sc_status_parse("sc.cron_schedule.format_failed");
    }
    if (schedule->month < 0) {
        written = snprintf(month, sizeof(month), "*");
    } else {
        written = snprintf(month, sizeof(month), "%d", schedule->month);
    }
    if (written < 0 || (size_t)written >= sizeof(month)) {
        return sc_status_parse("sc.cron_schedule.format_failed");
    }
    if (schedule->day_of_week < 0) {
        written = snprintf(day_of_week, sizeof(day_of_week), "*");
    } else {
        written = snprintf(day_of_week, sizeof(day_of_week), "%d", schedule->day_of_week);
    }
    if (written < 0 || (size_t)written >= sizeof(day_of_week)) {
        return sc_status_parse("sc.cron_schedule.format_failed");
    }

    written = snprintf(text, sizeof(text), "%s %s %s %s %s", minute, hour, day_of_month, month, day_of_week);
    if (written < 0 || (size_t)written >= sizeof(text)) {
        return sc_status_parse("sc.cron_schedule.format_failed");
    }
    return sc_string_from_cstr(alloc, text, out);
}

void sc_cron_job_clear(sc_cron_job *job)
{
    if (job == nullptr) {
        return;
    }
    sc_string_clear(&job->id);
    sc_string_clear(&job->command);
    sc_string_clear(&job->delivery_target);
    sc_string_clear(&job->schedule_text);
    *job = (sc_cron_job){0};
}

void sc_cron_run_record_clear(sc_cron_run_record *record)
{
    if (record == nullptr) {
        return;
    }
    sc_string_clear(&record->job_id);
    sc_string_clear(&record->output);
    *record = (sc_cron_run_record){0};
}

void sc_cron_job_store_init(sc_cron_job_store *store, sc_allocator *alloc)
{
    if (store == nullptr) {
        return;
    }
    store->alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_vec_init(&store->jobs, store->alloc, sizeof(sc_cron_job));
}

sc_status sc_cron_job_store_put(sc_cron_job_store *store, const sc_cron_job *job)
{
    sc_cron_job copy = {0};
    sc_status status;

    if (store == nullptr || job == nullptr || job->id.len == 0) {
        return sc_status_invalid_argument("sc.cron_job_store.invalid_argument");
    }
    status = cron_job_copy(store->alloc, job, &copy);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    for (size_t i = 0; i < store->jobs.len; i += 1) {
        sc_cron_job *existing = sc_vec_at(&store->jobs, i);
        if (existing != nullptr && sc_str_equal(sc_string_as_str(&existing->id), sc_string_as_str(&job->id))) {
            sc_cron_job_clear(existing);
            cron_job_move_assign(existing, &copy);
            return sc_status_ok();
        }
    }
    status = sc_vec_push(&store->jobs, &copy);
    if (!sc_status_is_ok(status)) {
        sc_cron_job_clear(&copy);
    }
    return status;
}

sc_cron_job *sc_cron_job_store_find(sc_cron_job_store *store, sc_str id)
{
    if (store == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < store->jobs.len; i += 1) {
        sc_cron_job *job = sc_vec_at(&store->jobs, i);
        if (job != nullptr && sc_str_equal(sc_string_as_str(&job->id), id)) {
            return job;
        }
    }
    return nullptr;
}

sc_status sc_cron_job_store_remove(sc_cron_job_store *store, sc_str id)
{
    if (store == nullptr || id.ptr == nullptr || id.len == 0) {
        return sc_status_invalid_argument("sc.cron_job_store.remove_invalid_argument");
    }
    for (size_t i = 0; i < store->jobs.len; i += 1) {
        sc_cron_job *job = sc_vec_at(&store->jobs, i);
        if (job != nullptr && sc_str_equal(sc_string_as_str(&job->id), id)) {
            sc_cron_job_clear(job);
            if (i + 1 < store->jobs.len) {
                unsigned char *base = store->jobs.ptr;
                (void)memmove(base + i * store->jobs.item_size,
                              base + (i + 1) * store->jobs.item_size,
                              (store->jobs.len - i - 1) * store->jobs.item_size);
            }
            store->jobs.len -= 1;
            return sc_status_ok();
        }
    }
    return sc_status_invalid_argument("sc.cron_job_store.not_found");
}

size_t sc_cron_job_store_len(const sc_cron_job_store *store)
{
    return store == nullptr ? 0 : store->jobs.len;
}

sc_status sc_cron_job_store_load_file(sc_cron_job_store *store, sc_str path)
{
    sc_string path_cstr = {0};
    sc_string body = {0};
    FILE *file = nullptr;
    sc_status status;

    if (store == nullptr || store->jobs.item_size != sizeof(sc_cron_job) || path.ptr == nullptr || path.len == 0) {
        return sc_status_invalid_argument("sc.cron_job_store.load_invalid_argument");
    }

    status = path_to_cstr(store->alloc == nullptr ? sc_allocator_heap() : store->alloc, path, &path_cstr);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    file = fopen(path_cstr.ptr, "rb");
    if (file == nullptr) {
        if (errno == ENOENT) {
            status = sc_status_ok();
        } else {
            status = sc_status_io("sc.cron_job_store.open_failed");
        }
        goto cleanup;
    }
    status = cron_store_read_all(store->alloc == nullptr ? sc_allocator_heap() : store->alloc, file, &body);
    if (sc_status_is_ok(status)) {
        status = cron_store_parse_body(store, sc_string_as_str(&body));
    }

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    sc_string_clear(&body);
    sc_string_clear(&path_cstr);
    return status;
}

sc_status sc_cron_job_store_save_file(const sc_cron_job_store *store, sc_str path)
{
    sc_allocator *alloc = nullptr;
    sc_string path_cstr = {0};
    FILE *file = nullptr;
    sc_status status;

    if (store == nullptr || store->jobs.item_size != sizeof(sc_cron_job) || path.ptr == nullptr || path.len == 0) {
        return sc_status_invalid_argument("sc.cron_job_store.save_invalid_argument");
    }
    alloc = store->alloc == nullptr ? sc_allocator_heap() : store->alloc;
    status = path_to_cstr(alloc, path, &path_cstr);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    file = fopen(path_cstr.ptr, "wb");
    if (file == nullptr) {
        status = sc_status_io("sc.cron_job_store.create_failed");
        goto cleanup;
    }
    if (fputs("sc-cron-jobs-v1\n", file) == EOF) {
        status = sc_status_io("sc.cron_job_store.write_failed");
        goto cleanup;
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < store->jobs.len; i += 1) {
        const sc_cron_job *job = sc_vec_at_const(&store->jobs, i);
        sc_string schedule = {0};
        sc_str schedule_value = {0};
        char last_run[48] = {0};
        int written = 0;

        if (job == nullptr) {
            status = sc_status_invalid_argument("sc.cron_job_store.job_invalid");
            break;
        }
        if (job->schedule_text.len > 0) {
            schedule_value = sc_string_as_str(&job->schedule_text);
        } else {
            status = sc_cron_schedule_format(&job->schedule, alloc, &schedule);
            schedule_value = sc_string_as_str(&schedule);
        }
        written = snprintf(last_run, sizeof(last_run), "%" PRId64, job->last_run_at.unix_ns);
        if (sc_status_is_ok(status) && (written < 0 || (size_t)written >= sizeof(last_run))) {
            status = sc_status_io("sc.cron_job_store.write_failed");
        }
        if (sc_status_is_ok(status) &&
            fprintf(file,
                    "job|%s|%s|%s|%s|",
                    job->kind == SC_CRON_JOB_SHELL ? "shell" : "agent",
                    job->enabled ? "true" : "false",
                    job->once ? "true" : "false",
                    last_run) < 0) {
            status = sc_status_io("sc.cron_job_store.write_failed");
        }
        if (sc_status_is_ok(status)) {
            status = cron_store_write_escaped(file, sc_string_as_str(&job->id));
        }
        if (sc_status_is_ok(status) && fputc('|', file) == EOF) {
            status = sc_status_io("sc.cron_job_store.write_failed");
        }
        if (sc_status_is_ok(status)) {
            status = cron_store_write_escaped(file, schedule_value);
        }
        if (sc_status_is_ok(status) && fputc('|', file) == EOF) {
            status = sc_status_io("sc.cron_job_store.write_failed");
        }
        if (sc_status_is_ok(status)) {
            status = cron_store_write_escaped(file, sc_string_as_str(&job->delivery_target));
        }
        if (sc_status_is_ok(status) && fputc('|', file) == EOF) {
            status = sc_status_io("sc.cron_job_store.write_failed");
        }
        if (sc_status_is_ok(status)) {
            status = cron_store_write_escaped(file, sc_string_as_str(&job->command));
        }
        if (sc_status_is_ok(status) && fputc('\n', file) == EOF) {
            status = sc_status_io("sc.cron_job_store.write_failed");
        }
        sc_string_clear(&schedule);
    }
    if (fclose(file) != 0 && sc_status_is_ok(status)) {
        status = sc_status_io("sc.cron_job_store.close_failed");
    }
    file = nullptr;

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    sc_string_clear(&path_cstr);
    return status;
}

void sc_cron_job_store_clear(sc_cron_job_store *store)
{
    if (store == nullptr) {
        return;
    }
    for (size_t i = 0; i < store->jobs.len; i += 1) {
        sc_cron_job *job = sc_vec_at(&store->jobs, i);
        sc_cron_job_clear(job);
    }
    sc_vec_clear(&store->jobs);
    *store = (sc_cron_job_store){0};
}

void sc_cron_run_store_init(sc_cron_run_store *store, sc_allocator *alloc)
{
    if (store == nullptr) {
        return;
    }
    store->alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_vec_init(&store->records, store->alloc, sizeof(sc_cron_run_record));
}

sc_status sc_cron_run_store_append(sc_cron_run_store *store, const sc_cron_run_record *record)
{
    sc_cron_run_record copy = {0};
    sc_status status;

    if (store == nullptr || record == nullptr) {
        return sc_status_invalid_argument("sc.cron_run_store.invalid_argument");
    }
    status = cron_run_record_copy(store->alloc, record, &copy);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&store->records, &copy);
    }
    if (!sc_status_is_ok(status)) {
        sc_cron_run_record_clear(&copy);
    }
    return status;
}

size_t sc_cron_run_store_len(const sc_cron_run_store *store)
{
    return store == nullptr ? 0 : store->records.len;
}

void sc_cron_run_store_clear(sc_cron_run_store *store)
{
    if (store == nullptr) {
        return;
    }
    for (size_t i = 0; i < store->records.len; i += 1) {
        sc_cron_run_record *record = sc_vec_at(&store->records, i);
        sc_cron_run_record_clear(record);
    }
    sc_vec_clear(&store->records);
    *store = (sc_cron_run_store){0};
}

bool sc_cron_job_is_due(const sc_cron_job *job, sc_wall_time now)
{
    if (job == nullptr || !job->enabled) {
        return false;
    }
    if (job->once && job->last_run_at.unix_ns != 0) {
        return false;
    }
    return sc_cron_schedule_matches(&job->schedule, now);
}

sc_status sc_cron_job_validate_shell(const sc_cron_job *job,
                                     const sc_security_policy *policy,
                                     const sc_estop_state *estop)
{
    bool approval_required = false;
    sc_security_tool_request request = {0};
    sc_status status;

    if (job == nullptr || job->kind != SC_CRON_JOB_SHELL) {
        return sc_status_invalid_argument("sc.cron_job.shell_invalid_argument");
    }
    request = (sc_security_tool_request){
        .struct_size = sizeof(request),
        .tool_name = sc_str_from_cstr("cron_shell"),
        .risk = SC_TOOL_RISK_SHELL,
        .shell_arg = sc_string_as_str(&job->command),
    };
    status = sc_security_validate_request(policy, estop, &request, &approval_required);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (approval_required) {
        return sc_status_cancelled("sc.cron_job.shell_approval_required");
    }
    return sc_status_ok();
}

sc_status sc_cron_job_execute(sc_cron_job *job,
                              sc_agent *agent,
                              sc_delivery_target *delivery,
                              sc_cron_run_store *runs,
                              sc_allocator *alloc)
{
    sc_wall_time started = {0};
    sc_wall_time ended = {0};
    sc_status status;
    sc_str output = sc_str_from_cstr("");
    sc_agent_turn_result turn_result = {0};

    if (job == nullptr || runs == nullptr) {
        return sc_status_invalid_argument("sc.cron_job.execute_invalid_argument");
    }
    if (!job->enabled) {
        return sc_status_ok();
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    (void)sc_clock_wall(&started);
    if (job->kind == SC_CRON_JOB_AGENT) {
        sc_turn turn = {
            .struct_size = sizeof(turn),
            .input = sc_string_as_str(&job->command),
            .session_id = sc_string_as_str(&job->id),
            .cancel_requested = job->cancel_requested,
        };
        if (agent == nullptr) {
            status = sc_status_invalid_argument("sc.cron_job.agent_missing");
        } else {
            status = sc_agent_process_message(agent, &turn, alloc, &turn_result);
            output = sc_string_as_str(&turn_result.output);
        }
    } else {
        status = sc_status_unsupported("sc.cron_job.shell_execute_unsupported");
        output = sc_str_from_cstr("shell execution is unsupported");
    }
    (void)sc_clock_wall(&ended);
    if (sc_status_is_ok(status) && delivery != nullptr) {
        sc_delivery_message message = {
            .struct_size = sizeof(message),
            .kind = SC_DELIVERY_SESSION_HISTORY,
            .target = sc_string_as_str(&job->delivery_target),
            .content = output,
        };
        status = sc_delivery_deliver(delivery, &message);
    }
    if (sc_status_is_ok(status)) {
        job->last_run_at = ended;
    }
    (void)cron_run_store_append_status(runs, sc_string_as_str(&job->id), status.code, output, started, ended);
    sc_agent_turn_result_clear(&turn_result);
    return status;
}

void sc_sop_document_init(sc_sop_document *document, sc_allocator *alloc)
{
    if (document == nullptr) {
        return;
    }
    document->alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_vec_init(&document->steps, document->alloc, sizeof(sc_sop_step));
}

sc_status sc_sop_markdown_parse(sc_allocator *alloc, sc_str markdown, sc_sop_document *out)
{
    sc_sop_step current = {.struct_size = sizeof(current), .action = SC_SOP_STEP_MANUAL};
    bool have_step = false;
    sc_status status = sc_status_ok();
    size_t offset = 0;

    if (out == nullptr || markdown.ptr == nullptr) {
        return sc_status_invalid_argument("sc.sop.parse_invalid_argument");
    }
    sc_sop_document_init(out, alloc);
    while (sc_status_is_ok(status) && offset <= markdown.len) {
        size_t start = offset;
        sc_str line = {0};
        while (offset < markdown.len && markdown.ptr[offset] != '\n') {
            offset += 1;
        }
        line = trim(sc_str_from_parts(&markdown.ptr[start], offset - start));
        if (offset < markdown.len) {
            offset += 1;
        } else if (start == markdown.len) {
            break;
        }

        if (str_starts_with(line, "# ") && !str_starts_with(line, "## ")) {
            status = replace_string(&out->title, out->alloc, trim(sc_str_from_parts(line.ptr + 2, line.len - 2)));
        } else if (str_starts_with(line, "## ")) {
            if (have_step) {
                status = sop_document_push_step(out, &current);
                sc_sop_step_clear(&current);
                current = (sc_sop_step){.struct_size = sizeof(current), .action = SC_SOP_STEP_MANUAL};
            }
            if (sc_status_is_ok(status)) {
                sc_str name = trim(sc_str_from_parts(line.ptr + 3, line.len - 3));
                if (str_starts_with(name, "Step:")) {
                    name = trim(sc_str_from_parts(name.ptr + 5, name.len - 5));
                }
                status = copy_string(out->alloc, name, &current.name);
                have_step = sc_status_is_ok(status);
            }
        } else if (have_step && line.len > 0) {
            const char *colon = memchr(line.ptr, ':', line.len);
            if (colon != nullptr) {
                size_t key_len = (size_t)(colon - line.ptr);
                sc_str key = trim(sc_str_from_parts(line.ptr, key_len));
                sc_str value = trim(sc_str_from_parts(colon + 1, line.len - key_len - 1));
                status = sop_parse_key_value(&current, out->alloc, key, value);
            }
        }
        if (offset > markdown.len) {
            break;
        }
    }
    if (sc_status_is_ok(status) && have_step) {
        status = sop_document_push_step(out, &current);
    }
    sc_sop_step_clear(&current);
    if (!sc_status_is_ok(status)) {
        sc_sop_document_clear(out);
    }
    return status;
}

sc_status sc_sop_markdown_load(sc_allocator *alloc, sc_str path, sc_sop_document *out)
{
    FILE *file = nullptr;
    sc_string path_copy = {0};
    sc_string content = {0};
    size_t read_count = 0;
    long size = 0;
    sc_status status;

    if (out == nullptr || path.ptr == nullptr || path.len == 0) {
        return sc_status_invalid_argument("sc.sop.load_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = path_to_cstr(alloc, path, &path_copy);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    file = fopen(path_copy.ptr, "rb");
    if (file == nullptr) {
        status = sc_status_io("sc.sop.open_failed");
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_END) != 0 || (size = ftell(file)) < 0 || fseek(file, 0, SEEK_SET) != 0) {
        status = sc_status_io("sc.sop.size_failed");
        goto cleanup;
    }
    content.ptr = sc_alloc(alloc, (size_t)size + 1, _Alignof(char));
    if (content.ptr == nullptr) {
        status = sc_status_no_memory();
        goto cleanup;
    }
    content.alloc = alloc;
    read_count = fread(content.ptr, 1, (size_t)size, file);
    if (fclose(file) != 0 || read_count != (size_t)size) {
        file = nullptr;
        status = sc_status_io("sc.sop.read_failed");
        goto cleanup;
    }
    file = nullptr;
    content.ptr[read_count] = '\0';
    content.len = read_count;
    status = sc_sop_markdown_parse(alloc, sc_string_as_str(&content), out);

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    sc_string_clear(&content);
    sc_string_clear(&path_copy);
    return status;
}

void sc_sop_step_clear(sc_sop_step *step)
{
    if (step == nullptr) {
        return;
    }
    sc_string_clear(&step->name);
    sc_string_clear(&step->target);
    sc_string_clear(&step->args);
    *step = (sc_sop_step){0};
}

void sc_sop_document_clear(sc_sop_document *document)
{
    if (document == nullptr) {
        return;
    }
    sc_string_clear(&document->title);
    for (size_t i = 0; i < document->steps.len; i += 1) {
        sc_sop_step *step = sc_vec_at(&document->steps, i);
        sc_sop_step_clear(step);
    }
    sc_vec_clear(&document->steps);
    *document = (sc_sop_document){0};
}

void sc_sop_run_start(sc_sop_run_state *run)
{
    if (run == nullptr) {
        return;
    }
    *run = (sc_sop_run_state){.struct_size = sizeof(*run)};
}

const sc_sop_step *sc_sop_run_current_step(const sc_sop_document *document, const sc_sop_run_state *run)
{
    if (document == nullptr || run == nullptr || run->completed || run->current_step >= document->steps.len) {
        return nullptr;
    }
    return sc_vec_at_const(&document->steps, run->current_step);
}

sc_status sc_sop_run_advance(sc_sop_run_state *run, const sc_sop_document *document, sc_audit_chain *audit)
{
    const sc_sop_step *step = sc_sop_run_current_step(document, run);
    if (run == nullptr || document == nullptr) {
        return sc_status_invalid_argument("sc.sop.run_invalid_argument");
    }
    if (step == nullptr) {
        run->completed = true;
        return sc_status_ok();
    }
    if (step->requires_approval && !run->waiting_approval) {
        run->waiting_approval = true;
        if (audit != nullptr) {
            (void)sc_audit_chain_append(audit, sc_str_from_cstr("sop.approval_required"), sc_string_as_str(&step->name));
        }
        return sc_status_cancelled("sc.sop.manual_approval_required");
    }
    if (audit != nullptr) {
        (void)sc_audit_chain_append(audit, sc_str_from_cstr("sop.step_completed"), sc_string_as_str(&step->name));
    }
    run->waiting_approval = false;
    run->current_step += 1;
    run->completed = run->current_step >= document->steps.len;
    return sc_status_ok();
}

sc_status sc_sop_run_approve_manual(sc_sop_run_state *run, const sc_sop_document *document, sc_audit_chain *audit)
{
    const sc_sop_step *step = sc_sop_run_current_step(document, run);
    if (run == nullptr || document == nullptr || step == nullptr || !run->waiting_approval) {
        return sc_status_invalid_argument("sc.sop.approve_invalid_argument");
    }
    if (audit != nullptr) {
        (void)sc_audit_chain_append(audit, sc_str_from_cstr("sop.manual_approved"), sc_string_as_str(&step->name));
    }
    run->waiting_approval = false;
    run->current_step += 1;
    run->completed = run->current_step >= document->steps.len;
    return sc_status_ok();
}

bool sc_sop_condition_evaluate(sc_str expected, sc_str actual)
{
    return expected.len == 0 || sc_str_equal(expected, actual);
}

void sc_routine_clear(sc_routine *routine)
{
    if (routine == nullptr) {
        return;
    }
    sc_string_clear(&routine->id);
    sc_string_clear(&routine->event_name);
    sc_string_clear(&routine->dispatch_target);
    sc_string_clear(&routine->prompt);
    *routine = (sc_routine){0};
}

sc_status sc_routine_init(sc_routine *routine,
                          sc_allocator *alloc,
                          sc_str id,
                          sc_str event_name,
                          sc_str dispatch_target,
                          sc_str prompt)
{
    sc_status status;

    if (routine == nullptr || id.ptr == nullptr || event_name.ptr == nullptr) {
        return sc_status_invalid_argument("sc.routine.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *routine = (sc_routine){.struct_size = sizeof(*routine), .enabled = true};
    status = copy_string(alloc, id, &routine->id);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, event_name, &routine->event_name);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, dispatch_target, &routine->dispatch_target);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, prompt, &routine->prompt);
    }
    if (!sc_status_is_ok(status)) {
        sc_routine_clear(routine);
    }
    return status;
}

bool sc_routine_matches(sc_routine *routine, const sc_routine_event *event)
{
    if (routine == nullptr || event == nullptr || !routine->enabled ||
        !sc_str_equal(sc_string_as_str(&routine->event_name), event->name)) {
        return false;
    }
    if (routine->cooldown_ms > 0 && routine->last_matched_at.unix_ns > 0) {
        int64_t elapsed_ms = (event->occurred_at.unix_ns - routine->last_matched_at.unix_ns) / 1000000;
        if (elapsed_ms < routine->cooldown_ms) {
            return false;
        }
    }
    routine->last_matched_at = event->occurred_at;
    return true;
}

sc_status sc_routine_dispatch(const sc_routine *routine,
                              sc_delivery_target *delivery,
                              const sc_routine_event *event)
{
    sc_delivery_message message = {0};
    if (routine == nullptr || delivery == nullptr || event == nullptr) {
        return sc_status_invalid_argument("sc.routine.dispatch_invalid_argument");
    }
    message = (sc_delivery_message){
        .struct_size = sizeof(message),
        .kind = SC_DELIVERY_SESSION_HISTORY,
        .target = sc_string_as_str(&routine->dispatch_target),
        .content = sc_string_as_str(&routine->prompt),
    };
    return sc_delivery_deliver(delivery, &message);
}

void sc_heartbeat_state_init(sc_heartbeat_state *state, sc_allocator *alloc)
{
    if (state == nullptr) {
        return;
    }
    *state = (sc_heartbeat_state){.struct_size = sizeof(*state), .alloc = alloc == nullptr ? sc_allocator_heap() : alloc, .healthy = true};
}

sc_status sc_heartbeat_tick(sc_heartbeat_state *state, sc_str message, sc_delivery_target *delivery)
{
    sc_status status;
    if (state == nullptr) {
        return sc_status_invalid_argument("sc.heartbeat.invalid_argument");
    }
    status = sc_clock_wall(&state->last_tick_at);
    if (sc_status_is_ok(status)) {
        status = heartbeat_message_replace(state, message);
    }
    if (sc_status_is_ok(status)) {
        state->tick_count += 1;
        state->healthy = true;
        if (delivery != nullptr) {
            sc_delivery_message delivery_message = {
                .struct_size = sizeof(delivery_message),
                .kind = SC_DELIVERY_SESSION_HISTORY,
                .target = sc_str_from_cstr("heartbeat"),
                .content = sc_string_as_str(&state->message),
            };
            status = sc_delivery_deliver(delivery, &delivery_message);
        }
    }
    return status;
}

sc_status sc_heartbeat_state_write(sc_str path, const sc_heartbeat_state *state)
{
    sc_string path_copy = {0};
    FILE *file = nullptr;
    sc_status status;

    if (path.ptr == nullptr || state == nullptr) {
        return sc_status_invalid_argument("sc.heartbeat.write_invalid_argument");
    }
    status = path_to_cstr(sc_allocator_heap(), path, &path_copy);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    file = fopen(path_copy.ptr, "wb");
    if (file == nullptr) {
        status = sc_status_io("sc.heartbeat.open_failed");
        goto cleanup;
    }
    if (fprintf(file,
                "%llu\n%lld\n%d\n%.*s\n",
                (unsigned long long)state->tick_count,
                (long long)state->last_tick_at.unix_ns,
                state->healthy ? 1 : 0,
                (int)state->message.len,
                state->message.ptr == nullptr ? "" : state->message.ptr) < 0 ||
        fclose(file) != 0) {
        file = nullptr;
        status = sc_status_io("sc.heartbeat.write_failed");
        goto cleanup;
    }
    file = nullptr;
    status = sc_status_ok();

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    sc_string_clear(&path_copy);
    return status;
}

sc_status sc_heartbeat_state_read(sc_allocator *alloc, sc_str path, sc_heartbeat_state *out)
{
    sc_string path_copy = {0};
    FILE *file = nullptr;
    unsigned long long tick_count = 0;
    long long last_ns = 0;
    int healthy = 0;
    char message[512] = {0};
    sc_status status;

    if (out == nullptr || path.ptr == nullptr) {
        return sc_status_invalid_argument("sc.heartbeat.read_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = path_to_cstr(alloc, path, &path_copy);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    file = fopen(path_copy.ptr, "rb");
    if (file == nullptr) {
        status = sc_status_io("sc.heartbeat.read_open_failed");
        goto cleanup;
    }
    if (fscanf(file, "%llu\n%lld\n%d\n%511[^\n]", &tick_count, &last_ns, &healthy, message) < 3) {
        status = sc_status_parse("sc.heartbeat.parse_failed");
        goto cleanup;
    }
    (void)fclose(file);
    file = nullptr;
    sc_heartbeat_state_init(out, alloc);
    out->tick_count = (uint64_t)tick_count;
    out->last_tick_at.unix_ns = (int64_t)last_ns;
    out->healthy = healthy != 0;
    status = heartbeat_message_replace(out, sc_str_from_cstr(message));
    if (!sc_status_is_ok(status)) {
        sc_heartbeat_state_clear(out);
    }

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    sc_string_clear(&path_copy);
    return status;
}

void sc_heartbeat_state_clear(sc_heartbeat_state *state)
{
    if (state == nullptr) {
        return;
    }
    sc_string_clear(&state->message);
    *state = (sc_heartbeat_state){0};
}

static bool delivery_vtab_valid(const sc_delivery_vtab *vtab)
{
    return vtab != nullptr &&
           vtab->struct_size >= sizeof(*vtab) &&
           vtab->abi_major == SC_ABI_VERSION_MAJOR &&
           sc_contract_name_is_valid(sc_str_from_cstr(vtab->name)) &&
           vtab->deliver != nullptr &&
           vtab->destroy != nullptr;
}

static sc_str trim(sc_str input)
{
    while (input.len > 0 && isspace((unsigned char)input.ptr[0])) {
        input.ptr += 1;
        input.len -= 1;
    }
    while (input.len > 0 && isspace((unsigned char)input.ptr[input.len - 1])) {
        input.len -= 1;
    }
    return input;
}

static bool str_starts_with(sc_str value, const char *prefix)
{
    sc_str prefix_str = sc_str_from_cstr(prefix);
    return value.len >= prefix_str.len && memcmp(value.ptr, prefix_str.ptr, prefix_str.len) == 0;
}

static sc_status copy_string(sc_allocator *alloc, sc_str value, sc_string *out)
{
    return sc_string_from_str(alloc, value.ptr == nullptr ? sc_str_from_cstr("") : value, out);
}

static sc_status replace_string(sc_string *target, sc_allocator *alloc, sc_str value)
{
    sc_string next = {0};
    sc_status status;
    if (target == nullptr) {
        return sc_status_invalid_argument("sc.string.replace_invalid_argument");
    }
    status = copy_string(alloc, value, &next);
    if (sc_status_is_ok(status)) {
        sc_string_clear(target);
        *target = next;
    }
    return status;
}

static sc_status parse_field(sc_str field, int min, int max, int *out)
{
    long value = 0;
    char *end = nullptr;
    sc_string copy = {0};
    sc_status status;
    if (out == nullptr || field.ptr == nullptr || field.len == 0) {
        return sc_status_parse("sc.cron_schedule.invalid_field");
    }
    if (field.len == 1 && field.ptr[0] == '*') {
        *out = -1;
        return sc_status_ok();
    }
    status = sc_string_from_str(sc_allocator_heap(), field, &copy);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    value = strtol(copy.ptr, &end, 10);
    if (end == copy.ptr || *end != '\0' || value < min || value > max) {
        sc_string_clear(&copy);
        return sc_status_parse("sc.cron_schedule.field_out_of_range");
    }
    *out = (int)value;
    sc_string_clear(&copy);
    return sc_status_ok();
}

static sc_status parse_minute_field(sc_str field, int *minute, int *step)
{
    if (field.len >= 3 && field.ptr[0] == '*' && field.ptr[1] == '/') {
        sc_status status = parse_field(sc_str_from_parts(field.ptr + 2, field.len - 2), 1, 59, step);
        if (sc_status_is_ok(status)) {
            *minute = -1;
        }
        return status;
    }
    *step = 0;
    return parse_field(field, 0, 59, minute);
}

static bool field_matches(int expected, int actual)
{
    return expected < 0 || expected == actual;
}

static bool minute_matches(const sc_cron_schedule *schedule, int minute)
{
    if (schedule->minute_step > 0) {
        return minute % schedule->minute_step == 0;
    }
    return field_matches(schedule->minute, minute);
}

#ifdef SC_HAVE_NANOCRON
static sc_status cron_schedule_normalize(sc_str expression, char out[sizeof(((sc_cron_schedule *)0)->nanocron_expression)])
{
    sc_allocator *alloc = sc_allocator_heap();
    sc_string copy = {0};
    char *save = nullptr;
    char *fields[5] = {0};
    sc_status status;
    size_t field_count = 0;
    int written = 0;

    if (out == nullptr || expression.ptr == nullptr || expression.len == 0 ||
        expression.len >= sizeof(((sc_cron_schedule *)0)->nanocron_expression)) {
        return sc_status_parse("sc.cron_schedule.invalid_expression");
    }

    field_count = cron_schedule_count_fields(expression);
    if (field_count == 7) {
        (void)memcpy(out, expression.ptr, expression.len);
        out[expression.len] = '\0';
        return sc_status_ok();
    }
    if (field_count != 5) {
        return sc_status_parse("sc.cron_schedule.field_count");
    }

    status = sc_string_from_str(alloc, expression, &copy);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    fields[0] = strtok_r(copy.ptr, " \t", &save);
    for (size_t i = 1; i < 5 && fields[i - 1] != nullptr; i += 1) {
        fields[i] = strtok_r(nullptr, " \t", &save);
    }
    if (fields[0] == nullptr || fields[1] == nullptr || fields[2] == nullptr || fields[3] == nullptr || fields[4] == nullptr) {
        sc_string_clear(&copy);
        return sc_status_parse("sc.cron_schedule.field_count");
    }

    written = snprintf(out,
                       sizeof(((sc_cron_schedule *)0)->nanocron_expression),
                       "0 0 %s %s %s %s %s",
                       fields[0],
                       fields[1],
                       fields[2],
                       fields[3],
                       fields[4]);
    sc_string_clear(&copy);
    if (written < 0 || (size_t)written >= sizeof(((sc_cron_schedule *)0)->nanocron_expression)) {
        out[0] = '\0';
        return sc_status_parse("sc.cron_schedule.expression_too_long");
    }
    return sc_status_ok();
}

static size_t cron_schedule_count_fields(sc_str expression)
{
    size_t count = 0;
    bool in_field = false;

    if (expression.ptr == nullptr) {
        return 0;
    }
    for (size_t i = 0; i < expression.len; i += 1) {
        if (isspace((unsigned char)expression.ptr[i])) {
            in_field = false;
        } else if (!in_field) {
            count += 1;
            in_field = true;
        }
    }
    return count;
}

static bool cron_schedule_to_timespec(sc_wall_time time, struct timespec *out)
{
    int64_t seconds = 0;
    int64_t nsec = 0;
    time_t cast_seconds = 0;

    if (out == nullptr) {
        return false;
    }

    seconds = time.unix_ns / 1000000000LL;
    nsec = time.unix_ns % 1000000000LL;
    if (nsec < 0) {
        nsec += 1000000000LL;
        seconds -= 1;
    }

    cast_seconds = (time_t)seconds;
    if ((int64_t)cast_seconds != seconds) {
        return false;
    }

    out->tv_sec = cast_seconds;
    out->tv_nsec = (long)nsec;
    return true;
}

static sc_wall_time cron_schedule_from_timespec(const struct timespec *time)
{
    return (sc_wall_time){
        .unix_ns = ((int64_t)time->tv_sec * 1000000000LL) + (int64_t)time->tv_nsec,
    };
}

static void cron_noop_callback(void *user_data, const struct timespec *trigger_time)
{
    (void)user_data;
    (void)trigger_time;
}

static void cron_match_callback(void *user_data, const struct timespec *trigger_time)
{
    bool *matched = user_data;

    (void)trigger_time;
    if (matched != nullptr) {
        *matched = true;
    }
}

static bool cron_schedule_nanocron_matches(const sc_cron_schedule *schedule, sc_wall_time when)
{
    cron_ctx_t *ctx = nullptr;
    const cron_job_t *job = nullptr;
    struct timespec now = {0};
    bool matched = false;

    if (schedule == nullptr || schedule->nanocron_expression[0] == '\0' || !cron_schedule_to_timespec(when, &now)) {
        return false;
    }

    ctx = cron_create();
    if (ctx == nullptr) {
        return false;
    }
    job = cron_add(ctx, schedule->nanocron_expression, cron_match_callback, &matched);
    if (job != nullptr) {
        cron_execute_due(ctx, &now);
    }
    cron_destroy(ctx);
    return matched;
}

static sc_status cron_schedule_nanocron_next_after(const sc_cron_schedule *schedule, sc_wall_time after, sc_wall_time *out)
{
    cron_ctx_t *ctx = nullptr;
    const cron_job_t *job = nullptr;
    struct timespec after_ts = {0};
    struct timespec next_ts = {0};
    bool found = false;

    if (schedule == nullptr || out == nullptr || schedule->nanocron_expression[0] == '\0') {
        return sc_status_invalid_argument("sc.cron_schedule.invalid_argument");
    }
    if (!cron_schedule_to_timespec(after, &after_ts)) {
        return sc_status_invalid_argument("sc.cron_schedule.invalid_time");
    }

    ctx = cron_create();
    if (ctx == nullptr) {
        return sc_status_no_memory();
    }
    job = cron_add(ctx, schedule->nanocron_expression, cron_noop_callback, nullptr);
    if (job != nullptr) {
        found = cron_get_next_trigger(ctx, &after_ts, &next_ts);
    }
    cron_destroy(ctx);
    if (!found) {
        return sc_status_timeout("sc.cron_schedule.next_not_found");
    }

    *out = cron_schedule_from_timespec(&next_ts);
    return sc_status_ok();
}
#endif

static void cron_job_move_assign(sc_cron_job *dst, sc_cron_job *src)
{
    *dst = *src;
    *src = (sc_cron_job){0};
}

static sc_status cron_job_copy(sc_allocator *alloc, const sc_cron_job *job, sc_cron_job *out)
{
    sc_status status;
    if (out == nullptr || job == nullptr) {
        return sc_status_invalid_argument("sc.cron_job.copy_invalid_argument");
    }
    *out = (sc_cron_job){
        .struct_size = sizeof(*out),
        .schedule = job->schedule,
        .kind = job->kind,
        .enabled = job->enabled,
        .once = job->once,
        .cancel_requested = job->cancel_requested,
        .last_run_at = job->last_run_at,
    };
    status = copy_string(alloc, sc_string_as_str(&job->id), &out->id);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&job->command), &out->command);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&job->delivery_target), &out->delivery_target);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&job->schedule_text), &out->schedule_text);
    }
    if (!sc_status_is_ok(status)) {
        sc_cron_job_clear(out);
    }
    return status;
}

static sc_status cron_run_record_copy(sc_allocator *alloc, const sc_cron_run_record *record, sc_cron_run_record *out)
{
    sc_status status;
    if (out == nullptr || record == nullptr) {
        return sc_status_invalid_argument("sc.cron_run.copy_invalid_argument");
    }
    *out = (sc_cron_run_record){
        .struct_size = sizeof(*out),
        .status_code = record->status_code,
        .started_at = record->started_at,
        .ended_at = record->ended_at,
    };
    status = copy_string(alloc, sc_string_as_str(&record->job_id), &out->job_id);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&record->output), &out->output);
    }
    if (!sc_status_is_ok(status)) {
        sc_cron_run_record_clear(out);
    }
    return status;
}

static sc_status cron_store_read_all(sc_allocator *alloc, FILE *file, sc_string *out)
{
    long file_size_long = 0;
    size_t file_size = 0;
    size_t alloc_size = 0;
    size_t read_len = 0;
    char *buffer = nullptr;
    sc_status status = sc_status_ok();

    if (alloc == nullptr || file == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.cron_job_store.read_invalid_argument");
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        return sc_status_io("sc.cron_job_store.seek_failed");
    }
    file_size_long = ftell(file);
    if (file_size_long < 0) {
        return sc_status_io("sc.cron_job_store.tell_failed");
    }
    if (file_size_long > SC_CRON_STATE_MAX_BYTES) {
        return sc_status_parse("sc.cron_job_store.file_too_large");
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        return sc_status_io("sc.cron_job_store.seek_failed");
    }

    file_size = (size_t)file_size_long;
    if (sc_size_add_overflow(file_size, 1U, &alloc_size)) {
        return sc_status_no_memory();
    }
    buffer = sc_alloc(alloc, alloc_size, _Alignof(char));
    if (buffer == nullptr) {
        return sc_status_no_memory();
    }
    read_len = fread(buffer, 1, file_size, file);
    if (read_len != file_size) {
        status = sc_status_io("sc.cron_job_store.read_failed");
        goto cleanup;
    }
    buffer[read_len] = '\0';
    *out = (sc_string){.ptr = buffer, .len = read_len, .alloc = alloc};
    buffer = nullptr;

cleanup:
    if (buffer != nullptr) {
        sc_free(alloc, buffer, alloc_size, _Alignof(char));
    }
    return status;
}

static sc_status cron_store_parse_body(sc_cron_job_store *store, sc_str body)
{
    size_t offset = 0;
    size_t line_index = 0;
    sc_status status = sc_status_ok();

    if (store == nullptr || (body.len > 0 && body.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.cron_job_store.parse_invalid_argument");
    }
    if (body.len == 0) {
        return sc_status_ok();
    }
    while (sc_status_is_ok(status) && offset < body.len) {
        size_t start = offset;
        sc_str line = {0};

        while (offset < body.len && body.ptr[offset] != '\n') {
            offset += 1;
        }
        line = sc_str_from_parts(&body.ptr[start], offset - start);
        if (offset < body.len && body.ptr[offset] == '\n') {
            offset += 1;
        }
        if (line.len > 0 && line.ptr[line.len - 1U] == '\r') {
            line.len -= 1U;
        }

        if (line_index == 0) {
            if (!sc_str_equal(line, sc_str_from_cstr("sc-cron-jobs-v1"))) {
                status = sc_status_parse("sc.cron_job_store.header_invalid");
            }
        } else if (line.len > 0) {
            status = cron_store_parse_job_line(store, line);
        }
        line_index += 1U;
    }
    return status;
}

static sc_status cron_store_parse_job_line(sc_cron_job_store *store, sc_str line)
{
    enum {
        CRON_STATE_FIELD_COUNT = 9
    };
    sc_str fields[CRON_STATE_FIELD_COUNT] = {0};
    size_t field_count = 0;
    size_t start = 0;
    bool enabled = false;
    bool once = false;
    int64_t last_run = 0;
    sc_string id = {0};
    sc_string schedule_text = {0};
    sc_string target = {0};
    sc_string prompt = {0};
    sc_cron_job job = {.struct_size = sizeof(job), .enabled = true};
    sc_allocator *alloc = store == nullptr || store->alloc == nullptr ? sc_allocator_heap() : store->alloc;
    sc_status status;

    if (store == nullptr || line.ptr == nullptr || line.len == 0) {
        return sc_status_parse("sc.cron_job_store.line_invalid");
    }
    for (size_t i = 0; i <= line.len; i += 1) {
        if (i == line.len || line.ptr[i] == '|') {
            if (field_count >= CRON_STATE_FIELD_COUNT) {
                return sc_status_parse("sc.cron_job_store.field_count");
            }
            fields[field_count] = sc_str_from_parts(&line.ptr[start], i - start);
            field_count += 1U;
            start = i + 1U;
        }
    }
    if (field_count != CRON_STATE_FIELD_COUNT || !sc_str_equal(fields[0], sc_str_from_cstr("job"))) {
        return sc_status_parse("sc.cron_job_store.field_count");
    }
    if (sc_str_equal(fields[1], sc_str_from_cstr("agent"))) {
        job.kind = SC_CRON_JOB_AGENT;
    } else if (sc_str_equal(fields[1], sc_str_from_cstr("shell"))) {
        job.kind = SC_CRON_JOB_SHELL;
    } else {
        return sc_status_parse("sc.cron_job_store.kind_invalid");
    }
    if (!cron_store_parse_bool(fields[2], &enabled) || !cron_store_parse_bool(fields[3], &once) ||
        !cron_store_parse_i64(alloc, fields[4], &last_run)) {
        return sc_status_parse("sc.cron_job_store.scalar_invalid");
    }
    status = cron_store_decode_escaped(alloc, fields[5], &id);
    if (sc_status_is_ok(status)) {
        status = cron_store_decode_escaped(alloc, fields[6], &schedule_text);
    }
    if (sc_status_is_ok(status)) {
        status = cron_store_decode_escaped(alloc, fields[7], &target);
    }
    if (sc_status_is_ok(status)) {
        status = cron_store_decode_escaped(alloc, fields[8], &prompt);
    }
    if (sc_status_is_ok(status)) {
        status = sc_cron_schedule_parse(sc_string_as_str(&schedule_text), &job.schedule);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&id), &job.id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&target), &job.delivery_target);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&prompt), &job.command);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&schedule_text), &job.schedule_text);
    }
    if (sc_status_is_ok(status)) {
        job.enabled = enabled;
        job.once = once;
        job.last_run_at = (sc_wall_time){.unix_ns = last_run};
        status = sc_cron_job_store_put(store, &job);
    }

    sc_cron_job_clear(&job);
    sc_string_clear(&prompt);
    sc_string_clear(&target);
    sc_string_clear(&schedule_text);
    sc_string_clear(&id);
    return status;
}

static sc_status cron_store_write_escaped(FILE *file, sc_str value)
{
    static const char hex[] = "0123456789ABCDEF";

    if (file == nullptr || (value.len > 0 && value.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.cron_job_store.escape_invalid_argument");
    }
    for (size_t i = 0; i < value.len; i += 1) {
        unsigned char ch = (unsigned char)value.ptr[i];
        if (ch == '%' || ch == '|' || ch < 0x20U || ch == 0x7FU) {
            if (fputc('%', file) == EOF || fputc(hex[ch >> 4U], file) == EOF ||
                fputc(hex[ch & 0x0FU], file) == EOF) {
                return sc_status_io("sc.cron_job_store.write_failed");
            }
        } else if (fputc((int)ch, file) == EOF) {
            return sc_status_io("sc.cron_job_store.write_failed");
        }
    }
    return sc_status_ok();
}

static sc_status cron_store_decode_escaped(sc_allocator *alloc, sc_str value, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    if (alloc == nullptr || out == nullptr || (value.len > 0 && value.ptr == nullptr)) {
        return sc_status_invalid_argument("sc.cron_job_store.decode_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    for (size_t i = 0; sc_status_is_ok(status) && i < value.len; i += 1) {
        char ch = value.ptr[i];
        if (ch == '%') {
            int hi = i + 2U < value.len ? cron_store_hex_value(value.ptr[i + 1U]) : -1;
            int lo = i + 2U < value.len ? cron_store_hex_value(value.ptr[i + 2U]) : -1;
            char decoded = 0;

            if (hi < 0 || lo < 0) {
                status = sc_status_parse("sc.cron_job_store.escape_invalid");
                break;
            }
            decoded = (char)((hi << 4) | lo);
            status = sc_string_builder_append(&builder, sc_str_from_parts(&decoded, 1));
            i += 2U;
        } else {
            status = sc_string_builder_append(&builder, sc_str_from_parts(&value.ptr[i], 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    } else {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static bool cron_store_parse_bool(sc_str value, bool *out)
{
    if (out == nullptr) {
        return false;
    }
    if (sc_str_equal(value, sc_str_from_cstr("true"))) {
        *out = true;
        return true;
    }
    if (sc_str_equal(value, sc_str_from_cstr("false"))) {
        *out = false;
        return true;
    }
    return false;
}

static bool cron_store_parse_i64(sc_allocator *alloc, sc_str value, int64_t *out)
{
    sc_string copy = {0};
    char *end = nullptr;
    long long parsed = 0;
    bool ok = false;

    if (out == nullptr || value.ptr == nullptr || value.len == 0) {
        return false;
    }
    if (!sc_status_is_ok(sc_string_from_str(alloc, value, &copy))) {
        return false;
    }
    errno = 0;
    parsed = strtoll(copy.ptr, &end, 10);
    if (errno == 0 && end != copy.ptr && end != nullptr && *end == '\0') {
        *out = (int64_t)parsed;
        ok = true;
    }
    sc_string_clear(&copy);
    return ok;
}

static int cron_store_hex_value(char ch)
{
    if (ch >= '0' && ch <= '9') {
        return ch - '0';
    }
    if (ch >= 'a' && ch <= 'f') {
        return 10 + ch - 'a';
    }
    if (ch >= 'A' && ch <= 'F') {
        return 10 + ch - 'A';
    }
    return -1;
}

static sc_status cron_run_store_append_status(sc_cron_run_store *store,
                                              sc_str job_id,
                                              sc_status_code code,
                                              sc_str output,
                                              sc_wall_time started,
                                              sc_wall_time ended)
{
    sc_cron_run_record record = {.struct_size = sizeof(record), .status_code = code, .started_at = started, .ended_at = ended};
    sc_status status = copy_string(store == nullptr ? sc_allocator_heap() : store->alloc, job_id, &record.job_id);
    if (sc_status_is_ok(status)) {
        status = copy_string(store == nullptr ? sc_allocator_heap() : store->alloc, output, &record.output);
    }
    if (sc_status_is_ok(status)) {
        status = sc_cron_run_store_append(store, &record);
    }
    sc_cron_run_record_clear(&record);
    return status;
}

static sc_status stdout_deliver(void *impl, const sc_delivery_message *message)
{
    (void)impl;
    if (message == nullptr || message->content.ptr == nullptr) {
        return sc_status_invalid_argument("sc.delivery.stdout_invalid_argument");
    }
    if (fwrite(message->content.ptr, 1, message->content.len, stdout) != message->content.len || fputc('\n', stdout) == EOF) {
        return sc_status_io("sc.delivery.stdout_write_failed");
    }
    return sc_status_ok();
}

static void stdout_destroy(void *impl)
{
    (void)impl;
}

static sc_status sop_step_copy(sc_allocator *alloc, const sc_sop_step *step, sc_sop_step *out)
{
    sc_status status;
    *out = (sc_sop_step){.struct_size = sizeof(*out), .action = step->action, .requires_approval = step->requires_approval};
    status = copy_string(alloc, sc_string_as_str(&step->name), &out->name);
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&step->target), &out->target);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, sc_string_as_str(&step->args), &out->args);
    }
    if (!sc_status_is_ok(status)) {
        sc_sop_step_clear(out);
    }
    return status;
}

static sc_status sop_document_push_step(sc_sop_document *document, const sc_sop_step *step)
{
    sc_sop_step copy = {0};
    sc_status status;
    if (document == nullptr || step == nullptr || step->name.len == 0) {
        return sc_status_invalid_argument("sc.sop.step_invalid_argument");
    }
    status = sop_step_copy(document->alloc, step, &copy);
    if (sc_status_is_ok(status)) {
        status = sc_vec_push(&document->steps, &copy);
    }
    if (!sc_status_is_ok(status)) {
        sc_sop_step_clear(&copy);
    }
    return status;
}

static sc_status sop_parse_key_value(sc_sop_step *step, sc_allocator *alloc, sc_str key, sc_str value)
{
    if (sc_str_equal(key, sc_str_from_cstr("action"))) {
        step->action = sop_action_from_str(value);
        return sc_status_ok();
    }
    if (sc_str_equal(key, sc_str_from_cstr("target"))) {
        return replace_string(&step->target, alloc, value);
    }
    if (sc_str_equal(key, sc_str_from_cstr("args")) || sc_str_equal(key, sc_str_from_cstr("prompt"))) {
        return replace_string(&step->args, alloc, value);
    }
    if (sc_str_equal(key, sc_str_from_cstr("requires_approval"))) {
        step->requires_approval = sc_str_equal(value, sc_str_from_cstr("true")) || sc_str_equal(value, sc_str_from_cstr("yes"));
    }
    return sc_status_ok();
}

static sc_sop_step_action sop_action_from_str(sc_str value)
{
    if (sc_str_equal(value, sc_str_from_cstr("tool"))) {
        return SC_SOP_STEP_TOOL;
    }
    if (sc_str_equal(value, sc_str_from_cstr("agent"))) {
        return SC_SOP_STEP_AGENT;
    }
    return SC_SOP_STEP_MANUAL;
}

static sc_status heartbeat_message_replace(sc_heartbeat_state *state, sc_str message)
{
    if (state == nullptr) {
        return sc_status_invalid_argument("sc.heartbeat.message_invalid_argument");
    }
    return replace_string(&state->message, state->alloc == nullptr ? sc_allocator_heap() : state->alloc, message);
}

static sc_status path_to_cstr(sc_allocator *alloc, sc_str path, sc_string *out)
{
    return copy_string(alloc, path, out);
}
