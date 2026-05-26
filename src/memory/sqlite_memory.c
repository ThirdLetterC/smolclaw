// cppcheck-suppress-file redundantInitialization
#include "memory/memory_internal.h"

#include "sc/contract.h"

#include <dlfcn.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>

#ifdef SC_HAVE_SQLITE
#include <sqlite3.h>
#endif

#define SC_SQLITE_OK 0
#define SC_SQLITE_ROW 100
#define SC_SQLITE_DONE 101
#define SC_SQLITE_STATIC ((void (*)(void *))0)

#ifndef SC_HAVE_SQLITE
typedef struct sqlite3 sqlite3;
typedef struct sqlite3_stmt sqlite3_stmt;
#endif

typedef struct sqlite_api {
    void *library;
    int (*open)(const char *filename, sqlite3 **db);
    int (*close)(sqlite3 *db);
    int (*busy_timeout)(sqlite3 *db, int ms);
    int (*exec)(sqlite3 *db, const char *sql, int (*callback)(void *, int, char **, char **), void *arg, char **errmsg);
    int (*prepare_v2)(sqlite3 *db, const char *sql, int nbyte, sqlite3_stmt **stmt, const char **tail);
    int (*bind_text)(sqlite3_stmt *stmt, int index, const char *value, int bytes, void (*destroy)(void *));
    int (*bind_int64)(sqlite3_stmt *stmt, int index, long long value);
    int (*bind_double)(sqlite3_stmt *stmt, int index, double value);
    int (*step)(sqlite3_stmt *stmt);
    int (*finalize)(sqlite3_stmt *stmt);
    const unsigned char *(*column_text)(sqlite3_stmt *stmt, int column);
    int (*column_bytes)(sqlite3_stmt *stmt, int column);
    long long (*column_int64)(sqlite3_stmt *stmt, int column);
    double (*column_double)(sqlite3_stmt *stmt, int column);
    const char *(*errmsg)(sqlite3 *db);
} sqlite_api;

typedef struct sqlite_memory {
    sc_allocator *alloc;
    sqlite_api api;
    sqlite3 *db;
} sqlite_memory;

static sc_status sqlite_put(void *impl, const sc_memory_record *record);
static sc_status sqlite_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out);
static sc_status sqlite_search(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_memory_result *out);
static sc_status sqlite_forget(void *impl, sc_str namespace_name, sc_str key);
static sc_status sqlite_redact(void *impl, sc_str namespace_name, sc_str key);
static sc_status sqlite_purge_namespace(void *impl, sc_str namespace_name);
static sc_status sqlite_purge_session(void *impl, sc_str namespace_name, sc_str session_id);
static sc_status sqlite_export_snapshot(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_string *out);
static sc_status sqlite_export_snapshot_ex(void *impl,
                                           const sc_memory_query *query,
                                           const sc_memory_export_options *options,
                                           sc_allocator *alloc,
                                           sc_string *out);
static void sqlite_destroy(void *impl);
static sc_status load_sqlite(sqlite_api *api);
static sc_status apply_migrations(sqlite_memory *memory);
static sc_status exec_sql(sqlite_memory *memory, const char *sql);
static sc_status begin_transaction(sqlite_memory *memory);
static sc_status commit_transaction(sqlite_memory *memory);
static void rollback_transaction(sqlite_memory *memory);
static sc_status bind_str(sqlite_memory *memory, sqlite3_stmt *stmt, int index, sc_str value);
static sc_status bind_i64(sqlite_memory *memory, sqlite3_stmt *stmt, int index, long long value);
static sc_status bind_search_filters(sqlite_memory *memory, sqlite3_stmt *stmt, const sc_memory_query *query, int first_index);
static bool sqlite_query_has_match_text(const sc_memory_query *query);
static long long sqlite_search_limit(const sc_memory_query *query);
static sc_status sqlite_make_fts_phrase(sc_allocator *alloc, sc_str query, sc_string *out);
static sc_status make_entry_from_stmt(sqlite_memory *memory, sqlite3_stmt *stmt, sc_allocator *alloc, sc_memory_entry *out);
static void sqlite_apply_total_result_byte_limit(const sc_memory_query *query, sc_memory_result *out);
static sc_status sqlite_result_to_snapshot(sc_memory_result *result,
                                           const sc_memory_export_options *options,
                                           sc_allocator *alloc,
                                           sc_string *out);

static const sc_memory_vtab sqlite_vtab = {
    .struct_size = sizeof(sc_memory_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "sqlite",
    .display_name = "SQLite memory",
    .feature_flag = "SC_MEMORY_SQLITE",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .put = sqlite_put,
    .get = sqlite_get,
    .search = sqlite_search,
    .forget = sqlite_forget,
    .purge_namespace = sqlite_purge_namespace,
    .purge_session = sqlite_purge_session,
    .export_snapshot = sqlite_export_snapshot,
    .destroy = sqlite_destroy,
    .config_schema_ref = "sc.schema.memory.sqlite.v3",
    .memory_capabilities = SC_MEMORY_BACKEND_CAP_STORE |
                           SC_MEMORY_BACKEND_CAP_RETRIEVE |
                           SC_MEMORY_BACKEND_CAP_DELETE |
                           SC_MEMORY_BACKEND_CAP_REDACT |
                           SC_MEMORY_BACKEND_CAP_SNAPSHOTS |
                           SC_MEMORY_BACKEND_CAP_IMPORT_EXPORT |
                           SC_MEMORY_BACKEND_CAP_MIGRATIONS,
    .persistence_type = SC_MEMORY_PERSISTENCE_SQLITE,
    .migration_version = 3,
    .supports_migrations = true,
    .redact = sqlite_redact,
    .export_snapshot_ex = sqlite_export_snapshot_ex,
};

sc_status sc_memory_sqlite_open(sc_allocator *alloc, sc_str path, sc_memory **out)
{
    sqlite_memory *impl = nullptr;
    sc_status status = sc_status_ok();
    char *path_cstr = nullptr;

    if (out == nullptr || path.ptr == nullptr || path.len == 0) {
        return sc_status_invalid_argument("sc.memory_sqlite.invalid_argument");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(sqlite_memory));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (sqlite_memory){.alloc = alloc};

    path_cstr = sc_alloc(alloc, path.len + 1, _Alignof(char));
    if (path_cstr == nullptr) {
        sqlite_destroy(impl);
        return sc_status_no_memory();
    }
    (void)memcpy(path_cstr, path.ptr, path.len);
    path_cstr[path.len] = '\0';

    status = load_sqlite(&impl->api);
    if (sc_status_is_ok(status) && impl->api.open(path_cstr, &impl->db) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.open_failed");
    }
    if (sc_status_is_ok(status) && impl->api.busy_timeout(impl->db, 250) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.busy_timeout_failed");
    }
    if (sc_status_is_ok(status)) {
        status = apply_migrations(impl);
    }
    if (sc_status_is_ok(status)) {
        status = sc_memory_new(alloc, &sqlite_vtab, impl, out);
    }

    sc_free(alloc, path_cstr, path.len + 1, _Alignof(char));
    if (!sc_status_is_ok(status)) {
        sqlite_destroy(impl);
    }
    return status;
}

static sc_status sqlite_put(void *impl, const sc_memory_record *record)
{
    sqlite_memory *memory = impl;
    sqlite3_stmt *stmt = nullptr;
    sc_string id = {0};
    sc_wall_time now = {0};
    sc_status status = sc_status_ok();
    const char *sql =
        "INSERT INTO memory_entries "
        "(id, namespace, session_id, category, key, content, timestamp_ns, score, importance, superseded_by, "
        "source, metadata_json, redaction_state, content_ref) "
        "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?) "
        "ON CONFLICT(namespace, key) DO UPDATE SET "
        "id=excluded.id, session_id=excluded.session_id, category=excluded.category, content=excluded.content, "
        "timestamp_ns=excluded.timestamp_ns, score=excluded.score, importance=excluded.importance, "
        "superseded_by=excluded.superseded_by, source=excluded.source, metadata_json=excluded.metadata_json, "
        "redaction_state=excluded.redaction_state, content_ref=excluded.content_ref";

    if (memory == nullptr || record == nullptr || record->namespace_name.ptr == nullptr || record->key.ptr == nullptr) {
        return sc_status_invalid_argument("sc.memory_sqlite.invalid_argument");
    }
    status = sc_memory_validate_record(record);
    if (sc_status_is_ok(status)) {
        status = record->id.ptr == nullptr || record->id.len == 0 ? sc_uuid_v4(memory->alloc, &id)
                                                           : sc_string_from_str(memory->alloc, record->id, &id);
    }
    if (sc_status_is_ok(status)) {
        status = sc_clock_wall(&now);
    }
    if (sc_status_is_ok(status)) {
        status = begin_transaction(memory);
    }
    if (sc_status_is_ok(status) &&
        memory->api.prepare_v2(memory->db, sql, -1, &stmt, nullptr) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.prepare_failed");
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 1, sc_string_as_str(&id));
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 2, record->namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 3, record->session_id);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 4, record->category);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 5, record->key);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 6, record->value);
    }
    if (sc_status_is_ok(status) && memory->api.bind_int64(stmt, 7, (long long)now.unix_ns) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.bind_failed");
    }
    if (sc_status_is_ok(status) && memory->api.bind_double(stmt, 8, record->score) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.bind_failed");
    }
    if (sc_status_is_ok(status) && memory->api.bind_int64(stmt, 9, (long long)record->importance) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.bind_failed");
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 10, record->superseded_by_id);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 11, record->source);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 12, record->metadata_json);
    }
    if (sc_status_is_ok(status) && memory->api.bind_int64(stmt, 13, (long long)record->redaction_state) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.bind_failed");
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 14, record->content_ref);
    }
    if (sc_status_is_ok(status) && memory->api.step(stmt) != SC_SQLITE_DONE) {
        status = sc_status_io("sc.memory_sqlite.step_failed");
    }
    if (stmt != nullptr && memory->api.finalize(stmt) != SC_SQLITE_OK && sc_status_is_ok(status)) {
        status = sc_status_io("sc.memory_sqlite.finalize_failed");
    }
    if (sc_status_is_ok(status)) {
        status = commit_transaction(memory);
    } else {
        rollback_transaction(memory);
    }

    sc_string_clear(&id);
    return status;
}

static sc_status sqlite_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out)
{
    sqlite_memory *memory = impl;
    sqlite3_stmt *stmt = nullptr;
    sc_status status = sc_status_ok();
    const char *sql = "SELECT content FROM memory_entries WHERE namespace=? AND key=? AND superseded_by='' AND redaction_state=0 LIMIT 1";

    if (memory == nullptr || out == nullptr || namespace_name.ptr == nullptr || key.ptr == nullptr) {
        return sc_status_invalid_argument("sc.memory_sqlite.invalid_argument");
    }
    if (memory->api.prepare_v2(memory->db, sql, -1, &stmt, nullptr) != SC_SQLITE_OK) {
        return sc_status_io("sc.memory_sqlite.prepare_failed");
    }
    status = bind_str(memory, stmt, 1, namespace_name);
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 2, key);
    }
    if (sc_status_is_ok(status)) {
        int rc = memory->api.step(stmt);
        if (rc == SC_SQLITE_ROW) {
            const unsigned char *text = memory->api.column_text(stmt, 0);
            int bytes = memory->api.column_bytes(stmt, 0);
            status = sc_string_from_str(alloc, sc_str_from_parts((const char *)text, (size_t)bytes), out);
        } else if (rc == SC_SQLITE_DONE) {
            status = sc_status_invalid_argument("sc.memory_sqlite.not_found");
        } else {
            status = sc_status_io("sc.memory_sqlite.step_failed");
        }
    }
    if (memory->api.finalize(stmt) != SC_SQLITE_OK && sc_status_is_ok(status)) {
        status = sc_status_io("sc.memory_sqlite.finalize_failed");
    }
    return status;
}

static sc_status sqlite_search(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_memory_result *out)
{
    sqlite_memory *memory = impl;
    sqlite3_stmt *stmt = nullptr;
    sc_string fts_query = {0};
    sc_status status = sc_status_ok();
    const char *sql_filter =
        "SELECT e.id, e.namespace, e.session_id, e.category, e.key, e.content, e.timestamp_ns, e.score, e.importance, "
        "e.superseded_by, e.source, e.metadata_json, e.redaction_state, e.content_ref "
        "FROM memory_entries AS e "
        "WHERE (?='' OR e.namespace=?) "
        "AND (?='' OR e.session_id=?) "
        "AND (?='' OR e.category=?) "
        "AND (?!=0 OR e.superseded_by='') "
        "AND (?!=0 OR e.redaction_state!=1) "
        "AND (?!=0 OR e.redaction_state!=2) "
        "ORDER BY e.score DESC, e.importance DESC, e.timestamp_ns DESC, e.key ASC, e.id ASC "
        "LIMIT ?";
    const char *sql_fts =
        "SELECT e.id, e.namespace, e.session_id, e.category, e.key, e.content, e.timestamp_ns, e.score, e.importance, "
        "e.superseded_by, e.source, e.metadata_json, e.redaction_state, e.content_ref "
        "FROM memory_entries AS e "
        "JOIN memory_entries_fts ON memory_entries_fts.rowid=e.rowid "
        "WHERE memory_entries_fts MATCH ? "
        "AND (?='' OR e.namespace=?) "
        "AND (?='' OR e.session_id=?) "
        "AND (?='' OR e.category=?) "
        "AND (?!=0 OR e.superseded_by='') "
        "AND (?!=0 OR e.redaction_state!=1) "
        "AND (?!=0 OR e.redaction_state!=2) "
        "ORDER BY bm25(memory_entries_fts, 10.0, 1.0), e.score DESC, e.importance DESC, e.timestamp_ns DESC, e.key ASC, e.id ASC "
        "LIMIT ?";
    bool use_fts = false;

    if (memory == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.memory_sqlite.invalid_argument");
    }
    status = sc_memory_validate_query(query);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    sc_memory_result_init(out, alloc);
    use_fts = sqlite_query_has_match_text(query);
    if (use_fts) {
        status = sqlite_make_fts_phrase(alloc, query->query, &fts_query);
    }
    if (sc_status_is_ok(status) &&
        memory->api.prepare_v2(memory->db, use_fts ? sql_fts : sql_filter, -1, &stmt, nullptr) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.prepare_failed");
    }
    if (sc_status_is_ok(status) && use_fts) {
        status = bind_str(memory, stmt, 1, sc_string_as_str(&fts_query));
    }
    if (sc_status_is_ok(status)) {
        status = bind_search_filters(memory, stmt, query, use_fts ? 2 : 1);
    }
    if (!sc_status_is_ok(status)) {
        if (stmt != nullptr) {
            (void)memory->api.finalize(stmt);
        }
        sc_string_clear(&fts_query);
        sc_memory_result_clear(out);
        return status;
    }

    for (;;) {
        int rc = memory->api.step(stmt);
        sc_memory_entry entry = {0};
        if (rc == SC_SQLITE_DONE) {
            break;
        }
        if (rc != SC_SQLITE_ROW) {
            status = sc_status_io("sc.memory_sqlite.step_failed");
            break;
        }
        status = make_entry_from_stmt(memory, stmt, alloc, &entry);
        if (!sc_status_is_ok(status)) {
            sc_memory_entry_clear(&entry);
            break;
        }
        status = sc_memory_result_push(out, &entry);
        sc_memory_entry_clear(&entry);
        if (!sc_status_is_ok(status)) {
            break;
        }
    }
    if (memory->api.finalize(stmt) != SC_SQLITE_OK && sc_status_is_ok(status)) {
        status = sc_status_io("sc.memory_sqlite.finalize_failed");
    }
    sc_string_clear(&fts_query);
    if (!sc_status_is_ok(status)) {
        sc_memory_result_clear(out);
        return status;
    }

    sqlite_apply_total_result_byte_limit(query, out);
    return sc_status_ok();
}

static sc_status sqlite_forget(void *impl, sc_str namespace_name, sc_str key)
{
    sqlite_memory *memory = impl;
    sqlite3_stmt *stmt = nullptr;
    sc_status status = sc_status_ok();
    const char *sql = "UPDATE memory_entries SET content='[REDACTED]', metadata_json='', redaction_state=2 WHERE namespace=? AND key=?";

    if (memory == nullptr || namespace_name.ptr == nullptr || key.ptr == nullptr) {
        return sc_status_invalid_argument("sc.memory_sqlite.invalid_argument");
    }
    status = begin_transaction(memory);
    if (sc_status_is_ok(status) &&
        memory->api.prepare_v2(memory->db, sql, -1, &stmt, nullptr) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.prepare_failed");
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 1, namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 2, key);
    }
    if (sc_status_is_ok(status) && memory->api.step(stmt) != SC_SQLITE_DONE) {
        status = sc_status_io("sc.memory_sqlite.step_failed");
    }
    if (stmt != nullptr && memory->api.finalize(stmt) != SC_SQLITE_OK && sc_status_is_ok(status)) {
        status = sc_status_io("sc.memory_sqlite.finalize_failed");
    }
    if (sc_status_is_ok(status)) {
        status = commit_transaction(memory);
    } else {
        rollback_transaction(memory);
    }
    return status;
}

static sc_status sqlite_redact(void *impl, sc_str namespace_name, sc_str key)
{
    sqlite_memory *memory = impl;
    sqlite3_stmt *stmt = nullptr;
    sc_status status = sc_status_ok();
    const char *sql = "UPDATE memory_entries SET content='[REDACTED]', metadata_json='', redaction_state=1 WHERE namespace=? AND key=?";

    if (memory == nullptr || namespace_name.ptr == nullptr || key.ptr == nullptr) {
        return sc_status_invalid_argument("sc.memory_sqlite.invalid_argument");
    }
    status = begin_transaction(memory);
    if (sc_status_is_ok(status) &&
        memory->api.prepare_v2(memory->db, sql, -1, &stmt, nullptr) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.prepare_failed");
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 1, namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 2, key);
    }
    if (sc_status_is_ok(status) && memory->api.step(stmt) != SC_SQLITE_DONE) {
        status = sc_status_io("sc.memory_sqlite.step_failed");
    }
    if (stmt != nullptr && memory->api.finalize(stmt) != SC_SQLITE_OK && sc_status_is_ok(status)) {
        status = sc_status_io("sc.memory_sqlite.finalize_failed");
    }
    if (sc_status_is_ok(status)) {
        status = commit_transaction(memory);
    } else {
        rollback_transaction(memory);
    }
    return status;
}

static sc_status sqlite_purge_namespace(void *impl, sc_str namespace_name)
{
    sqlite_memory *memory = impl;
    sqlite3_stmt *stmt = nullptr;
    sc_status status = sc_status_ok();
    const char *sql = "DELETE FROM memory_entries WHERE namespace=?";

    if (memory == nullptr || namespace_name.ptr == nullptr) {
        return sc_status_invalid_argument("sc.memory_sqlite.invalid_argument");
    }
    status = begin_transaction(memory);
    if (sc_status_is_ok(status) &&
        memory->api.prepare_v2(memory->db, sql, -1, &stmt, nullptr) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.prepare_failed");
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 1, namespace_name);
    }
    if (sc_status_is_ok(status) && memory->api.step(stmt) != SC_SQLITE_DONE) {
        status = sc_status_io("sc.memory_sqlite.step_failed");
    }
    if (stmt != nullptr && memory->api.finalize(stmt) != SC_SQLITE_OK && sc_status_is_ok(status)) {
        status = sc_status_io("sc.memory_sqlite.finalize_failed");
    }
    if (sc_status_is_ok(status)) {
        status = commit_transaction(memory);
    } else {
        rollback_transaction(memory);
    }
    return status;
}

static sc_status sqlite_purge_session(void *impl, sc_str namespace_name, sc_str session_id)
{
    sqlite_memory *memory = impl;
    sqlite3_stmt *stmt = nullptr;
    sc_status status = sc_status_ok();
    const char *sql = "DELETE FROM memory_entries WHERE namespace=? AND session_id=?";

    if (memory == nullptr || namespace_name.ptr == nullptr || session_id.ptr == nullptr) {
        return sc_status_invalid_argument("sc.memory_sqlite.invalid_argument");
    }
    status = begin_transaction(memory);
    if (sc_status_is_ok(status) &&
        memory->api.prepare_v2(memory->db, sql, -1, &stmt, nullptr) != SC_SQLITE_OK) {
        status = sc_status_io("sc.memory_sqlite.prepare_failed");
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 1, namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, 2, session_id);
    }
    if (sc_status_is_ok(status) && memory->api.step(stmt) != SC_SQLITE_DONE) {
        status = sc_status_io("sc.memory_sqlite.step_failed");
    }
    if (stmt != nullptr && memory->api.finalize(stmt) != SC_SQLITE_OK && sc_status_is_ok(status)) {
        status = sc_status_io("sc.memory_sqlite.finalize_failed");
    }
    if (sc_status_is_ok(status)) {
        status = commit_transaction(memory);
    } else {
        rollback_transaction(memory);
    }
    return status;
}

static sc_status sqlite_export_snapshot(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_string *out)
{
    return sqlite_export_snapshot_ex(impl, query, nullptr, alloc, out);
}

static sc_status sqlite_export_snapshot_ex(void *impl,
                                           const sc_memory_query *query,
                                           const sc_memory_export_options *options,
                                           sc_allocator *alloc,
                                           sc_string *out)
{
    sc_memory_result result = {0};
    sc_status status = sc_status_ok();

    status = sqlite_search(impl, query, alloc, &result);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = sqlite_result_to_snapshot(&result, options, alloc, out);
    sc_memory_result_clear(&result);
    return status;
}

static void sqlite_destroy(void *impl)
{
    sqlite_memory *memory = impl;
    if (memory == nullptr) {
        return;
    }
    if (memory->db != nullptr && memory->api.close != nullptr) {
        (void)memory->api.close(memory->db);
    }
#ifndef SC_HAVE_SQLITE
    if (memory->api.library != nullptr) {
        (void)dlclose(memory->api.library);
    }
#endif
    sc_free(memory->alloc, memory, sizeof(*memory), _Alignof(sqlite_memory));
}

#ifndef SC_HAVE_SQLITE
static sc_status load_symbol(void *library, const char *name, void **out)
{
    *out = dlsym(library, name);
    return *out == nullptr ? sc_status_unsupported("sc.memory_sqlite.symbol_missing") : sc_status_ok();
}
#endif

static sc_status load_sqlite(sqlite_api *api)
{
#ifndef SC_HAVE_SQLITE
    sc_status status = sc_status_ok();
#endif

#ifdef SC_HAVE_SQLITE
    *api = (sqlite_api){
        .open = sqlite3_open,
        .close = sqlite3_close,
        .busy_timeout = sqlite3_busy_timeout,
        .exec = sqlite3_exec,
        .prepare_v2 = sqlite3_prepare_v2,
        .bind_text = sqlite3_bind_text,
        .bind_int64 = sqlite3_bind_int64,
        .bind_double = sqlite3_bind_double,
        .step = sqlite3_step,
        .finalize = sqlite3_finalize,
        .column_text = sqlite3_column_text,
        .column_bytes = sqlite3_column_bytes,
        .column_int64 = sqlite3_column_int64,
        .column_double = sqlite3_column_double,
        .errmsg = sqlite3_errmsg,
    };
    return sc_status_ok();
#else
    api->library = dlopen("libsqlite3.so.0", RTLD_NOW | RTLD_LOCAL);
    if (api->library == nullptr) {
        api->library = dlopen("libsqlite3.dylib", RTLD_NOW | RTLD_LOCAL);
    }
    if (api->library == nullptr) {
        return sc_status_unsupported("sc.memory_sqlite.unavailable");
    }

    status = load_symbol(api->library, "sqlite3_open", (void **)&api->open);
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_close", (void **)&api->close);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_busy_timeout", (void **)&api->busy_timeout);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_exec", (void **)&api->exec);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_prepare_v2", (void **)&api->prepare_v2);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_bind_text", (void **)&api->bind_text);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_bind_int64", (void **)&api->bind_int64);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_bind_double", (void **)&api->bind_double);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_step", (void **)&api->step);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_finalize", (void **)&api->finalize);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_column_text", (void **)&api->column_text);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_column_bytes", (void **)&api->column_bytes);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_column_int64", (void **)&api->column_int64);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_column_double", (void **)&api->column_double);
    }
    if (sc_status_is_ok(status)) {
        status = load_symbol(api->library, "sqlite3_errmsg", (void **)&api->errmsg);
    }
    if (!sc_status_is_ok(status)) {
        (void)dlclose(api->library);
        *api = (sqlite_api){0};
    }
    return status;
#endif
}

static sc_status apply_migrations(sqlite_memory *memory)
{
    sc_status status = exec_sql(memory, "PRAGMA journal_mode=WAL");
    if (sc_status_is_ok(status)) {
        status = exec_sql(memory,
                          "CREATE TABLE IF NOT EXISTS memory_entries ("
                          "id TEXT NOT null, "
                          "namespace TEXT NOT null, "
                          "session_id TEXT NOT null DEFAULT '', "
                          "category TEXT NOT null DEFAULT '', "
                          "key TEXT NOT null, "
                          "content TEXT NOT null DEFAULT '', "
                          "timestamp_ns INTEGER NOT null, "
                          "score REAL NOT null DEFAULT 0, "
                          "importance INTEGER NOT null DEFAULT 0, "
                          "superseded_by TEXT NOT null DEFAULT '', "
                          "source TEXT NOT null DEFAULT '', "
                          "metadata_json TEXT NOT null DEFAULT '', "
                          "redaction_state INTEGER NOT null DEFAULT 0, "
                          "content_ref TEXT NOT null DEFAULT '', "
                          "PRIMARY KEY(namespace, key))");
    }
    if (sc_status_is_ok(status)) {
        (void)exec_sql(memory, "ALTER TABLE memory_entries ADD COLUMN source TEXT NOT null DEFAULT ''");
        (void)exec_sql(memory, "ALTER TABLE memory_entries ADD COLUMN metadata_json TEXT NOT null DEFAULT ''");
        (void)exec_sql(memory, "ALTER TABLE memory_entries ADD COLUMN redaction_state INTEGER NOT null DEFAULT 0");
        (void)exec_sql(memory, "ALTER TABLE memory_entries ADD COLUMN content_ref TEXT NOT null DEFAULT ''");
    }
    if (sc_status_is_ok(status)) {
        status = exec_sql(memory, "CREATE INDEX IF NOT EXISTS idx_memory_filters ON memory_entries(namespace, session_id, category)");
    }
    if (sc_status_is_ok(status)) {
        status = exec_sql(memory,
                          "CREATE VIRTUAL TABLE IF NOT EXISTS memory_entries_fts "
                          "USING fts5(key, content, content='memory_entries', content_rowid='rowid')");
    }
    if (sc_status_is_ok(status)) {
        status = exec_sql(memory,
                          "CREATE TRIGGER IF NOT EXISTS memory_entries_fts_ai AFTER INSERT ON memory_entries BEGIN "
                          "INSERT INTO memory_entries_fts(rowid, key, content) VALUES (new.rowid, new.key, new.content); "
                          "END");
    }
    if (sc_status_is_ok(status)) {
        status = exec_sql(memory,
                          "CREATE TRIGGER IF NOT EXISTS memory_entries_fts_ad AFTER DELETE ON memory_entries BEGIN "
                          "INSERT INTO memory_entries_fts(memory_entries_fts, rowid, key, content) "
                          "VALUES('delete', old.rowid, old.key, old.content); "
                          "END");
    }
    if (sc_status_is_ok(status)) {
        status = exec_sql(memory,
                          "CREATE TRIGGER IF NOT EXISTS memory_entries_fts_au AFTER UPDATE ON memory_entries BEGIN "
                          "INSERT INTO memory_entries_fts(memory_entries_fts, rowid, key, content) "
                          "VALUES('delete', old.rowid, old.key, old.content); "
                          "INSERT INTO memory_entries_fts(rowid, key, content) VALUES (new.rowid, new.key, new.content); "
                          "END");
    }
    if (sc_status_is_ok(status)) {
        status = exec_sql(memory, "INSERT INTO memory_entries_fts(memory_entries_fts) VALUES('rebuild')");
    }
    if (sc_status_is_ok(status)) {
        status = exec_sql(memory, "PRAGMA user_version=3");
    }
    return status;
}

static sc_status exec_sql(sqlite_memory *memory, const char *sql)
{
    return memory->api.exec(memory->db, sql, nullptr, nullptr, nullptr) == SC_SQLITE_OK
               ? sc_status_ok()
               : sc_status_io("sc.memory_sqlite.exec_failed");
}

static sc_status begin_transaction(sqlite_memory *memory)
{
    return exec_sql(memory, "BEGIN IMMEDIATE");
}

static sc_status commit_transaction(sqlite_memory *memory)
{
    return exec_sql(memory, "COMMIT");
}

static void rollback_transaction(sqlite_memory *memory)
{
    (void)exec_sql(memory, "ROLLBACK");
}

static sc_status bind_str(sqlite_memory *memory, sqlite3_stmt *stmt, int index, sc_str value)
{
    if (value.ptr == nullptr) {
        value = sc_str_from_cstr("");
    }
    if (value.len > (size_t)INT_MAX) {
        return sc_status_invalid_argument("sc.memory_sqlite.value_too_large");
    }
    return memory->api.bind_text(stmt, index, value.ptr, (int)value.len, SC_SQLITE_STATIC) == SC_SQLITE_OK
               ? sc_status_ok()
               : sc_status_io("sc.memory_sqlite.bind_failed");
}

static sc_status bind_i64(sqlite_memory *memory, sqlite3_stmt *stmt, int index, long long value)
{
    return memory->api.bind_int64(stmt, index, value) == SC_SQLITE_OK
               ? sc_status_ok()
               : sc_status_io("sc.memory_sqlite.bind_failed");
}

static sc_status bind_search_filters(sqlite_memory *memory, sqlite3_stmt *stmt, const sc_memory_query *query, int first_index)
{
    sc_str empty = sc_str_from_cstr("");
    sc_str namespace_name = query == nullptr ? empty : query->namespace_name;
    sc_str session_id = query == nullptr ? empty : query->session_id;
    sc_str category = query == nullptr ? empty : query->category;
    sc_status status = bind_str(memory, stmt, first_index, namespace_name);

    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, first_index + 1, namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, first_index + 2, session_id);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, first_index + 3, session_id);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, first_index + 4, category);
    }
    if (sc_status_is_ok(status)) {
        status = bind_str(memory, stmt, first_index + 5, category);
    }
    if (sc_status_is_ok(status)) {
        status = bind_i64(memory, stmt, first_index + 6, query != nullptr && query->include_superseded ? 1 : 0);
    }
    if (sc_status_is_ok(status)) {
        status = bind_i64(memory, stmt, first_index + 7, query != nullptr && query->include_redacted ? 1 : 0);
    }
    if (sc_status_is_ok(status)) {
        status = bind_i64(memory, stmt, first_index + 8, query != nullptr && query->include_deleted ? 1 : 0);
    }
    if (sc_status_is_ok(status)) {
        status = bind_i64(memory, stmt, first_index + 9, sqlite_search_limit(query));
    }
    return status;
}

static bool sqlite_query_has_match_text(const sc_memory_query *query)
{
    return query != nullptr && query->query.ptr != nullptr && query->query.len > 0;
}

static long long sqlite_search_limit(const sc_memory_query *query)
{
    size_t limit = SC_MEMORY_DEFAULT_MAX_RESULTS;

    if (query != nullptr) {
        limit = query->max_results > 0 ? query->max_results : query->limit;
        if (limit == 0) {
            return -1;
        }
    }
    return limit > (size_t)LLONG_MAX ? LLONG_MAX : (long long)limit;
}

static sc_status sqlite_make_fts_phrase(sc_allocator *alloc, sc_str query, sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "\"");
    for (size_t i = 0; sc_status_is_ok(status) && i < query.len; i += 1) {
        if (query.ptr[i] == '"') {
            status = sc_string_builder_append_cstr(&builder, "\"\"");
        } else {
            status = sc_string_builder_append(&builder, sc_str_from_parts(query.ptr + i, 1));
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "\"");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    sc_string_builder_clear(&builder);
    return status;
}

static sc_status copy_column(sqlite_memory *memory,
                             sqlite3_stmt *stmt,
                             int column,
                             sc_allocator *alloc,
                             sc_string *out)
{
    const unsigned char *text = memory->api.column_text(stmt, column);
    int bytes = memory->api.column_bytes(stmt, column);

    return sc_string_from_str(alloc, sc_str_from_parts((const char *)text, (size_t)bytes), out);
}

static sc_status make_entry_from_stmt(sqlite_memory *memory, sqlite3_stmt *stmt, sc_allocator *alloc, sc_memory_entry *out)
{
    sc_status status = sc_status_ok();

    *out = (sc_memory_entry){.struct_size = sizeof(*out)};
    status = copy_column(memory, stmt, 0, alloc, &out->id);
    if (sc_status_is_ok(status)) {
        status = copy_column(memory, stmt, 1, alloc, &out->namespace_name);
    }
    if (sc_status_is_ok(status)) {
        status = copy_column(memory, stmt, 2, alloc, &out->session_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_column(memory, stmt, 3, alloc, &out->category);
    }
    if (sc_status_is_ok(status)) {
        status = copy_column(memory, stmt, 4, alloc, &out->key);
    }
    if (sc_status_is_ok(status)) {
        status = copy_column(memory, stmt, 5, alloc, &out->content);
    }
    if (sc_status_is_ok(status)) {
        out->timestamp.unix_ns = (int64_t)memory->api.column_int64(stmt, 6);
        out->score = memory->api.column_double(stmt, 7);
        out->importance = (int64_t)memory->api.column_int64(stmt, 8);
        status = copy_column(memory, stmt, 9, alloc, &out->superseded_by_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_column(memory, stmt, 10, alloc, &out->source);
    }
    if (sc_status_is_ok(status)) {
        status = copy_column(memory, stmt, 11, alloc, &out->metadata_json);
    }
    if (sc_status_is_ok(status)) {
        out->redaction_state = (sc_memory_redaction_state)memory->api.column_int64(stmt, 12);
        status = copy_column(memory, stmt, 13, alloc, &out->content_ref);
    }
    return status;
}

static void sqlite_apply_total_result_byte_limit(const sc_memory_query *query, sc_memory_result *out)
{
    size_t total = 0;

    if (query == nullptr || query->max_total_result_bytes == 0 || out == nullptr) {
        return;
    }
    for (size_t i = 0; i < out->entries.len;) {
        sc_memory_entry *entry = sc_vec_at(&out->entries, i);
        size_t bytes = 0;
        size_t next_total = 0;

        if (entry != nullptr && sc_size_add_overflow(entry->content.len, entry->metadata_json.len, &bytes)) {
            bytes = SIZE_MAX;
        }
        if (sc_size_add_overflow(total, bytes, &next_total) || next_total > query->max_total_result_bytes) {
            for (size_t j = i; j < out->entries.len; j += 1) {
                sc_memory_entry *removed = sc_vec_at(&out->entries, j);
                sc_memory_entry_clear(removed);
            }
            out->entries.len = i;
            break;
        }
        total = next_total;
        i += 1;
    }
}

static sc_status sqlite_result_to_snapshot(sc_memory_result *result,
                                           const sc_memory_export_options *options,
                                           sc_allocator *alloc,
                                           sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status = sc_status_ok();

    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "[");
    for (size_t i = 0; sc_status_is_ok(status) && i < result->entries.len; i += 1) {
        const sc_memory_entry *entry = sc_vec_at_const(&result->entries, i);
        if (i > 0) {
            status = sc_string_builder_append_cstr(&builder, ",");
        }
        if (sc_status_is_ok(status)) {
            status = sc_memory_append_entry_json(&builder, entry, options);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(&builder, "]");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    sc_string_builder_clear(&builder);
    return status;
}
