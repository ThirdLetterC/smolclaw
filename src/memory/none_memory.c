#include "memory/memory_internal.h"

#include "sc/contract.h"

static sc_status none_put(void *impl, const sc_memory_record *record);
static sc_status none_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out);
static sc_status none_search(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_memory_result *out);
static sc_status none_forget(void *impl, sc_str namespace_name, sc_str key);
static sc_status none_purge_namespace(void *impl, sc_str namespace_name);
static sc_status none_purge_session(void *impl, sc_str namespace_name, sc_str session_id);
static sc_status none_export_snapshot(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_string *out);
static void none_destroy(void *impl);

static const sc_memory_vtab none_vtab = {
    .struct_size = sizeof(sc_memory_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "none",
    .display_name = "No-op memory",
    .feature_flag = "SC_MEMORY_NONE",
    .capabilities = 0,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .put = none_put,
    .get = none_get,
    .search = none_search,
    .forget = none_forget,
    .purge_namespace = none_purge_namespace,
    .purge_session = none_purge_session,
    .export_snapshot = none_export_snapshot,
    .destroy = none_destroy,
    .config_schema_ref = "sc.schema.memory.none.v1",
    .memory_capabilities = 0,
    .persistence_type = SC_MEMORY_PERSISTENCE_NONE,
    .migration_version = 0,
    .supports_migrations = false,
};

sc_status sc_memory_none_new(sc_allocator *alloc, sc_memory **out)
{
    return sc_memory_new(alloc, &none_vtab, nullptr, out);
}

static sc_status none_put(void *impl, const sc_memory_record *record)
{
    (void)impl;
    if (record == nullptr) {
        return sc_status_invalid_argument("sc.memory_none.invalid_argument");
    }
    return sc_memory_validate_record(record);
}

static sc_status none_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out)
{
    (void)impl;
    (void)namespace_name;
    (void)key;
    (void)alloc;
    (void)out;
    return sc_status_invalid_argument("sc.memory_none.not_found");
}

static sc_status none_search(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_memory_result *out)
{
    (void)impl;
    (void)query;
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.memory_none.invalid_argument");
    }
    sc_memory_result_init(out, alloc);
    return sc_status_ok();
}

static sc_status none_forget(void *impl, sc_str namespace_name, sc_str key)
{
    (void)impl;
    (void)namespace_name;
    (void)key;
    return sc_status_ok();
}

static sc_status none_purge_namespace(void *impl, sc_str namespace_name)
{
    (void)impl;
    (void)namespace_name;
    return sc_status_ok();
}

static sc_status none_purge_session(void *impl, sc_str namespace_name, sc_str session_id)
{
    (void)impl;
    (void)namespace_name;
    (void)session_id;
    return sc_status_ok();
}

static sc_status none_export_snapshot(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_string *out)
{
    (void)impl;
    (void)query;
    return sc_string_from_cstr(alloc, "[]", out);
}

static void none_destroy(void *impl)
{
    (void)impl;
}
