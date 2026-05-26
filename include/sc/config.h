#pragma once

#include <stddef.h>
#include <stdint.h>

#include "sc/allocator.h"
#include "sc/map.h"
#include "sc/result.h"
#include "sc/sc_config_schema.h"
#include "sc/string.h"
#include "sc/toml.h"
#include "sc/vector.h"

typedef enum sc_config_source_kind {
    SC_CONFIG_SOURCE_BUILTIN_DEFAULTS = 0,
    SC_CONFIG_SOURCE_DEFAULT_FILE,
    SC_CONFIG_SOURCE_WORKSPACE_MARKER,
    SC_CONFIG_SOURCE_EXPLICIT_FILE,
    SC_CONFIG_SOURCE_ENVIRONMENT,
    SC_CONFIG_SOURCE_RUNTIME
} sc_config_source_kind;

typedef struct sc_config_source {
    sc_config_source_kind kind;
    sc_str source_path;
    sc_str body;
    bool present;
} sc_config_source;

typedef struct sc_config_override {
    sc_str path;
    sc_str value;
} sc_config_override;

typedef struct sc_config_load_options {
    sc_config_source default_file;
    sc_config_source workspace_marker;
    sc_config_source explicit_file;
    const sc_config_override *env_overrides;
    size_t env_override_count;
    const sc_config_override *runtime_overrides;
    size_t runtime_override_count;
} sc_config_load_options;

typedef struct sc_config_diag {
    sc_string path;
    sc_string source_path;
    size_t line;
    size_t column;
    const char *error_key;
} sc_config_diag;

typedef struct sc_secret_store sc_secret_store;

typedef struct sc_secret_store_vtab {
    size_t struct_size;
    sc_status (*put)(void *impl, sc_str path, sc_str value);
    sc_status (*get)(void *impl, sc_str path, sc_allocator *alloc, sc_string *out);
    void (*destroy)(void *impl);
} sc_secret_store_vtab;

typedef struct sc_provider_alias {
    sc_string alias;
    sc_string provider;
    sc_string model;
    sc_string credential_env;
} sc_provider_alias;

typedef struct sc_provider_resolved {
    sc_str provider;
    sc_str model;
    sc_str credential_env;
} sc_provider_resolved;

typedef struct sc_proxy_rule {
    sc_string service;
    sc_string proxy_url;
    bool enabled;
} sc_proxy_rule;

typedef enum sc_mcp_transport {
    SC_MCP_TRANSPORT_STDIO = 0,
    SC_MCP_TRANSPORT_SSE,
    SC_MCP_TRANSPORT_HTTP,
    SC_MCP_TRANSPORT_UNKNOWN
} sc_mcp_transport;

typedef struct sc_mcp_server {
    sc_string name;
    sc_mcp_transport transport;
    sc_string command;
    sc_string args;
    sc_string url;
    sc_string headers;
    bool enabled;
    bool deferred_loading;
    bool deferred_loading_set;
} sc_mcp_server;

typedef struct sc_mcp_server_view {
    sc_str name;
    sc_mcp_transport transport;
    sc_str command;
    sc_str args;
    sc_str url;
    sc_str headers;
    bool enabled;
    bool deferred_loading;
} sc_mcp_server_view;

typedef struct sc_config {
    sc_allocator *alloc;
    uint32_t schema_version;
    sc_string runtime_autonomy_level;
    sc_string runtime_workspace_path;
    sc_string provider_default;
    sc_string provider_default_model;
    sc_string provider_api_key;
    sc_string provider_api_key_env;
    sc_string model_route_default;
    int64_t reliability_max_retries;
    int64_t reliability_retry_backoff_ms;
    int64_t reliability_timeout_ms;
    sc_string memory_backend;
    sc_string gateway_bind;
    int64_t gateway_port;
    bool proxy_enabled;
    sc_string proxy_default_url;
    bool mcp_deferred_loading;
    sc_vec provider_aliases;
    sc_vec proxy_rules;
    sc_vec mcp_servers;
    sc_map values;
    sc_map sources;
    sc_secret_store *secret_store;
    bool secret_store_enabled;
} sc_config;

void sc_config_init(sc_config *config, sc_allocator *alloc);
sc_status sc_config_init_defaults(sc_config *config, sc_allocator *alloc);
sc_status sc_config_load(sc_allocator *alloc,
                         const sc_config_load_options *options,
                         sc_config *out,
                         sc_config_diag *diag);
sc_status sc_config_apply_toml_source(sc_config *config,
                                      const sc_toml_source *source,
                                      sc_config_source_kind kind,
                                      sc_config_diag *diag);
sc_status sc_config_apply_override(sc_config *config,
                                   sc_config_source_kind kind,
                                   sc_str path,
                                   sc_str value,
                                   sc_config_diag *diag);
sc_status sc_config_get_prop(const sc_config *config, sc_str path, sc_allocator *alloc, sc_string *out);
bool sc_config_has_prop(const sc_config *config, sc_str path);
bool sc_config_get_bool(const sc_config *config, sc_str path, bool fallback);
int64_t sc_config_get_int(const sc_config *config, sc_str path, int64_t fallback);
sc_status sc_config_get_prop_redacted(const sc_config *config,
                                      sc_str path,
                                      sc_allocator *alloc,
                                      sc_string *out);
sc_status sc_config_export_redacted(const sc_config *config, sc_allocator *alloc, sc_string *out);
sc_status sc_config_attach_secret_store(sc_config *config, sc_secret_store *store, bool enabled);
sc_status sc_config_get_prop_source(const sc_config *config,
                                    sc_str path,
                                    sc_allocator *alloc,
                                    sc_string *out);
sc_status sc_config_set_prop(sc_config *config, sc_str path, sc_str value);
sc_status sc_config_validate(const sc_config *config, sc_config_diag *diag);
sc_status sc_config_migrate(sc_config *config, uint32_t from_version, sc_config_diag *diag);
sc_status sc_config_describe_fields(sc_allocator *alloc, bool include_secret_paths, sc_string *out);
sc_status sc_config_add_provider_alias(sc_config *config,
                                       sc_str alias,
                                       sc_str provider,
                                       sc_str model,
                                       sc_str credential_env);
bool sc_config_resolve_provider_alias(const sc_config *config, sc_str alias, sc_provider_resolved *out);
sc_status sc_config_provider_credential_source(const sc_config *config, sc_allocator *alloc, sc_string *out);
sc_status sc_config_add_proxy_rule(sc_config *config, sc_str service, sc_str proxy_url, bool enabled);
sc_status sc_config_match_proxy(const sc_config *config, sc_str service, sc_allocator *alloc, sc_string *out);
sc_mcp_transport sc_mcp_transport_from_str(sc_str transport);
sc_str sc_mcp_transport_to_str(sc_mcp_transport transport);
sc_status sc_config_add_mcp_server(sc_config *config,
                                   sc_str name,
                                   sc_mcp_transport transport,
                                   sc_str command,
                                   sc_str args,
                                   sc_str url,
                                   sc_str headers,
                                   bool enabled,
                                   bool deferred_loading);
size_t sc_config_mcp_server_count(const sc_config *config);
bool sc_config_mcp_server_at(const sc_config *config, size_t index, sc_mcp_server_view *out);
bool sc_config_find_mcp_server(const sc_config *config, sc_str name, sc_mcp_server_view *out);
void sc_config_diag_clear(sc_config_diag *diag);
void sc_config_clear(sc_config *config);
sc_status sc_secret_store_new(sc_allocator *alloc,
                              const sc_secret_store_vtab *vtab,
                              void *impl,
                              sc_secret_store **out);
sc_status sc_secret_store_memory_new(sc_allocator *alloc, sc_secret_store **out);
sc_status sc_secret_store_file_new(sc_allocator *alloc, sc_str path, sc_secret_store **out);
sc_status sc_secret_store_put(sc_secret_store *store, sc_str path, sc_str value);
sc_status sc_secret_store_get(sc_secret_store *store, sc_str path, sc_allocator *alloc, sc_string *out);
void sc_secret_store_destroy(sc_secret_store *store);
