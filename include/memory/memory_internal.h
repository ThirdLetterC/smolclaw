#pragma once

#include "sc/json.h"
#include "sc/memory.h"

#include <stddef.h>

typedef struct sc_memory_store {
    sc_allocator *alloc;
    sc_vec entries;
} sc_memory_store;

enum {
    SC_MEMORY_MAX_NAMESPACE_BYTES = 256,
    SC_MEMORY_MAX_SESSION_BYTES = 256,
    SC_MEMORY_MAX_CATEGORY_BYTES = 128,
    SC_MEMORY_MAX_KEY_BYTES = 512,
    SC_MEMORY_MAX_SOURCE_BYTES = 256,
    SC_MEMORY_MAX_CONTENT_BYTES = 262144,
    SC_MEMORY_MAX_METADATA_BYTES = 65536,
    SC_MEMORY_MAX_QUERY_BYTES = 8192,
    SC_MEMORY_DEFAULT_MAX_RESULTS = 64,
    SC_MEMORY_DEFAULT_MAX_TOTAL_RESULT_BYTES = 1048576,
    SC_MEMORY_IMPORT_MAX_BYTES = 4194304,
    SC_MEMORY_IMPORT_MAX_RECORDS = 10000,
};

void sc_memory_store_init(sc_memory_store *store, sc_allocator *alloc);
sc_status sc_memory_validate_record(const sc_memory_record *record);
sc_status sc_memory_validate_query(const sc_memory_query *query);
sc_status sc_memory_store_put(sc_memory_store *store, const sc_memory_record *record);
sc_status sc_memory_store_get(const sc_memory_store *store,
                              sc_str namespace_name,
                              sc_str key,
                              sc_allocator *alloc,
                              sc_string *out);
sc_status sc_memory_store_search(const sc_memory_store *store,
                                 const sc_memory_query *query,
                                 sc_allocator *alloc,
                                 sc_memory_result *out);
sc_status sc_memory_store_forget(sc_memory_store *store, sc_str namespace_name, sc_str key);
sc_status sc_memory_store_redact(sc_memory_store *store, sc_str namespace_name, sc_str key);
sc_status sc_memory_store_purge_namespace(sc_memory_store *store, sc_str namespace_name);
sc_status sc_memory_store_purge_session(sc_memory_store *store, sc_str namespace_name, sc_str session_id);
sc_status sc_memory_store_export_snapshot(const sc_memory_store *store,
                                          const sc_memory_query *query,
                                          const sc_memory_export_options *options,
                                          sc_allocator *alloc,
                                          sc_string *out);
sc_status sc_memory_store_import_snapshot(sc_memory_store *store,
                                          sc_str snapshot_json,
                                          const sc_memory_import_options *options);
void sc_memory_store_clear(sc_memory_store *store);
bool sc_memory_query_matches_entry(const sc_memory_query *query, const sc_memory_entry *entry);
int sc_memory_entry_compare_desc(const void *left, const void *right);
sc_status sc_memory_append_json_string(sc_string_builder *builder, sc_str value);
sc_status sc_memory_append_entry_json(sc_string_builder *builder,
                                      const sc_memory_entry *entry,
                                      const sc_memory_export_options *options);
sc_status sc_memory_hydrate_entry(sc_memory *memory, const sc_json_value *object, bool allow_secrets);
sc_status sc_memory_hydrate_store_entry(sc_memory_store *store, const sc_json_value *object, bool allow_secrets);
