#pragma once

#include "sc/async.h"
#include "sc/allocator.h"
#include "sc/buffer.h"
#include "sc/contract.h"
#include "sc/json.h"
#include "sc/memory.h"
#include "sc/result.h"
#include "sc/security.h"
#include "sc/string.h"

SC_BEGIN_DECLS

/*
 * Ownership/threading: specs and calls borrow inputs. Tool results are
 * caller-owned. The handle owns impl and calls destroy exactly once. The
 * wrapper does not synchronize invocations; tool implementations document
 * their own thread-safety.
 */
typedef struct sc_tool sc_tool;
typedef struct sc_cancel_token sc_cancel_token;
typedef struct sc_config sc_config;
typedef struct sc_cron_job_store sc_cron_job_store;
typedef struct sc_cron_run_store sc_cron_run_store;

typedef enum sc_tool_capability_category {
    SC_TOOL_CAPABILITY_NONE = 0,
    SC_TOOL_CAPABILITY_FILESYSTEM = 1u << 0u,
    SC_TOOL_CAPABILITY_MEMORY = 1u << 1u,
    SC_TOOL_CAPABILITY_NETWORK = 1u << 2u,
    SC_TOOL_CAPABILITY_PROCESS = 1u << 3u,
    SC_TOOL_CAPABILITY_MCP = 1u << 4u,
    SC_TOOL_CAPABILITY_BROWSER = 1u << 5u,
    SC_TOOL_CAPABILITY_HARDWARE = 1u << 6u,
    SC_TOOL_CAPABILITY_SAAS = 1u << 7u,
} sc_tool_capability_category;

typedef enum sc_tool_side_effect {
    SC_TOOL_SIDE_EFFECT_NONE = 0,
    SC_TOOL_SIDE_EFFECT_READ,
    SC_TOOL_SIDE_EFFECT_WRITE,
    SC_TOOL_SIDE_EFFECT_NETWORK,
    SC_TOOL_SIDE_EFFECT_PROCESS,
    SC_TOOL_SIDE_EFFECT_DESTRUCTIVE,
} sc_tool_side_effect;

typedef struct sc_tool_context {
    size_t struct_size;
    const sc_security_policy *policy;
    const sc_estop_state *estop;
    sc_receipt_chain *receipts;
    sc_memory *memory;
    size_t max_output_bytes;
    size_t max_arg_bytes;
    int64_t timeout_ms;
    const sc_cancel_token *cancel_token;
    const sc_config *config;
    sc_cron_job_store *cron_jobs;
    sc_cron_run_store *cron_runs;
    sc_tool *const *tools;
    size_t tool_capacity;
} sc_tool_context;

typedef struct sc_tool_spec {
    size_t struct_size;
    sc_str name;
    sc_str description;
    const sc_json_value *input_schema;
    uint64_t capabilities;
    sc_tool_risk risk;
    const sc_json_value *output_schema;
    sc_tool_capability_category capability_category;
    sc_tool_side_effect side_effect;
    sc_autonomy_level default_autonomy;
    sc_str catalog_metadata_key;
} sc_tool_spec;

typedef struct sc_tool_call {
    size_t struct_size;
    sc_str call_id;
    const sc_json_value *args;
    const sc_cancel_token *cancel_token;
} sc_tool_call;

typedef struct sc_tool_result {
    size_t struct_size;
    bool success;
    sc_string output;
    sc_string attachment_content_type;
    sc_string attachment_filename;
    sc_bytes attachment_bytes;
    sc_attachment_delivery attachment_delivery;
} sc_tool_result;

typedef void (*sc_tool_invoke_complete_fn)(void *user_data,
                                           const sc_tool_result *result,
                                           sc_status status);

typedef struct sc_tool_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    const char *display_name;
    const char *feature_flag;
    uint64_t capabilities;
    sc_stability_tier stability;
    sc_status (*spec)(void *impl, sc_tool_spec *out);
    sc_status (*invoke)(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
    void (*destroy)(void *impl);
    sc_status (*invoke_async)(void *impl,
                              sc_async_context *context,
                              const sc_tool_call *call,
                              sc_allocator *alloc,
                              sc_tool_invoke_complete_fn complete,
                              void *complete_user_data,
                              sc_async_op **out);
} sc_tool_vtab;

static inline bool sc_tool_handle_is_null(const sc_tool *tool)
{
    return tool == nullptr;
}

bool sc_tool_vtab_valid(const sc_tool_vtab *vtab);
sc_status sc_tool_new(sc_allocator *alloc, const sc_tool_vtab *vtab, void *impl, sc_tool **out);
sc_status sc_tool_spec_get(sc_tool *tool, sc_tool_spec *out);
sc_status sc_tool_invoke(sc_tool *tool, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
sc_status sc_tool_invoke_async(sc_tool *tool,
                               sc_async_context *context,
                               const sc_tool_call *call,
                               sc_allocator *alloc,
                               sc_tool_invoke_complete_fn complete,
                               void *complete_user_data,
                               sc_async_op **out);
sc_status sc_tool_validate_call_against_schema(const sc_tool_spec *spec, const sc_tool_call *call);
const sc_tool_vtab *sc_tool_vtab_of(const sc_tool *tool);
void sc_tool_destroy(sc_tool *tool);
void sc_tool_result_clear(sc_tool_result *result);

sc_status sc_tool_registry_model_specs_from_tools(sc_tool **tools,
                                                  size_t tool_count,
                                                  sc_allocator *alloc,
                                                  sc_json_value **out);

sc_status sc_tool_file_read_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_file_write_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_file_list_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_content_search_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_glob_search_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_shell_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_http_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_web_search_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_browser_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_browser_screenshot_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_pdf_extract_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_time_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_memory_store_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_memory_recall_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_memory_search_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_memory_pin_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_memory_forget_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_memory_export_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_memory_purge_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_sop_inspect_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_sop_advance_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_cron_list_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_cron_upsert_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_cron_remove_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_tool_diagnostics_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_policy_explain_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_tool_registry_list_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_dependency_status_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_capability_matrix_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_resource_usage_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_approval_test_new(sc_allocator *alloc, const sc_tool_context *context, sc_tool **out);
sc_status sc_tool_mcp_server_new(sc_allocator *alloc,
                                  const sc_tool_context *context,
                                  sc_str server_name,
                                  sc_str transport,
                                  sc_str command,
                                  sc_str args,
                                  sc_str url,
                                  sc_str headers,
                                  sc_tool **out);
sc_status sc_tool_mcp_server_tool_new(sc_allocator *alloc,
                                      const sc_tool_context *context,
                                      sc_str server_name,
                                      sc_str tool_name,
                                      sc_str transport,
                                      sc_str command,
                                      sc_str args,
                                      sc_str url,
                                      sc_str headers,
                                      sc_tool **out);
sc_status sc_tool_mcp_server_discover(sc_allocator *alloc,
                                      const sc_tool_context *context,
                                      sc_str server_name,
                                      sc_str transport,
                                      sc_str command,
                                      sc_str args,
                                      sc_str url,
                                      sc_str headers,
                                      sc_tool ***out_tools,
                                      size_t *out_count);
sc_status sc_tool_rate_limit_wrapper_new(sc_allocator *alloc, sc_tool *inner, size_t max_calls, sc_tool **out);
sc_status sc_tool_domain_guard_wrapper_new(sc_allocator *alloc,
                                           sc_tool *inner,
                                           const sc_security_policy *policy,
                                           sc_tool **out);
sc_status sc_tool_timeout_wrapper_new(sc_allocator *alloc,
                                      sc_tool *inner,
                                      int64_t timeout_ms,
                                      sc_tool **out);

SC_END_DECLS
