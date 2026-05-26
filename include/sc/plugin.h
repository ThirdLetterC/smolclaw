#pragma once

#include "sc/api.h"
#include "sc/channel.h"
#include "sc/memory.h"
#include "sc/observer.h"
#include "sc/peripheral.h"
#include "sc/provider.h"
#include "sc/registry.h"
#include "sc/tool.h"

SC_BEGIN_DECLS

#define SC_PLUGIN_DESCRIPTOR_SYMBOL "sc_plugin_descriptor"

typedef struct sc_plugin sc_plugin;
typedef struct sc_plugin_host sc_plugin_host;

typedef enum sc_plugin_permission {
    SC_PLUGIN_PERMISSION_NONE = 0,
    SC_PLUGIN_PERMISSION_FILESYSTEM = 1u << 0u,
    SC_PLUGIN_PERMISSION_NETWORK = 1u << 1u,
    SC_PLUGIN_PERMISSION_SHELL = 1u << 2u,
    SC_PLUGIN_PERMISSION_HARDWARE = 1u << 3u,
    SC_PLUGIN_PERMISSION_MEMORY = 1u << 4u,
    SC_PLUGIN_PERMISSION_CONFIG = 1u << 5u,
    SC_PLUGIN_PERMISSION_SECRETS = 1u << 6u,
    SC_PLUGIN_PERMISSION_PROVIDER = 1u << 7u,
    SC_PLUGIN_PERMISSION_CHANNEL = 1u << 8u,
    SC_PLUGIN_PERMISSION_TOOL = 1u << 9u,
    SC_PLUGIN_PERMISSION_OBSERVER = 1u << 10u,
    SC_PLUGIN_PERMISSION_PERIPHERAL = 1u << 11u
} sc_plugin_permission;

typedef enum sc_plugin_capability {
    SC_PLUGIN_CAP_NONE = 0,
    SC_PLUGIN_CAP_PROVIDER = 1u << 0u,
    SC_PLUGIN_CAP_CHANNEL = 1u << 1u,
    SC_PLUGIN_CAP_TOOL = 1u << 2u,
    SC_PLUGIN_CAP_MEMORY = 1u << 3u,
    SC_PLUGIN_CAP_WASM = 1u << 4u,
    SC_PLUGIN_CAP_OBSERVER = 1u << 5u,
    SC_PLUGIN_CAP_PERIPHERAL = 1u << 6u,
    SC_PLUGIN_CAP_PYTHON = 1u << 7u
} sc_plugin_capability;

typedef struct sc_plugin_manifest {
    size_t struct_size;
    sc_allocator *alloc;
    sc_string name;
    sc_string version;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint64_t capabilities;
    uint64_t requested_permissions;
    sc_string artifact_path;
    sc_string signature_metadata;
} sc_plugin_manifest;

typedef struct sc_plugin_host_api sc_plugin_host_api;
typedef void (*sc_plugin_release_memory_fn)(void *plugin_state, void *ptr, size_t len);

typedef sc_status (*sc_plugin_register_provider_fn)(const sc_plugin_host_api *api,
                                                    const sc_provider_vtab *vtab,
                                                    sc_provider_factory factory);
typedef sc_status (*sc_plugin_register_channel_fn)(const sc_plugin_host_api *api,
                                                   const sc_channel_vtab *vtab,
                                                   sc_channel_factory factory);
typedef sc_status (*sc_plugin_register_tool_fn)(const sc_plugin_host_api *api,
                                                const sc_tool_vtab *vtab,
                                                sc_tool_factory factory);
typedef sc_status (*sc_plugin_register_memory_fn)(const sc_plugin_host_api *api,
                                                  const sc_memory_vtab *vtab,
                                                  sc_memory_factory factory);
typedef sc_status (*sc_observer_factory)(sc_allocator *alloc, sc_observer **out);
typedef sc_status (*sc_plugin_register_observer_fn)(const sc_plugin_host_api *api,
                                                    const sc_observer_vtab *vtab,
                                                    sc_observer_factory factory);
typedef sc_status (*sc_plugin_register_peripheral_fn)(const sc_plugin_host_api *api,
                                                      const sc_peripheral_vtab *vtab,
                                                      sc_peripheral_factory factory);

struct sc_plugin_host_api {
    size_t struct_size;
    void *userdata;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint64_t allowed_permissions;
    sc_plugin_register_provider_fn register_provider;
    sc_plugin_register_channel_fn register_channel;
    sc_plugin_register_tool_fn register_tool;
    sc_plugin_register_memory_fn register_memory;
    sc_plugin_register_observer_fn register_observer;
    sc_plugin_register_peripheral_fn register_peripheral;
};

typedef sc_status (*sc_plugin_init_fn)(const sc_plugin_host_api *api, void **plugin_state);
typedef void (*sc_plugin_shutdown_fn)(void *plugin_state);

typedef struct sc_plugin_descriptor {
    size_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    const char *name;
    const char *version;
    sc_plugin_init_fn init;
    sc_plugin_shutdown_fn shutdown;
    const char *metadata_json;
    uint64_t requested_capabilities;
    uint64_t requested_permissions;
    size_t minimum_host_descriptor_size;
    const char *manifest_schema_ref;
    sc_plugin_release_memory_fn release_memory;
} sc_plugin_descriptor;

typedef const sc_plugin_descriptor *(*sc_plugin_descriptor_getter)(void);

typedef struct sc_plugin_host_options {
    size_t struct_size;
    sc_provider_registry *providers;
    sc_channel_registry *channels;
    sc_tool_registry *tools;
    sc_memory_registry *memory_backends;
    sc_observer_list *observers;
    sc_peripheral_registry *peripherals;
    uint64_t allowed_permissions;
    bool allow_memory_backends;
    const sc_str *plugin_roots;
    size_t plugin_root_count;
    const sc_str *allowed_extensions;
    size_t allowed_extension_count;
    bool dynamic_modules_enabled;
    bool wasm_enabled;
    bool python_enabled;
} sc_plugin_host_options;

void sc_plugin_manifest_init(sc_plugin_manifest *manifest, sc_allocator *alloc);
sc_status sc_plugin_manifest_parse(sc_allocator *alloc, sc_str json_text, sc_plugin_manifest *out);
sc_status sc_plugin_manifest_copy(sc_allocator *alloc,
                                  const sc_plugin_manifest *manifest,
                                  sc_plugin_manifest *out);
void sc_plugin_manifest_clear(sc_plugin_manifest *manifest);

sc_status sc_plugin_host_new(sc_allocator *alloc,
                             const sc_plugin_host_options *options,
                             sc_plugin_host **out);
size_t sc_plugin_host_provider_count(const sc_plugin_host *host);
size_t sc_plugin_host_channel_count(const sc_plugin_host *host);
size_t sc_plugin_host_tool_count(const sc_plugin_host *host);
size_t sc_plugin_host_memory_count(const sc_plugin_host *host);
size_t sc_plugin_host_peripheral_count(const sc_plugin_host *host);
sc_status sc_plugin_host_create_provider(const sc_plugin_host *host,
                                         sc_str name,
                                         sc_allocator *alloc,
                                         sc_provider **out);
sc_status sc_plugin_host_create_channel(const sc_plugin_host *host,
                                        sc_str name,
                                        sc_allocator *alloc,
                                        sc_channel **out);
sc_status sc_plugin_host_create_tool(const sc_plugin_host *host,
                                     sc_str name,
                                     const sc_tool_context *context,
                                     sc_allocator *alloc,
                                     sc_tool **out);
sc_status sc_plugin_host_create_memory(const sc_plugin_host *host,
                                       sc_str name,
                                       sc_allocator *alloc,
                                       sc_memory **out);
sc_status sc_plugin_host_create_peripheral(const sc_plugin_host *host,
                                           sc_str name,
                                           sc_allocator *alloc,
                                           sc_peripheral **out);
void sc_plugin_host_destroy(sc_plugin_host *host);

sc_status sc_plugin_load_descriptor(sc_plugin_host *host,
                                    const sc_plugin_descriptor *descriptor,
                                    const sc_plugin_manifest *manifest,
                                    sc_plugin **out);
sc_status sc_plugin_load_path(sc_plugin_host *host, const sc_plugin_manifest *manifest, sc_plugin **out);
const sc_plugin_manifest *sc_plugin_manifest_of(const sc_plugin *plugin);
sc_status sc_plugin_acquire(sc_plugin *plugin);
void sc_plugin_release(sc_plugin *plugin);
size_t sc_plugin_active_references(const sc_plugin *plugin);
sc_status sc_plugin_unload(sc_plugin *plugin);
void sc_plugin_release_memory(sc_plugin *plugin, void *ptr, size_t len);

SC_END_DECLS
