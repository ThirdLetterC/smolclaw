#pragma once

#include "sc/allocator.h"
#include "sc/contract.h"
#include "sc/result.h"
#include "sc/string.h"
#include "sc/time.h"
#include "sc/vector.h"

SC_BEGIN_DECLS

/*
 * Ownership/threading: records and lookup keys are borrowed for the call. Get
 * results are caller-owned. The handle owns impl and calls destroy exactly
 * once. The wrapper does not synchronize access; memory backends document their
 * own thread-safety.
 */
typedef struct sc_memory sc_memory;

typedef enum sc_memory_backend_capability {
    SC_MEMORY_BACKEND_CAP_STORE = 1u << 0u,
    SC_MEMORY_BACKEND_CAP_RETRIEVE = 1u << 1u,
    SC_MEMORY_BACKEND_CAP_DELETE = 1u << 2u,
    SC_MEMORY_BACKEND_CAP_REDACT = 1u << 3u,
    SC_MEMORY_BACKEND_CAP_EMBEDDINGS = 1u << 4u,
    SC_MEMORY_BACKEND_CAP_SNAPSHOTS = 1u << 5u,
    SC_MEMORY_BACKEND_CAP_IMPORT_EXPORT = 1u << 6u,
    SC_MEMORY_BACKEND_CAP_MIGRATIONS = 1u << 7u,
} sc_memory_backend_capability;

typedef enum sc_memory_persistence_type {
    SC_MEMORY_PERSISTENCE_NONE = 0,
    SC_MEMORY_PERSISTENCE_FILE,
    SC_MEMORY_PERSISTENCE_SQLITE,
    SC_MEMORY_PERSISTENCE_REMOTE,
    SC_MEMORY_PERSISTENCE_VECTOR,
} sc_memory_persistence_type;

typedef enum sc_memory_redaction_state {
    SC_MEMORY_REDACTION_NONE = 0,
    SC_MEMORY_REDACTION_REDACTED,
    SC_MEMORY_REDACTION_DELETED,
} sc_memory_redaction_state;

typedef enum sc_memory_retention_policy {
    SC_MEMORY_RETENTION_DEFAULT = 0,
    SC_MEMORY_RETENTION_EPHEMERAL,
    SC_MEMORY_RETENTION_PERSISTENT,
    SC_MEMORY_RETENTION_EXPIRES_AT,
} sc_memory_retention_policy;

typedef struct sc_memory_import_options {
    size_t struct_size;
    size_t max_bytes;
    size_t max_records;
    bool allow_secrets;
} sc_memory_import_options;

typedef struct sc_memory_export_options {
    size_t struct_size;
    bool include_raw_sensitive_content;
    bool include_redacted;
    bool include_deleted;
} sc_memory_export_options;

typedef struct sc_memory_embedding_spec {
    size_t struct_size;
    sc_str record_id;
    sc_str model_id;
    size_t dimensions;
    size_t batch_size;
    bool contains_secret;
} sc_memory_embedding_spec;

typedef struct sc_memory_record {
    size_t struct_size;
    sc_str id;
    sc_str namespace_name;
    sc_str session_id;
    sc_str category;
    sc_str key;
    sc_str value;
    double score;
    int64_t importance;
    sc_str superseded_by_id;
    sc_str source;
    sc_str metadata_json;
    sc_memory_redaction_state redaction_state;
    sc_memory_retention_policy retention_policy;
    sc_wall_time expires_at;
    sc_str content_ref;
    bool allow_sensitive_content;
} sc_memory_record;

typedef struct sc_memory_entry {
    size_t struct_size;
    sc_string id;
    sc_string namespace_name;
    sc_string session_id;
    sc_string category;
    sc_string key;
    sc_string content;
    sc_wall_time timestamp;
    double score;
    int64_t importance;
    sc_string superseded_by_id;
    sc_string source;
    sc_string metadata_json;
    sc_memory_redaction_state redaction_state;
    sc_memory_retention_policy retention_policy;
    sc_wall_time expires_at;
    sc_string content_ref;
} sc_memory_entry;

typedef struct sc_memory_query {
    size_t struct_size;
    sc_str namespace_name;
    sc_str session_id;
    sc_str category;
    sc_str query;
    size_t limit;
    bool include_superseded;
    size_t max_query_bytes;
    size_t max_results;
    size_t max_total_result_bytes;
    bool include_redacted;
    bool include_deleted;
} sc_memory_query;

typedef struct sc_memory_result {
    sc_allocator *alloc;
    sc_vec entries;
} sc_memory_result;

typedef struct sc_memory_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    const char *display_name;
    const char *feature_flag;
    uint64_t capabilities;
    sc_stability_tier stability;
    sc_status (*put)(void *impl, const sc_memory_record *record);
    sc_status (*get)(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out);
    sc_status (*search)(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_memory_result *out);
    sc_status (*forget)(void *impl, sc_str namespace_name, sc_str key);
    sc_status (*purge_namespace)(void *impl, sc_str namespace_name);
    sc_status (*purge_session)(void *impl, sc_str namespace_name, sc_str session_id);
    sc_status (*export_snapshot)(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_string *out);
    void (*destroy)(void *impl);
    const char *config_schema_ref;
    uint64_t memory_capabilities;
    sc_memory_persistence_type persistence_type;
    uint32_t migration_version;
    bool supports_migrations;
    sc_status (*redact)(void *impl, sc_str namespace_name, sc_str key);
    sc_status (*export_snapshot_ex)(void *impl,
                                    const sc_memory_query *query,
                                    const sc_memory_export_options *options,
                                    sc_allocator *alloc,
                                    sc_string *out);
} sc_memory_vtab;

static inline bool sc_memory_handle_is_null(const sc_memory *memory)
{
    return memory == nullptr;
}

bool sc_memory_vtab_valid(const sc_memory_vtab *vtab);
sc_status sc_memory_new(sc_allocator *alloc, const sc_memory_vtab *vtab, void *impl, sc_memory **out);
sc_status sc_memory_put(sc_memory *memory, const sc_memory_record *record);
sc_status sc_memory_get(sc_memory *memory, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out);
sc_status sc_memory_search(sc_memory *memory, const sc_memory_query *query, sc_allocator *alloc, sc_memory_result *out);
sc_status sc_memory_forget(sc_memory *memory, sc_str namespace_name, sc_str key);
sc_status sc_memory_purge_namespace(sc_memory *memory, sc_str namespace_name);
sc_status sc_memory_purge_session(sc_memory *memory, sc_str namespace_name, sc_str session_id);
sc_status sc_memory_redact(sc_memory *memory, sc_str namespace_name, sc_str key);
sc_status sc_memory_export_snapshot(sc_memory *memory, const sc_memory_query *query, sc_allocator *alloc, sc_string *out);
sc_status sc_memory_export_snapshot_ex(sc_memory *memory,
                                       const sc_memory_query *query,
                                       const sc_memory_export_options *options,
                                       sc_allocator *alloc,
                                       sc_string *out);
const sc_memory_vtab *sc_memory_vtab_of(const sc_memory *memory);
void sc_memory_destroy(sc_memory *memory);
sc_status sc_memory_apply_retention(sc_memory *memory, sc_str namespace_name, size_t max_entries);
sc_status sc_memory_find_duplicate(sc_memory *memory,
                                   sc_str namespace_name,
                                   sc_str content,
                                   sc_allocator *alloc,
                                   sc_string *out_key);
sc_status sc_memory_hydrate_snapshot(sc_memory *memory, sc_str snapshot_json);
sc_status sc_memory_import_snapshot(sc_memory *memory,
                                    sc_str snapshot_json,
                                    const sc_memory_import_options *options);
sc_status sc_memory_validate_embedding(const sc_memory_vtab *vtab,
                                       const sc_memory_embedding_spec *expected,
                                       const sc_memory_embedding_spec *actual);

void sc_memory_result_init(sc_memory_result *result, sc_allocator *alloc);
sc_status sc_memory_result_push(sc_memory_result *result, const sc_memory_entry *entry);
void sc_memory_entry_clear(sc_memory_entry *entry);
void sc_memory_result_clear(sc_memory_result *result);
bool sc_memory_key_is_reserved(sc_str key);

sc_status sc_memory_none_new(sc_allocator *alloc, sc_memory **out);
sc_status sc_memory_markdown_open(sc_allocator *alloc, sc_str path, sc_memory **out);
sc_status sc_memory_sqlite_open(sc_allocator *alloc, sc_str path, sc_memory **out);

SC_END_DECLS
