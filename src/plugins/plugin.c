#define _XOPEN_SOURCE 700

#include "sc/plugin.h"

#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sc/json.h"

#include "plugin_script_internal.h"

#ifdef SC_HAVE_WAMR
#include <wasm_export.h>
#endif

#include <dlfcn.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct sc_plugin_provider_entry {
    sc_plugin *plugin;
    const sc_provider_vtab *vtab;
    sc_provider_factory factory;
} sc_plugin_provider_entry;

typedef struct sc_plugin_channel_entry {
    sc_plugin *plugin;
    const sc_channel_vtab *vtab;
    sc_channel_factory factory;
} sc_plugin_channel_entry;

typedef struct sc_plugin_tool_entry {
    sc_plugin *plugin;
    const sc_tool_vtab *vtab;
    sc_tool_factory factory;
} sc_plugin_tool_entry;

typedef struct sc_plugin_memory_entry {
    sc_plugin *plugin;
    const sc_memory_vtab *vtab;
    sc_memory_factory factory;
} sc_plugin_memory_entry;

typedef struct sc_plugin_peripheral_entry {
    sc_plugin *plugin;
    const sc_peripheral_vtab *vtab;
    sc_peripheral_factory factory;
} sc_plugin_peripheral_entry;

#ifdef SC_HAVE_WAMR
typedef struct sc_wasm_plugin_state sc_wasm_plugin_state;

typedef struct sc_wasm_tool_entry {
    sc_wasm_plugin_state *state;
    sc_string name;
    sc_string description;
    sc_string catalog_metadata_key;
    sc_string invoke_name;
    sc_json_value *input_schema;
    sc_json_value *output_schema;
    sc_tool_vtab vtab;
    uint64_t capabilities;
    sc_tool_risk risk;
    sc_tool_capability_category capability_category;
    sc_tool_side_effect side_effect;
    sc_autonomy_level default_autonomy;
} sc_wasm_tool_entry;

typedef struct sc_wasm_call_state {
    sc_allocator *alloc;
    sc_tool_result *result;
    bool result_set;
    sc_status_code status_code;
    sc_string status_key;
} sc_wasm_call_state;

struct sc_wasm_plugin_state {
    sc_allocator *alloc;
    sc_plugin *plugin;
    uint8_t *module_bytes;
    size_t module_size;
    wasm_module_t module;
    wasm_module_inst_t module_inst;
    wasm_exec_env_t exec_env;
    sc_ptr_vec tools;
    sc_wasm_call_state *active_call;
};
#endif

typedef struct sc_script_plugin_state sc_script_plugin_state;

typedef struct sc_script_tool_entry {
    sc_script_plugin_state *state;
    sc_string name;
    sc_string description;
    sc_string catalog_metadata_key;
    sc_json_value *input_schema;
    sc_json_value *output_schema;
    sc_tool_vtab vtab;
    void *invoke_ref;
    uint64_t capabilities;
    sc_tool_risk risk;
    sc_tool_capability_category capability_category;
    sc_tool_side_effect side_effect;
    sc_autonomy_level default_autonomy;
} sc_script_tool_entry;

struct sc_script_plugin_state {
    sc_allocator *alloc;
    sc_plugin *plugin;
    uint64_t language_capability;
    void *runtime_state;
    const sc_script_runtime_vtab *vtab;
    sc_ptr_vec tools;
};

struct sc_plugin_host {
    sc_allocator *alloc;
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
    sc_vec provider_entries;
    sc_vec channel_entries;
    sc_vec tool_entries;
    sc_vec memory_entries;
    sc_vec peripheral_entries;
    sc_ptr_vec loaded_plugins;
};

struct sc_plugin {
    sc_allocator *alloc;
    sc_plugin_host *host;
    const sc_plugin_descriptor *descriptor;
    sc_plugin_manifest manifest;
    void *state;
    void *dynamic_handle;
#ifdef SC_HAVE_WAMR
    sc_wasm_plugin_state *wasm_state;
#endif
    sc_script_plugin_state *script_state;
    size_t active_refs;
    bool initialized;
};

typedef struct sc_plugin_registration_context {
    sc_plugin_host *host;
    sc_plugin *plugin;
} sc_plugin_registration_context;

typedef struct sc_plugin_tool_adapter {
    sc_allocator *alloc;
    sc_plugin *plugin;
    sc_tool *inner;
    sc_tool_context context;
} sc_plugin_tool_adapter;

typedef struct sc_plugin_provider_adapter {
    sc_allocator *alloc;
    sc_plugin *plugin;
    sc_provider *inner;
    sc_provider_vtab vtab;
} sc_plugin_provider_adapter;

typedef struct sc_plugin_channel_adapter {
    sc_allocator *alloc;
    sc_plugin *plugin;
    sc_channel *inner;
    sc_channel_vtab vtab;
} sc_plugin_channel_adapter;

typedef struct sc_plugin_memory_adapter {
    sc_allocator *alloc;
    sc_plugin *plugin;
    sc_memory *inner;
    sc_memory_vtab vtab;
} sc_plugin_memory_adapter;

typedef struct sc_plugin_observer_adapter {
    sc_allocator *alloc;
    sc_plugin *plugin;
    sc_observer *inner;
    sc_observer_vtab vtab;
} sc_plugin_observer_adapter;

typedef struct sc_plugin_peripheral_adapter {
    sc_allocator *alloc;
    sc_plugin *plugin;
    sc_peripheral *inner;
    sc_peripheral_vtab vtab;
} sc_plugin_peripheral_adapter;

static sc_status register_provider_api(const sc_plugin_host_api *api,
                                       const sc_provider_vtab *vtab,
                                       sc_provider_factory factory);
static sc_status register_channel_api(const sc_plugin_host_api *api,
                                      const sc_channel_vtab *vtab,
                                      sc_channel_factory factory);
static sc_status register_tool_api(const sc_plugin_host_api *api,
                                   const sc_tool_vtab *vtab,
                                   sc_tool_factory factory);
static sc_status register_memory_api(const sc_plugin_host_api *api,
                                     const sc_memory_vtab *vtab,
                                     sc_memory_factory factory);
static sc_status register_observer_api(const sc_plugin_host_api *api,
                                       const sc_observer_vtab *vtab,
                                       sc_observer_factory factory);
static sc_status register_peripheral_api(const sc_plugin_host_api *api,
                                         const sc_peripheral_vtab *vtab,
                                         sc_peripheral_factory factory);
static sc_status plugin_load_descriptor_with_handle(sc_plugin_host *host,
                                                    const sc_plugin_descriptor *descriptor,
                                                    const sc_plugin_manifest *manifest,
                                                    void *dynamic_handle,
                                                    sc_plugin **out);
static sc_status plugin_load_wasm_path(sc_plugin_host *host, const sc_plugin_manifest *manifest, sc_plugin **out);
static void plugin_wasm_shutdown(sc_plugin *plugin);
static void plugin_script_shutdown(sc_plugin *plugin, bool call_runtime_shutdown);
static bool descriptor_valid(const sc_plugin_descriptor *descriptor);
static sc_status validate_descriptor_and_manifest(const sc_plugin_host *host,
                                                  const sc_plugin_descriptor *descriptor,
                                                  const sc_plugin_manifest *manifest);
static sc_status copy_json_string(sc_allocator *alloc,
                                  const sc_json_value *object,
                                  sc_str key,
                                  bool required,
                                  sc_string *out);
static sc_status parse_u32_field(const sc_json_value *object, sc_str key, bool required, uint32_t *out);
static sc_status parse_string_flags(const sc_json_value *object,
                                    sc_str key,
                                    uint64_t (*flag_from_name)(sc_str name),
                                    uint64_t *out);
static uint64_t capability_from_name(sc_str name);
static uint64_t permission_from_name(sc_str name);
static sc_status register_permission_check(const sc_plugin *plugin, uint64_t permission, uint64_t capability);
static bool provider_registered(const sc_plugin_host *host, sc_str name);
static bool channel_registered(const sc_plugin_host *host, sc_str name);
static bool tool_registered(const sc_plugin_host *host, sc_str name);
static bool memory_registered(const sc_plugin_host *host, sc_str name);
static bool peripheral_registered(const sc_plugin_host *host, sc_str name);
static const sc_plugin_tool_entry *find_tool_entry(const sc_plugin_host *host, sc_str name);
static const sc_plugin_channel_entry *find_channel_entry(const sc_plugin_host *host, sc_str name);
static const sc_plugin_provider_entry *find_provider_entry(const sc_plugin_host *host, sc_str name);
static const sc_plugin_memory_entry *find_memory_entry(const sc_plugin_host *host, sc_str name);
static const sc_plugin_peripheral_entry *find_peripheral_entry(const sc_plugin_host *host, sc_str name);
static void remove_plugin_registrations(sc_plugin_host *host, const sc_plugin *plugin);
static void remove_plugin_pointer(sc_plugin_host *host, const sc_plugin *plugin);
static bool name_matches(const char *entry_name, sc_str name);
static bool descriptor_field_available(const sc_plugin_descriptor *descriptor, size_t required_size);
static uint64_t descriptor_requested_capabilities(const sc_plugin_descriptor *descriptor);
static uint64_t descriptor_requested_permissions(const sc_plugin_descriptor *descriptor);
static bool plugin_name_loaded(const sc_plugin_host *host, sc_str name);
static sc_status validate_plugin_path(const sc_plugin_host *host, const sc_plugin_manifest *manifest);
static bool path_has_extension(sc_str path, sc_str extension);
static bool path_extension_allowed(const sc_plugin_host *host, sc_str path);
static sc_status script_validate_manifest(const sc_plugin_host *host,
                                          const sc_plugin_manifest *manifest,
                                          uint64_t language_capability,
                                          const char *capability_key);
static sc_status script_parse_u64_field(const sc_json_value *object,
                                        sc_str key,
                                        uint64_t default_value,
                                        uint64_t *out);
static sc_status script_parse_tool_spec(sc_script_plugin_state *state,
                                        sc_str spec_json,
                                        void *invoke_ref,
                                        sc_script_tool_entry **out);
static sc_status script_register_tool_entry(sc_script_tool_entry *tool);
static sc_status script_create_tool_for_vtab(sc_plugin *plugin,
                                             const sc_tool_vtab *vtab,
                                             sc_allocator *alloc,
                                             sc_tool **out);
static sc_status script_tool_factory(sc_allocator *alloc, sc_tool **out);
static sc_status script_tool_spec(void *impl, sc_tool_spec *out);
static sc_status script_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void script_tool_destroy(void *impl);
static void script_tool_entry_destroy(sc_script_tool_entry *tool);
static const char *script_feature_flag(uint64_t language_capability);
static sc_status make_host_api(sc_plugin_registration_context *context, sc_plugin_host_api *out);
static sc_status provider_adapter_generate(void *impl,
                                           const sc_provider_request *request,
                                           sc_allocator *alloc,
                                           sc_provider_response *out);
static sc_status provider_adapter_stream(void *impl,
                                         const sc_provider_request *request,
                                         sc_allocator *alloc,
                                         sc_provider_stream_callback callback,
                                         void *callback_user_data);
static void provider_adapter_destroy(void *impl);
static sc_status tool_adapter_spec(void *impl, sc_tool_spec *out);
static sc_status tool_adapter_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void tool_adapter_destroy(void *impl);
static sc_status channel_adapter_send(void *impl, const sc_channel_message *message);
static sc_status channel_adapter_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out);
static sc_status channel_adapter_health(void *impl, sc_allocator *alloc, sc_channel_health *out);
static sc_status channel_adapter_approval(void *impl,
                                          const sc_channel_approval_request *request,
                                          sc_allocator *alloc,
                                          sc_channel_approval_response *out);
static void channel_adapter_destroy(void *impl);
static sc_status memory_adapter_put(void *impl, const sc_memory_record *record);
static sc_status memory_adapter_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out);
static sc_status memory_adapter_search(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_memory_result *out);
static sc_status memory_adapter_forget(void *impl, sc_str namespace_name, sc_str key);
static sc_status memory_adapter_purge_namespace(void *impl, sc_str namespace_name);
static sc_status memory_adapter_purge_session(void *impl, sc_str namespace_name, sc_str session_id);
static sc_status memory_adapter_export_snapshot(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_string *out);
static sc_status memory_adapter_redact(void *impl, sc_str namespace_name, sc_str key);
static sc_status memory_adapter_export_snapshot_ex(void *impl,
                                                   const sc_memory_query *query,
                                                   const sc_memory_export_options *options,
                                                   sc_allocator *alloc,
                                                   sc_string *out);
static void memory_adapter_destroy(void *impl);
static sc_status observer_adapter_emit(void *impl, const sc_observer_event *event);
static sc_status observer_adapter_flush(void *impl);
static void observer_adapter_destroy(void *impl);
static sc_status peripheral_adapter_health(void *impl, sc_allocator *alloc, sc_peripheral_health *out);
static sc_status peripheral_adapter_command(void *impl,
                                            const sc_peripheral_command *command,
                                            sc_allocator *alloc,
                                            sc_peripheral_result *out);
static sc_status peripheral_adapter_describe_context(void *impl, sc_allocator *alloc, sc_peripheral_context *out);
static void peripheral_adapter_destroy(void *impl);
#ifdef SC_HAVE_WAMR
static sc_status wasm_runtime_acquire(void);
static void wasm_runtime_release(void);
static sc_status wasm_read_module(sc_allocator *alloc, sc_str path, uint8_t **bytes_out, size_t *size_out);
static sc_status wasm_validate_manifest(const sc_plugin_host *host, const sc_plugin_manifest *manifest);
static sc_status wasm_parse_tool_spec(sc_wasm_plugin_state *state,
                                      sc_str spec_json,
                                      sc_str invoke_name,
                                      sc_wasm_tool_entry **out);
static void wasm_tool_entry_destroy(sc_wasm_tool_entry *tool);
static void wasm_state_destroy(sc_wasm_plugin_state *state);
static sc_status wasm_register_tool_entry(sc_wasm_tool_entry *tool);
static sc_status wasm_create_tool_for_vtab(sc_plugin *plugin, const sc_tool_vtab *vtab, sc_allocator *alloc, sc_tool **out);
static sc_status wasm_tool_factory(sc_allocator *alloc, sc_tool **out);
static sc_status wasm_tool_spec(void *impl, sc_tool_spec *out);
static sc_status wasm_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void wasm_tool_destroy(void *impl);
static int32_t wasm_host_register_tool(wasm_exec_env_t exec_env,
                                       uint32_t spec_ptr,
                                       uint32_t spec_len,
                                       uint32_t invoke_ptr,
                                       uint32_t invoke_len);
static void wasm_host_set_tool_result(wasm_exec_env_t exec_env, int32_t success, uint32_t output_ptr, uint32_t output_len);
static void wasm_host_set_status(wasm_exec_env_t exec_env, int32_t code, uint32_t key_ptr, uint32_t key_len);
static sc_wasm_plugin_state *wasm_state_from_exec_env(wasm_exec_env_t exec_env);
static bool wasm_copy_app_str(wasm_module_inst_t module_inst,
                              uint32_t ptr,
                              uint32_t len,
                              sc_allocator *alloc,
                              sc_string *out);
static sc_status wasm_status_from_code(sc_status_code code, const char *key);
#endif

static const sc_tool_vtab plugin_tool_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "plugin-tool-adapter",
    .display_name = "Plugin tool adapter",
    .feature_flag = "SC_ENABLE_PLUGINS",
    .capabilities = SC_CONTRACT_CAP_SECURE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = tool_adapter_spec,
    .invoke = tool_adapter_invoke,
    .destroy = tool_adapter_destroy,
};

void sc_plugin_manifest_init(sc_plugin_manifest *manifest, sc_allocator *alloc)
{
    if (manifest == nullptr) {
        return;
    }
    *manifest = (sc_plugin_manifest){
        .struct_size = sizeof(*manifest),
        .alloc = alloc == nullptr ? sc_allocator_heap() : alloc,
        .abi_major = SC_ABI_VERSION_MAJOR,
        .abi_minor = SC_ABI_VERSION_MINOR,
    };
}

sc_status sc_plugin_manifest_parse(sc_allocator *alloc, sc_str json_text, sc_plugin_manifest *out)
{
    sc_json_value *root = nullptr;
    sc_json_parse_error error = {0};
    sc_plugin_manifest tmp = {0};
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_manifest.invalid_argument");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_plugin_manifest_init(&tmp, alloc);
    status = sc_json_parse(alloc, json_text, &root, &error);
    if (sc_status_is_ok(status) && sc_json_type_of(root) != SC_JSON_OBJECT) {
        status = sc_status_parse("sc.plugin_manifest.root_not_object");
    }
    if (sc_status_is_ok(status)) {
        status = copy_json_string(alloc, root, sc_str_from_cstr("name"), true, &tmp.name);
    }
    if (sc_status_is_ok(status)) {
        status = copy_json_string(alloc, root, sc_str_from_cstr("version"), true, &tmp.version);
    }
    if (sc_status_is_ok(status)) {
        status = parse_u32_field(root, sc_str_from_cstr("abi_major"), true, &tmp.abi_major);
    }
    if (sc_status_is_ok(status)) {
        status = parse_u32_field(root, sc_str_from_cstr("abi_minor"), false, &tmp.abi_minor);
    }
    if (sc_status_is_ok(status)) {
        status = parse_string_flags(root, sc_str_from_cstr("capabilities"), capability_from_name, &tmp.capabilities);
    }
    if (sc_status_is_ok(status)) {
        status = parse_string_flags(root,
                                    sc_str_from_cstr("requested_permissions"),
                                    permission_from_name,
                                    &tmp.requested_permissions);
    }
    if (sc_status_is_ok(status)) {
        status = copy_json_string(alloc, root, sc_str_from_cstr("entry_artifact"), true, &tmp.artifact_path);
    }
    if (sc_status_is_ok(status)) {
        status = copy_json_string(alloc,
                                  root,
                                  sc_str_from_cstr("signature_metadata"),
                                  false,
                                  &tmp.signature_metadata);
    }

    sc_json_destroy(root);
    if (!sc_status_is_ok(status)) {
        sc_plugin_manifest_clear(&tmp);
        return status;
    }
    *out = tmp;
    return sc_status_ok();
}

sc_status sc_plugin_manifest_copy(sc_allocator *alloc,
                                  const sc_plugin_manifest *manifest,
                                  sc_plugin_manifest *out)
{
    sc_plugin_manifest tmp = {0};
    sc_status status;

    if (manifest == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_manifest.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    sc_plugin_manifest_init(&tmp, alloc);
    tmp.abi_major = manifest->abi_major;
    tmp.abi_minor = manifest->abi_minor;
    tmp.capabilities = manifest->capabilities;
    tmp.requested_permissions = manifest->requested_permissions;
    status = sc_string_from_str(alloc, sc_string_as_str(&manifest->name), &tmp.name);
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&manifest->version), &tmp.version);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&manifest->artifact_path), &tmp.artifact_path);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(alloc, sc_string_as_str(&manifest->signature_metadata), &tmp.signature_metadata);
    }
    if (!sc_status_is_ok(status)) {
        sc_plugin_manifest_clear(&tmp);
        return status;
    }
    *out = tmp;
    return sc_status_ok();
}

void sc_plugin_manifest_clear(sc_plugin_manifest *manifest)
{
    if (manifest == nullptr) {
        return;
    }
    sc_string_clear(&manifest->name);
    sc_string_clear(&manifest->version);
    sc_string_clear(&manifest->artifact_path);
    sc_string_clear(&manifest->signature_metadata);
    *manifest = (sc_plugin_manifest){0};
}

sc_status sc_plugin_host_new(sc_allocator *alloc,
                             const sc_plugin_host_options *options,
                             sc_plugin_host **out)
{
    sc_plugin_host *host = nullptr;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host.invalid_argument");
    }

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    host = sc_alloc(alloc, sizeof(*host), _Alignof(sc_plugin_host));
    if (host == nullptr) {
        return sc_status_no_memory();
    }
    *host = (sc_plugin_host){.alloc = alloc};
    if (options != nullptr) {
        host->providers = options->providers;
        host->channels = options->channels;
        host->tools = options->tools;
        host->memory_backends = options->memory_backends;
        host->observers = options->observers;
        host->peripherals = options->peripherals;
        host->allowed_permissions = options->allowed_permissions;
        host->allow_memory_backends = options->allow_memory_backends;
        host->plugin_roots = options->plugin_roots;
        host->plugin_root_count = options->plugin_root_count;
        host->allowed_extensions = options->allowed_extensions;
        host->allowed_extension_count = options->allowed_extension_count;
        host->dynamic_modules_enabled = true;
        host->wasm_enabled = options->struct_size > offsetof(sc_plugin_host_options, wasm_enabled)
                                 ? options->wasm_enabled
                                 : false;
        host->python_enabled = options->struct_size > offsetof(sc_plugin_host_options, python_enabled)
                                   ? options->python_enabled
                                   : false;
    } else {
        host->dynamic_modules_enabled = true;
    }
    sc_vec_init(&host->provider_entries, alloc, sizeof(sc_plugin_provider_entry));
    sc_vec_init(&host->channel_entries, alloc, sizeof(sc_plugin_channel_entry));
    sc_vec_init(&host->tool_entries, alloc, sizeof(sc_plugin_tool_entry));
    sc_vec_init(&host->memory_entries, alloc, sizeof(sc_plugin_memory_entry));
    sc_vec_init(&host->peripheral_entries, alloc, sizeof(sc_plugin_peripheral_entry));
    sc_ptr_vec_init(&host->loaded_plugins, alloc);
    *out = host;
    return sc_status_ok();
}

size_t sc_plugin_host_provider_count(const sc_plugin_host *host)
{
    return host == nullptr ? 0 : host->provider_entries.len;
}

size_t sc_plugin_host_channel_count(const sc_plugin_host *host)
{
    return host == nullptr ? 0 : host->channel_entries.len;
}

size_t sc_plugin_host_tool_count(const sc_plugin_host *host)
{
    return host == nullptr ? 0 : host->tool_entries.len;
}

size_t sc_plugin_host_memory_count(const sc_plugin_host *host)
{
    return host == nullptr ? 0 : host->memory_entries.len;
}

size_t sc_plugin_host_peripheral_count(const sc_plugin_host *host)
{
    return host == nullptr ? 0 : host->peripheral_entries.len;
}

sc_status sc_plugin_host_create_provider(const sc_plugin_host *host,
                                         sc_str name,
                                         sc_allocator *alloc,
                                         sc_provider **out)
{
    const sc_plugin_provider_entry *entry = nullptr;
    sc_plugin_provider_adapter *adapter = nullptr;
    sc_provider *inner = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host.invalid_argument");
    }
    entry = find_provider_entry(host, name);
    if (entry == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host.provider_not_found");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = entry->factory(alloc, &inner);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    adapter = sc_alloc(alloc, sizeof(*adapter), _Alignof(sc_plugin_provider_adapter));
    if (adapter == nullptr) {
        sc_provider_destroy(inner);
        return sc_status_no_memory();
    }
    *adapter = (sc_plugin_provider_adapter){.alloc = alloc, .plugin = entry->plugin, .inner = inner, .vtab = *entry->vtab};
    adapter->vtab.generate = provider_adapter_generate;
    adapter->vtab.stream = provider_adapter_stream;
    adapter->vtab.destroy = provider_adapter_destroy;
    status = sc_plugin_acquire(entry->plugin);
    if (sc_status_is_ok(status)) {
        status = sc_provider_new(alloc, &adapter->vtab, adapter, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_plugin_release(entry->plugin);
        sc_provider_destroy(inner);
        sc_free(alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_provider_adapter));
    }
    return status;
}

sc_status sc_plugin_host_create_channel(const sc_plugin_host *host,
                                        sc_str name,
                                        sc_allocator *alloc,
                                        sc_channel **out)
{
    const sc_plugin_channel_entry *entry = nullptr;
    sc_plugin_channel_adapter *adapter = nullptr;
    sc_channel *inner = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host.invalid_argument");
    }
    entry = find_channel_entry(host, name);
    if (entry == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host.channel_not_found");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = entry->factory(alloc, &inner);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    adapter = sc_alloc(alloc, sizeof(*adapter), _Alignof(sc_plugin_channel_adapter));
    if (adapter == nullptr) {
        sc_channel_destroy(inner);
        return sc_status_no_memory();
    }
    *adapter = (sc_plugin_channel_adapter){.alloc = alloc, .plugin = entry->plugin, .inner = inner, .vtab = *entry->vtab};
    adapter->vtab.send = channel_adapter_send;
    adapter->vtab.listen = channel_adapter_listen;
    adapter->vtab.health = channel_adapter_health;
    adapter->vtab.request_approval = channel_adapter_approval;
    adapter->vtab.destroy = channel_adapter_destroy;
    if (adapter->vtab.description_key == nullptr) {
        adapter->vtab.description_key = "channel.plugin.description";
    }
    if (adapter->vtab.config_schema_ref == nullptr) {
        adapter->vtab.config_schema_ref = "sc.schema.channels.plugin";
    }
    status = sc_plugin_acquire(entry->plugin);
    if (sc_status_is_ok(status)) {
        status = sc_channel_new(alloc, &adapter->vtab, adapter, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_plugin_release(entry->plugin);
        sc_channel_destroy(inner);
        sc_free(alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_channel_adapter));
    }
    return status;
}

sc_status sc_plugin_host_create_tool(const sc_plugin_host *host,
                                     sc_str name,
                                     const sc_tool_context *context,
                                     sc_allocator *alloc,
                                     sc_tool **out)
{
    const sc_plugin_tool_entry *entry = nullptr;
    sc_plugin_tool_adapter *adapter = nullptr;
    sc_tool *inner = nullptr;
    sc_status status;

    if (out == nullptr || context == nullptr || context->policy == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host.invalid_argument");
    }
    entry = find_tool_entry(host, name);
    if (entry == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host.tool_not_found");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
#ifdef SC_HAVE_WAMR
    if (entry->plugin != nullptr && entry->plugin->wasm_state != nullptr) {
        status = wasm_create_tool_for_vtab(entry->plugin, entry->vtab, alloc, &inner);
    } else
#endif
    if (entry->plugin != nullptr && entry->plugin->script_state != nullptr) {
        status = script_create_tool_for_vtab(entry->plugin, entry->vtab, alloc, &inner);
    } else {
        status = entry->factory(alloc, &inner);
    }
    if (!sc_status_is_ok(status)) {
        return status;
    }
    adapter = sc_alloc(alloc, sizeof(*adapter), _Alignof(sc_plugin_tool_adapter));
    if (adapter == nullptr) {
        sc_tool_destroy(inner);
        return sc_status_no_memory();
    }
    *adapter = (sc_plugin_tool_adapter){.alloc = alloc, .plugin = entry->plugin, .inner = inner, .context = *context};
    status = sc_plugin_acquire(entry->plugin);
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, &plugin_tool_vtab, adapter, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_plugin_release(entry->plugin);
        sc_tool_destroy(inner);
        sc_free(alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_tool_adapter));
    }
    return status;
}

sc_status sc_plugin_host_create_memory(const sc_plugin_host *host,
                                       sc_str name,
                                       sc_allocator *alloc,
                                       sc_memory **out)
{
    const sc_plugin_memory_entry *entry = nullptr;
    sc_plugin_memory_adapter *adapter = nullptr;
    sc_memory *inner = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host.invalid_argument");
    }
    entry = find_memory_entry(host, name);
    if (entry == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host.memory_not_found");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = entry->factory(alloc, &inner);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    adapter = sc_alloc(alloc, sizeof(*adapter), _Alignof(sc_plugin_memory_adapter));
    if (adapter == nullptr) {
        sc_memory_destroy(inner);
        return sc_status_no_memory();
    }
    *adapter = (sc_plugin_memory_adapter){.alloc = alloc, .plugin = entry->plugin, .inner = inner, .vtab = *entry->vtab};
    adapter->vtab.put = memory_adapter_put;
    adapter->vtab.get = memory_adapter_get;
    adapter->vtab.search = memory_adapter_search;
    adapter->vtab.forget = memory_adapter_forget;
    adapter->vtab.purge_namespace = memory_adapter_purge_namespace;
    adapter->vtab.purge_session = memory_adapter_purge_session;
    adapter->vtab.export_snapshot = memory_adapter_export_snapshot;
    adapter->vtab.redact = memory_adapter_redact;
    adapter->vtab.export_snapshot_ex = memory_adapter_export_snapshot_ex;
    adapter->vtab.destroy = memory_adapter_destroy;
    status = sc_plugin_acquire(entry->plugin);
    if (sc_status_is_ok(status)) {
        status = sc_memory_new(alloc, &adapter->vtab, adapter, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_plugin_release(entry->plugin);
        sc_memory_destroy(inner);
        sc_free(alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_memory_adapter));
    }
    return status;
}

sc_status sc_plugin_host_create_peripheral(const sc_plugin_host *host,
                                           sc_str name,
                                           sc_allocator *alloc,
                                           sc_peripheral **out)
{
    const sc_plugin_peripheral_entry *entry = nullptr;
    sc_plugin_peripheral_adapter *adapter = nullptr;
    sc_peripheral *inner = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host.invalid_argument");
    }
    entry = find_peripheral_entry(host, name);
    if (entry == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host.peripheral_not_found");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = entry->factory(alloc, &inner);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    adapter = sc_alloc(alloc, sizeof(*adapter), _Alignof(sc_plugin_peripheral_adapter));
    if (adapter == nullptr) {
        sc_peripheral_destroy(inner);
        return sc_status_no_memory();
    }
    *adapter = (sc_plugin_peripheral_adapter){.alloc = alloc, .plugin = entry->plugin, .inner = inner};
    (void)memcpy(&adapter->vtab,
                 entry->vtab,
                 entry->vtab->struct_size < sizeof(adapter->vtab) ? entry->vtab->struct_size : sizeof(adapter->vtab));
    adapter->vtab.health = peripheral_adapter_health;
    adapter->vtab.command = peripheral_adapter_command;
    adapter->vtab.describe_context = peripheral_adapter_describe_context;
    adapter->vtab.destroy = peripheral_adapter_destroy;
    status = sc_plugin_acquire(entry->plugin);
    if (sc_status_is_ok(status)) {
        status = sc_peripheral_new(alloc, &adapter->vtab, adapter, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_plugin_release(entry->plugin);
        sc_peripheral_destroy(inner);
        sc_free(alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_peripheral_adapter));
    }
    return status;
}

void sc_plugin_host_destroy(sc_plugin_host *host)
{
    if (host == nullptr) {
        return;
    }
    for (size_t i = 0; i < host->loaded_plugins.inner.len; i += 1) {
        const sc_plugin *plugin = sc_ptr_vec_at(&host->loaded_plugins, i);
        if (plugin != nullptr && plugin->active_refs > 0) {
            /* Adapters borrow plugin code and state; the host must outlive every adapter. */
            return;
        }
    }
    while (host->loaded_plugins.inner.len > 0) {
        sc_plugin *plugin = sc_ptr_vec_at(&host->loaded_plugins, 0);
        if (plugin == nullptr) {
            break;
        }
        (void)sc_plugin_unload(plugin);
    }
    sc_vec_clear(&host->provider_entries);
    sc_vec_clear(&host->channel_entries);
    sc_vec_clear(&host->tool_entries);
    sc_vec_clear(&host->memory_entries);
    sc_vec_clear(&host->peripheral_entries);
    sc_ptr_vec_clear(&host->loaded_plugins);
    sc_free(host->alloc, host, sizeof(*host), _Alignof(sc_plugin_host));
}

sc_status sc_plugin_load_descriptor(sc_plugin_host *host,
                                    const sc_plugin_descriptor *descriptor,
                                    const sc_plugin_manifest *manifest,
                                    sc_plugin **out)
{
    return plugin_load_descriptor_with_handle(host, descriptor, manifest, nullptr, out);
}

sc_status sc_plugin_load_path(sc_plugin_host *host, const sc_plugin_manifest *manifest, sc_plugin **out)
{
    void *handle = nullptr;
    void *symbol = nullptr;
    sc_plugin_descriptor_getter getter = nullptr;
    const sc_plugin_descriptor *descriptor = nullptr;
    sc_status status;

    if (host == nullptr || manifest == nullptr || out == nullptr || sc_string_is_empty(&manifest->artifact_path)) {
        return sc_status_invalid_argument("sc.plugin_loader.invalid_argument");
    }
    status = validate_plugin_path(host, manifest);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if ((manifest->requested_permissions & ~host->allowed_permissions) != 0) {
        return sc_status_security_denied("sc.plugin_loader.permission_denied");
    }
    if ((manifest->capabilities & SC_PLUGIN_CAP_WASM) != 0 ||
        path_has_extension(sc_string_as_str(&manifest->artifact_path), sc_str_from_cstr(".wasm"))) {
        return host->wasm_enabled ? plugin_load_wasm_path(host, manifest, out)
                                  : sc_status_security_denied("sc.plugin_loader.wasm_disabled");
    }
    if ((manifest->capabilities & SC_PLUGIN_CAP_PYTHON) != 0 ||
        path_has_extension(sc_string_as_str(&manifest->artifact_path), sc_str_from_cstr(".py"))) {
        return host->python_enabled ? sc_plugin_load_python_path(host, manifest, out)
                                    : sc_status_security_denied("sc.plugin_loader.python_disabled");
    }
    if (!host->dynamic_modules_enabled) {
        return sc_status_security_denied("sc.plugin_loader.dynamic_disabled");
    }
    handle = dlopen(manifest->artifact_path.ptr, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr) {
        return sc_status_io("sc.plugin_loader.open_failed");
    }
    symbol = dlsym(handle, SC_PLUGIN_DESCRIPTOR_SYMBOL);
    if (symbol == nullptr) {
        (void)dlclose(handle);
        return sc_status_invalid_argument("sc.plugin_loader.missing_descriptor");
    }
    if (sizeof(getter) != sizeof(symbol)) {
        (void)dlclose(handle);
        return sc_status_unsupported("sc.plugin_loader.function_pointer_size");
    }
    (void)memcpy(&getter, &symbol, sizeof(getter));
    descriptor = getter();
    status = plugin_load_descriptor_with_handle(host, descriptor, manifest, handle, out);
    if (!sc_status_is_ok(status)) {
        (void)dlclose(handle);
    }
    return status;
}

const sc_plugin_manifest *sc_plugin_manifest_of(const sc_plugin *plugin)
{
    return plugin == nullptr ? nullptr : &plugin->manifest;
}

sc_status sc_plugin_acquire(sc_plugin *plugin)
{
    if (plugin == nullptr || !plugin->initialized) {
        return sc_status_invalid_argument("sc.plugin.invalid_argument");
    }
    plugin->active_refs += 1;
    return sc_status_ok();
}

void sc_plugin_release(sc_plugin *plugin)
{
    if (plugin == nullptr || plugin->active_refs == 0) {
        return;
    }
    plugin->active_refs -= 1;
}

size_t sc_plugin_active_references(const sc_plugin *plugin)
{
    return plugin == nullptr ? 0 : plugin->active_refs;
}

sc_status sc_plugin_unload(sc_plugin *plugin)
{
    sc_plugin_host *host = nullptr;

    if (plugin == nullptr) {
        return sc_status_invalid_argument("sc.plugin.invalid_argument");
    }
    if (plugin->active_refs > 0) {
        return sc_status_cancelled("sc.plugin.active_references");
    }
    host = plugin->host;
#ifdef SC_HAVE_WAMR
    if (plugin->wasm_state != nullptr) {
        plugin_wasm_shutdown(plugin);
    } else
#endif
    if (plugin->script_state != nullptr) {
        plugin_script_shutdown(plugin, true);
    } else if (plugin->initialized && plugin->descriptor != nullptr && plugin->descriptor->shutdown != nullptr) {
        plugin->descriptor->shutdown(plugin->state);
    }
    plugin->initialized = false;
    if (host != nullptr) {
        remove_plugin_registrations(host, plugin);
        remove_plugin_pointer(host, plugin);
    }
    if (plugin->dynamic_handle != nullptr) {
        (void)dlclose(plugin->dynamic_handle);
    }
    sc_plugin_manifest_clear(&plugin->manifest);
    sc_free(plugin->alloc, plugin, sizeof(*plugin), _Alignof(sc_plugin));
    return sc_status_ok();
}

void sc_plugin_release_memory(sc_plugin *plugin, void *ptr, size_t len)
{
    if (plugin == nullptr || ptr == nullptr || plugin->descriptor == nullptr) {
        return;
    }
    if (descriptor_field_available(plugin->descriptor,
                                   offsetof(sc_plugin_descriptor, release_memory) +
                                       sizeof(plugin->descriptor->release_memory)) &&
        plugin->descriptor->release_memory != nullptr) {
        plugin->descriptor->release_memory(plugin->state, ptr, len);
    }
}

static sc_status register_provider_api(const sc_plugin_host_api *api,
                                       const sc_provider_vtab *vtab,
                                       sc_provider_factory factory)
{
    sc_plugin_registration_context *context = api == nullptr ? nullptr : api->userdata;
    sc_plugin_provider_entry entry = {0};
    sc_status status;

    if (context == nullptr || context->host == nullptr || context->plugin == nullptr ||
        factory == nullptr || !sc_provider_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.plugin.provider.invalid_registration");
    }
    status = register_permission_check(context->plugin,
                                       SC_PLUGIN_PERMISSION_PROVIDER,
                                       SC_PLUGIN_CAP_PROVIDER);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (provider_registered(context->host, sc_str_from_cstr(vtab->name))) {
        return sc_status_invalid_argument("sc.plugin.provider.duplicate");
    }
    entry = (sc_plugin_provider_entry){.plugin = context->plugin, .vtab = vtab, .factory = factory};
    return sc_vec_push(&context->host->provider_entries, &entry);
}

static sc_status register_channel_api(const sc_plugin_host_api *api,
                                      const sc_channel_vtab *vtab,
                                      sc_channel_factory factory)
{
    sc_plugin_registration_context *context = api == nullptr ? nullptr : api->userdata;
    sc_plugin_channel_entry entry = {0};
    sc_status status;

    if (context == nullptr || context->host == nullptr || context->plugin == nullptr ||
        factory == nullptr || !sc_channel_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.plugin.channel.invalid_registration");
    }
    status = register_permission_check(context->plugin,
                                       SC_PLUGIN_PERMISSION_CHANNEL,
                                       SC_PLUGIN_CAP_CHANNEL);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (channel_registered(context->host, sc_str_from_cstr(vtab->name))) {
        return sc_status_invalid_argument("sc.plugin.channel.duplicate");
    }
    entry = (sc_plugin_channel_entry){.plugin = context->plugin, .vtab = vtab, .factory = factory};
    return sc_vec_push(&context->host->channel_entries, &entry);
}

static sc_status register_tool_api(const sc_plugin_host_api *api,
                                   const sc_tool_vtab *vtab,
                                   sc_tool_factory factory)
{
    sc_plugin_registration_context *context = api == nullptr ? nullptr : api->userdata;
    sc_plugin_tool_entry entry = {0};
    sc_status status;

    if (context == nullptr || context->host == nullptr || context->plugin == nullptr ||
        factory == nullptr || !sc_tool_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.plugin.tool.invalid_registration");
    }
    status = register_permission_check(context->plugin, SC_PLUGIN_PERMISSION_TOOL, SC_PLUGIN_CAP_TOOL);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (tool_registered(context->host, sc_str_from_cstr(vtab->name))) {
        return sc_status_invalid_argument("sc.plugin.tool.duplicate");
    }
    entry = (sc_plugin_tool_entry){.plugin = context->plugin, .vtab = vtab, .factory = factory};
    return sc_vec_push(&context->host->tool_entries, &entry);
}

static sc_status register_memory_api(const sc_plugin_host_api *api,
                                     const sc_memory_vtab *vtab,
                                     sc_memory_factory factory)
{
    sc_plugin_registration_context *context = api == nullptr ? nullptr : api->userdata;
    sc_plugin_memory_entry entry = {0};
    sc_status status;

    if (context == nullptr || context->host == nullptr || context->plugin == nullptr ||
        factory == nullptr || !sc_memory_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.plugin.memory.invalid_registration");
    }
    if (!context->host->allow_memory_backends) {
        return sc_status_security_denied("sc.plugin.memory.not_allowed");
    }
    status = register_permission_check(context->plugin, SC_PLUGIN_PERMISSION_MEMORY, SC_PLUGIN_CAP_MEMORY);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (memory_registered(context->host, sc_str_from_cstr(vtab->name))) {
        return sc_status_invalid_argument("sc.plugin.memory.duplicate");
    }
    entry = (sc_plugin_memory_entry){.plugin = context->plugin, .vtab = vtab, .factory = factory};
    return sc_vec_push(&context->host->memory_entries, &entry);
}

#ifndef SC_HAVE_WAMR
static sc_status plugin_load_wasm_path(sc_plugin_host *host, const sc_plugin_manifest *manifest, sc_plugin **out)
{
    (void)host;
    (void)manifest;
    (void)out;
    return sc_status_unsupported("sc.plugin_loader.wasm_unavailable");
}

[[maybe_unused]] static void plugin_wasm_shutdown(sc_plugin *plugin)
{
    (void)plugin;
}
#else
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wpedantic"
#endif
/* WAMR's public NativeSymbol ABI stores native code pointers as void*. */
static NativeSymbol wasm_native_symbols[] = {
    {"sc_host_register_tool", (void *)wasm_host_register_tool, "(iiii)i", nullptr},
    {"sc_host_set_tool_result", (void *)wasm_host_set_tool_result, "(iii)", nullptr},
    {"sc_host_set_status", (void *)wasm_host_set_status, "(iii)", nullptr},
};
#if defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic pop
#endif
static size_t wasm_runtime_ref_count;

static sc_status wasm_runtime_acquire(void)
{
    RuntimeInitArgs init_args = {0};

    if (wasm_runtime_ref_count > 0) {
        wasm_runtime_ref_count += 1;
        return sc_status_ok();
    }

    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    init_args.native_module_name = "env";
    init_args.native_symbols = wasm_native_symbols;
    init_args.n_native_symbols = (uint32_t)SC_ARRAY_LEN(wasm_native_symbols);
    init_args.running_mode = Mode_Interp;
    if (!wasm_runtime_full_init(&init_args)) {
        return sc_status_unsupported("sc.plugin_loader.wamr_init_failed");
    }
    wasm_runtime_set_log_level(WASM_LOG_LEVEL_WARNING);
    wasm_runtime_ref_count = 1;
    return sc_status_ok();
}

static void wasm_runtime_release(void)
{
    if (wasm_runtime_ref_count == 0) {
        return;
    }
    wasm_runtime_ref_count -= 1;
    if (wasm_runtime_ref_count == 0) {
        wasm_runtime_destroy();
    }
}

static sc_status wasm_read_module(sc_allocator *alloc, sc_str path, uint8_t **bytes_out, size_t *size_out)
{
    enum { SC_WASM_MAX_MODULE_BYTES = 16 * 1'024 * 1'024 };
    FILE *file = nullptr;
    long size = 0;
    uint8_t *bytes = nullptr;
    sc_status status = sc_status_ok();

    if (path.ptr == nullptr || bytes_out == nullptr || size_out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.wasm_invalid_argument");
    }
    file = fopen(path.ptr, "rb");
    if (file == nullptr) {
        status = sc_status_io("sc.plugin_loader.wasm_open_failed");
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        status = sc_status_io("sc.plugin_loader.wasm_seek_failed");
        goto cleanup;
    }
    size = ftell(file);
    if (size <= 0 || size > SC_WASM_MAX_MODULE_BYTES) {
        status = sc_status_invalid_argument("sc.plugin_loader.wasm_size_invalid");
        goto cleanup;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        status = sc_status_io("sc.plugin_loader.wasm_seek_failed");
        goto cleanup;
    }
    bytes = sc_alloc(alloc, (size_t)size, _Alignof(uint8_t));
    if (bytes == nullptr) {
        status = sc_status_no_memory();
        goto cleanup;
    }
    if (fread(bytes, 1, (size_t)size, file) != (size_t)size) {
        status = sc_status_io("sc.plugin_loader.wasm_read_failed");
        goto cleanup;
    }
    (void)fclose(file);
    file = nullptr;
    *bytes_out = bytes;
    *size_out = (size_t)size;
    bytes = nullptr;

cleanup:
    if (file != nullptr) {
        (void)fclose(file);
    }
    if (bytes != nullptr) {
        sc_free(alloc, bytes, (size_t)size, _Alignof(uint8_t));
    }
    return status;
}

static sc_status wasm_validate_manifest(const sc_plugin_host *host, const sc_plugin_manifest *manifest)
{
    uint64_t denied_permissions = 0;

    if (host == nullptr || manifest == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.invalid_argument");
    }
    if ((manifest->capabilities & SC_PLUGIN_CAP_WASM) == 0) {
        return sc_status_invalid_argument("sc.plugin_loader.wasm_capability_missing");
    }
    if ((manifest->capabilities & ~((uint64_t)SC_PLUGIN_CAP_WASM | (uint64_t)SC_PLUGIN_CAP_TOOL)) != 0) {
        return sc_status_unsupported("sc.plugin_loader.wasm_capability_unsupported");
    }
    if ((manifest->capabilities & SC_PLUGIN_CAP_TOOL) == 0) {
        return sc_status_unsupported("sc.plugin_loader.wasm_tool_required");
    }
    if (manifest->abi_major != SC_ABI_VERSION_MAJOR) {
        return sc_status_invalid_argument("sc.plugin_loader.abi_mismatch");
    }
    if (sc_string_is_empty(&manifest->name) || sc_string_is_empty(&manifest->version)) {
        return sc_status_invalid_argument("sc.plugin_loader.manifest_mismatch");
    }
    if ((manifest->requested_permissions & SC_PLUGIN_PERMISSION_TOOL) == 0) {
        return sc_status_security_denied("sc.plugin.registration.permission_denied");
    }
    denied_permissions = manifest->requested_permissions & ~host->allowed_permissions;
    if (denied_permissions != 0) {
        return sc_status_security_denied("sc.plugin_loader.permission_denied");
    }
    if (plugin_name_loaded(host, sc_string_as_str(&manifest->name))) {
        return sc_status_invalid_argument("sc.plugin_loader.duplicate_plugin");
    }
    return sc_status_ok();
}

static sc_status plugin_load_wasm_path(sc_plugin_host *host, const sc_plugin_manifest *manifest, sc_plugin **out)
{
    sc_plugin *plugin = nullptr;
    sc_wasm_plugin_state *state = nullptr;
    char error_buf[256] = {0};
    uint32_t argv[2] = {0};
    wasm_function_inst_t abi_func = nullptr;
    wasm_function_inst_t init_func = nullptr;
    sc_status status;

    if (host == nullptr || manifest == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.invalid_argument");
    }
    *out = nullptr;
    status = wasm_validate_manifest(host, manifest);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    plugin = sc_alloc(host->alloc, sizeof(*plugin), _Alignof(sc_plugin));
    state = sc_alloc(host->alloc, sizeof(*state), _Alignof(sc_wasm_plugin_state));
    if (plugin == nullptr || state == nullptr) {
        if (plugin != nullptr) {
            sc_free(host->alloc, plugin, sizeof(*plugin), _Alignof(sc_plugin));
        }
        if (state != nullptr) {
            sc_free(host->alloc, state, sizeof(*state), _Alignof(sc_wasm_plugin_state));
        }
        return sc_status_no_memory();
    }
    *plugin = (sc_plugin){.alloc = host->alloc, .host = host, .wasm_state = state};
    *state = (sc_wasm_plugin_state){.alloc = host->alloc, .plugin = plugin};
    sc_ptr_vec_init(&state->tools, host->alloc);

    status = sc_plugin_manifest_copy(host->alloc, manifest, &plugin->manifest);
    if (sc_status_is_ok(status)) {
        status = wasm_runtime_acquire();
    }
    if (sc_status_is_ok(status)) {
        status = wasm_read_module(host->alloc, sc_string_as_str(&plugin->manifest.artifact_path), &state->module_bytes, &state->module_size);
    }
    if (sc_status_is_ok(status)) {
        LoadArgs load_args = {.wasm_binary_freeable = true};
        state->module = wasm_runtime_load_ex(state->module_bytes,
                                             (uint32_t)state->module_size,
                                             &load_args,
                                             error_buf,
                                             (uint32_t)sizeof(error_buf));
        if (state->module == nullptr) {
            status = sc_status_parse("sc.plugin_loader.wasm_load_failed");
        }
    }
    if (sc_status_is_ok(status)) {
        wasm_runtime_set_wasi_args_ex(state->module, nullptr, 0, nullptr, 0, nullptr, 0, nullptr, 0, -1, -1, -1);
        state->module_inst = wasm_runtime_instantiate(state->module, 64 * 1'024, 64 * 1'024, error_buf, (uint32_t)sizeof(error_buf));
        if (state->module_inst == nullptr) {
            status = sc_status_invalid_argument("sc.plugin_loader.wasm_instantiate_failed");
        }
    }
    if (sc_status_is_ok(status)) {
        wasm_runtime_set_custom_data(state->module_inst, state);
        state->exec_env = wasm_runtime_create_exec_env(state->module_inst, 64 * 1'024);
        if (state->exec_env == nullptr) {
            status = sc_status_no_memory();
        }
    }
    if (sc_status_is_ok(status)) {
        abi_func = wasm_runtime_lookup_function(state->module_inst, "sc_plugin_guest_abi_version");
        init_func = wasm_runtime_lookup_function(state->module_inst, "sc_plugin_init");
        if (abi_func == nullptr || init_func == nullptr) {
            status = sc_status_invalid_argument("sc.plugin_loader.wasm_missing_export");
        }
    }
    if (sc_status_is_ok(status) && !wasm_runtime_call_wasm(state->exec_env, abi_func, 0, argv)) {
        status = sc_status_invalid_argument("sc.plugin_loader.wasm_abi_failed");
    }
    if (sc_status_is_ok(status) && argv[0] != 1) {
        status = sc_status_unsupported("sc.plugin_loader.wasm_abi_unsupported");
    }
    if (sc_status_is_ok(status) && !wasm_runtime_call_wasm(state->exec_env, init_func, 0, nullptr)) {
        status = sc_status_invalid_argument("sc.plugin_loader.wasm_init_failed");
    }
    if (sc_status_is_ok(status) && state->tools.inner.len == 0) {
        status = sc_status_invalid_argument("sc.plugin_loader.wasm_no_tools_registered");
    }
    if (sc_status_is_ok(status)) {
        plugin->initialized = true;
        status = sc_ptr_vec_push(&host->loaded_plugins, plugin);
    }
    if (!sc_status_is_ok(status)) {
        plugin_wasm_shutdown(plugin);
        sc_plugin_manifest_clear(&plugin->manifest);
        sc_free(host->alloc, plugin, sizeof(*plugin), _Alignof(sc_plugin));
        return status;
    }
    *out = plugin;
    return sc_status_ok();
}

static void plugin_wasm_shutdown(sc_plugin *plugin)
{
    if (plugin == nullptr || plugin->wasm_state == nullptr) {
        return;
    }
    if (plugin->wasm_state->module_inst != nullptr && plugin->wasm_state->exec_env != nullptr) {
        wasm_function_inst_t shutdown_func = wasm_runtime_lookup_function(plugin->wasm_state->module_inst, "sc_plugin_shutdown");
        if (shutdown_func != nullptr) {
            (void)wasm_runtime_call_wasm(plugin->wasm_state->exec_env, shutdown_func, 0, nullptr);
        }
    }
    wasm_state_destroy(plugin->wasm_state);
    sc_free(plugin->alloc, plugin->wasm_state, sizeof(*plugin->wasm_state), _Alignof(sc_wasm_plugin_state));
    plugin->wasm_state = nullptr;
}

static sc_status wasm_parse_u64_field(const sc_json_value *object, sc_str key, uint64_t default_value, uint64_t *out)
{
    sc_json_value *value = sc_json_object_get(object, key);
    double number = 0.0;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.wasm_spec_invalid_argument");
    }
    if (value == nullptr || sc_json_is_null(value)) {
        *out = default_value;
        return sc_status_ok();
    }
    if (!sc_json_as_number(value, &number) || number < 0.0 || number > (double)UINT64_MAX) {
        return sc_status_parse("sc.plugin_loader.wasm_spec_number_invalid");
    }
    *out = (uint64_t)number;
    return sc_status_ok();
}

static sc_status wasm_parse_tool_spec(sc_wasm_plugin_state *state,
                                      sc_str spec_json,
                                      sc_str invoke_name,
                                      sc_wasm_tool_entry **out)
{
    sc_json_value *root = nullptr;
    sc_json_value *schema = nullptr;
    sc_wasm_tool_entry *tool = nullptr;
    uint64_t number = 0;
    sc_status status;

    if (state == nullptr || out == nullptr || invoke_name.len == 0) {
        return sc_status_invalid_argument("sc.plugin_loader.wasm_spec_invalid_argument");
    }
    status = sc_json_parse(state->alloc, spec_json, &root, nullptr);
    if (sc_status_is_ok(status) && sc_json_type_of(root) != SC_JSON_OBJECT) {
        status = sc_status_parse("sc.plugin_loader.wasm_spec_root_not_object");
    }
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(root);
        return status;
    }
    tool = sc_alloc(state->alloc, sizeof(*tool), _Alignof(sc_wasm_tool_entry));
    if (tool == nullptr) {
        sc_json_destroy(root);
        return sc_status_no_memory();
    }
    *tool = (sc_wasm_tool_entry){.state = state};
    status = copy_json_string(state->alloc, root, sc_str_from_cstr("name"), true, &tool->name);
    if (sc_status_is_ok(status)) {
        status = copy_json_string(state->alloc, root, sc_str_from_cstr("description"), true, &tool->description);
    }
    if (sc_status_is_ok(status)) {
        status = copy_json_string(state->alloc, root, sc_str_from_cstr("catalog_metadata_key"), false, &tool->catalog_metadata_key);
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_from_str(state->alloc, invoke_name, &tool->invoke_name);
    }
    if (sc_status_is_ok(status)) {
        status = wasm_parse_u64_field(root, sc_str_from_cstr("capabilities"), SC_CONTRACT_CAP_NONE, &tool->capabilities);
    }
    if (sc_status_is_ok(status)) {
        status = wasm_parse_u64_field(root, sc_str_from_cstr("risk"), SC_TOOL_RISK_READONLY, &number);
        tool->risk = (sc_tool_risk)number;
    }
    if (sc_status_is_ok(status)) {
        status = wasm_parse_u64_field(root, sc_str_from_cstr("capability_category"), SC_TOOL_CAPABILITY_NONE, &number);
        tool->capability_category = (sc_tool_capability_category)number;
    }
    if (sc_status_is_ok(status)) {
        status = wasm_parse_u64_field(root, sc_str_from_cstr("side_effect"), SC_TOOL_SIDE_EFFECT_NONE, &number);
        tool->side_effect = (sc_tool_side_effect)number;
    }
    if (sc_status_is_ok(status)) {
        status = wasm_parse_u64_field(root, sc_str_from_cstr("default_autonomy"), SC_AUTONOMY_READ_ONLY, &number);
        tool->default_autonomy = (sc_autonomy_level)number;
    }
    schema = sc_json_object_get(root, sc_str_from_cstr("input_schema"));
    if (sc_status_is_ok(status) && schema != nullptr && !sc_json_is_null(schema)) {
        status = sc_json_clone(schema, state->alloc, &tool->input_schema);
    }
    schema = sc_json_object_get(root, sc_str_from_cstr("output_schema"));
    if (sc_status_is_ok(status) && schema != nullptr && !sc_json_is_null(schema)) {
        status = sc_json_clone(schema, state->alloc, &tool->output_schema);
    }
    if (sc_status_is_ok(status)) {
        tool->vtab = (sc_tool_vtab){
            .struct_size = sizeof(sc_tool_vtab),
            .abi_major = SC_ABI_VERSION_MAJOR,
            .name = tool->name.ptr,
            .display_name = tool->name.ptr,
            .feature_flag = "SC_ENABLE_WASM_PLUGINS",
            .capabilities = tool->capabilities,
            .stability = SC_STABILITY_EXPERIMENTAL,
            .spec = wasm_tool_spec,
            .invoke = wasm_tool_invoke,
            .destroy = wasm_tool_destroy,
        };
        if (!sc_tool_vtab_valid(&tool->vtab)) {
            status = sc_status_invalid_argument("sc.plugin_loader.wasm_tool_invalid_vtab");
        }
    }
    sc_json_destroy(root);
    if (!sc_status_is_ok(status)) {
        wasm_tool_entry_destroy(tool);
        return status;
    }
    *out = tool;
    return sc_status_ok();
}

static void wasm_tool_entry_destroy(sc_wasm_tool_entry *tool)
{
    if (tool == nullptr) {
        return;
    }
    sc_string_clear(&tool->name);
    sc_string_clear(&tool->description);
    sc_string_clear(&tool->catalog_metadata_key);
    sc_string_clear(&tool->invoke_name);
    sc_json_destroy(tool->input_schema);
    sc_json_destroy(tool->output_schema);
    sc_free(tool->state->alloc, tool, sizeof(*tool), _Alignof(sc_wasm_tool_entry));
}

static void wasm_state_destroy(sc_wasm_plugin_state *state)
{
    if (state == nullptr) {
        return;
    }
    for (size_t i = 0; i < state->tools.inner.len; i += 1) {
        void *const *slot = sc_vec_at_const(&state->tools.inner, i);
        wasm_tool_entry_destroy(slot == nullptr ? nullptr : *slot);
    }
    sc_ptr_vec_clear(&state->tools);
    if (state->exec_env != nullptr) {
        wasm_runtime_destroy_exec_env(state->exec_env);
    }
    if (state->module_inst != nullptr) {
        wasm_runtime_deinstantiate(state->module_inst);
    }
    if (state->module != nullptr) {
        wasm_runtime_unload(state->module);
    }
    if (state->module_bytes != nullptr) {
        sc_free(state->alloc, state->module_bytes, state->module_size, _Alignof(uint8_t));
    }
    wasm_runtime_release();
    *state = (sc_wasm_plugin_state){0};
}

static sc_status wasm_register_tool_entry(sc_wasm_tool_entry *tool)
{
    sc_plugin_tool_entry entry = {0};

    if (tool == nullptr || tool->state == nullptr || tool->state->plugin == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.wasm_tool_invalid_argument");
    }
    if (tool_registered(tool->state->plugin->host, sc_string_as_str(&tool->name))) {
        return sc_status_invalid_argument("sc.plugin.tool.duplicate");
    }
    entry = (sc_plugin_tool_entry){.plugin = tool->state->plugin, .vtab = &tool->vtab, .factory = wasm_tool_factory};
    return sc_vec_push(&tool->state->plugin->host->tool_entries, &entry);
}

static sc_status wasm_create_tool_for_vtab(sc_plugin *plugin, const sc_tool_vtab *vtab, sc_allocator *alloc, sc_tool **out)
{
    if (plugin == nullptr || plugin->wasm_state == nullptr || vtab == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.wasm_tool_invalid_argument");
    }
    for (size_t i = 0; i < plugin->wasm_state->tools.inner.len; i += 1) {
        void *const *slot = sc_vec_at_const(&plugin->wasm_state->tools.inner, i);
        sc_wasm_tool_entry *tool = slot == nullptr ? nullptr : *slot;
        if (tool != nullptr && &tool->vtab == vtab) {
            return sc_tool_new(alloc, &tool->vtab, tool, out);
        }
    }
    return sc_status_invalid_argument("sc.plugin_loader.wasm_tool_not_found");
}

static sc_status wasm_tool_factory(sc_allocator *alloc, sc_tool **out)
{
    (void)alloc;
    (void)out;
    return sc_status_invalid_argument("sc.plugin_loader.wasm_factory_unbound");
}

static sc_status wasm_tool_spec(void *impl, sc_tool_spec *out)
{
    sc_wasm_tool_entry *tool = impl;

    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.wasm_tool_invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(sc_tool_spec),
        .name = sc_string_as_str(&tool->name),
        .description = sc_string_as_str(&tool->description),
        .input_schema = tool->input_schema,
        .capabilities = tool->capabilities,
        .risk = tool->risk,
        .output_schema = tool->output_schema,
        .capability_category = tool->capability_category,
        .side_effect = tool->side_effect,
        .default_autonomy = tool->default_autonomy,
        .catalog_metadata_key = sc_string_as_str(&tool->catalog_metadata_key),
    };
    return sc_status_ok();
}

static sc_status wasm_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_wasm_tool_entry *tool = impl;
    sc_string args_json = {0};
    void *native_args = nullptr;
    uint64_t app_args = 0;
    sc_wasm_call_state call_state = {0};
    wasm_function_inst_t func = nullptr;
    sc_status status;

    if (tool == nullptr || call == nullptr || out == nullptr || tool->state == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.wasm_tool_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_tool_result){.struct_size = sizeof(*out)};
    if (call->args != nullptr) {
        status = sc_json_serialize(call->args, alloc, &args_json);
    } else {
        status = sc_string_from_cstr(alloc, "{}", &args_json);
    }
    if (sc_status_is_ok(status)) {
        func = wasm_runtime_lookup_function(tool->state->module_inst, tool->invoke_name.ptr);
        if (func == nullptr) {
            status = sc_status_invalid_argument("sc.plugin_loader.wasm_invoke_missing");
        }
    }
    if (sc_status_is_ok(status)) {
        app_args = wasm_runtime_module_malloc(tool->state->module_inst, args_json.len, &native_args);
        if (app_args == 0 || native_args == nullptr) {
            status = sc_status_no_memory();
        }
    }
    if (sc_status_is_ok(status)) {
        uint32_t argv[2] = {0};

        memcpy(native_args, args_json.ptr, args_json.len);
        call_state = (sc_wasm_call_state){.alloc = alloc, .result = out, .status_code = SC_OK};
        tool->state->active_call = &call_state;
        argv[0] = (uint32_t)app_args;
        argv[1] = (uint32_t)args_json.len;
        if (!wasm_runtime_call_wasm(tool->state->exec_env, func, 2, argv)) {
            status = sc_status_invalid_argument("sc.plugin_loader.wasm_invoke_failed");
        } else if (call_state.status_code != SC_OK) {
            status = wasm_status_from_code(call_state.status_code,
                                           call_state.status_key.ptr == nullptr ? "sc.plugin_loader.wasm_guest_failed"
                                                                                : call_state.status_key.ptr);
        } else if (!call_state.result_set) {
            status = sc_status_invalid_argument("sc.plugin_loader.wasm_result_missing");
        }
        tool->state->active_call = nullptr;
    }
    if (app_args != 0) {
        wasm_runtime_module_free(tool->state->module_inst, app_args);
    }
    sc_string_clear(&args_json);
    sc_string_clear(&call_state.status_key);
    if (!sc_status_is_ok(status)) {
        sc_tool_result_clear(out);
    }
    return status;
}

static void wasm_tool_destroy(void *impl)
{
    (void)impl;
}

static int32_t wasm_host_register_tool(wasm_exec_env_t exec_env,
                                       uint32_t spec_ptr,
                                       uint32_t spec_len,
                                       uint32_t invoke_ptr,
                                       uint32_t invoke_len)
{
    sc_wasm_plugin_state *state = wasm_state_from_exec_env(exec_env);
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    sc_string spec_json = {0};
    sc_string invoke_name = {0};
    sc_wasm_tool_entry *tool = nullptr;
    sc_status status;

    if (state == nullptr || spec_len == 0 || invoke_len == 0) {
        return SC_ERR_INVALID_ARGUMENT;
    }
    if (!wasm_copy_app_str(module_inst, spec_ptr, spec_len, state->alloc, &spec_json) ||
        !wasm_copy_app_str(module_inst, invoke_ptr, invoke_len, state->alloc, &invoke_name)) {
        sc_string_clear(&spec_json);
        sc_string_clear(&invoke_name);
        return SC_ERR_INVALID_ARGUMENT;
    }
    status = wasm_parse_tool_spec(state, sc_string_as_str(&spec_json), sc_string_as_str(&invoke_name), &tool);
    if (sc_status_is_ok(status)) {
        status = wasm_register_tool_entry(tool);
    }
    if (sc_status_is_ok(status)) {
        status = sc_ptr_vec_push(&state->tools, tool);
    }
    if (!sc_status_is_ok(status)) {
        wasm_tool_entry_destroy(tool);
    }
    sc_string_clear(&spec_json);
    sc_string_clear(&invoke_name);
    return status.code;
}

static void wasm_host_set_tool_result(wasm_exec_env_t exec_env, int32_t success, uint32_t output_ptr, uint32_t output_len)
{
    sc_wasm_plugin_state *state = wasm_state_from_exec_env(exec_env);
    sc_wasm_call_state *call = state == nullptr ? nullptr : state->active_call;
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    sc_string output = {0};

    if (call == nullptr || call->result == nullptr) {
        return;
    }
    if (!wasm_copy_app_str(module_inst, output_ptr, output_len, call->alloc, &output)) {
        call->status_code = SC_ERR_INVALID_ARGUMENT;
        return;
    }
    sc_string_clear(&call->result->output);
    call->result->success = success != 0;
    call->result->output = output;
    call->result_set = true;
}

static void wasm_host_set_status(wasm_exec_env_t exec_env, int32_t code, uint32_t key_ptr, uint32_t key_len)
{
    sc_wasm_plugin_state *state = wasm_state_from_exec_env(exec_env);
    sc_wasm_call_state *call = state == nullptr ? nullptr : state->active_call;
    wasm_module_inst_t module_inst = wasm_runtime_get_module_inst(exec_env);
    sc_string key = {0};

    if (call == nullptr) {
        return;
    }
    if (key_len > 0 && !wasm_copy_app_str(module_inst, key_ptr, key_len, call->alloc, &key)) {
        call->status_code = SC_ERR_INVALID_ARGUMENT;
        return;
    }
    sc_string_clear(&call->status_key);
    call->status_code = code >= SC_OK && code <= SC_ERR_CANCELLED ? (sc_status_code)code : SC_ERR_INVALID_ARGUMENT;
    call->status_key = key;
}

static sc_wasm_plugin_state *wasm_state_from_exec_env(wasm_exec_env_t exec_env)
{
    wasm_module_inst_t module_inst = exec_env == nullptr ? nullptr : wasm_runtime_get_module_inst(exec_env);
    return module_inst == nullptr ? nullptr : wasm_runtime_get_custom_data(module_inst);
}

static bool wasm_copy_app_str(wasm_module_inst_t module_inst,
                              uint32_t ptr,
                              uint32_t len,
                              sc_allocator *alloc,
                              sc_string *out)
{
    const char *native = nullptr;

    if (out == nullptr || module_inst == nullptr || (len > 0 && ptr == 0)) {
        return false;
    }
    if (len == 0) {
        return sc_status_is_ok(sc_string_from_cstr(alloc, "", out));
    }
    if (!wasm_runtime_validate_app_addr(module_inst, ptr, len)) {
        return false;
    }
    native = wasm_runtime_addr_app_to_native(module_inst, ptr);
    return native != nullptr && sc_status_is_ok(sc_string_from_str(alloc, sc_str_from_parts(native, len), out));
}

static sc_status wasm_status_from_code(sc_status_code code, const char *key)
{
    switch (code) {
    case SC_OK:
        return sc_status_ok();
    case SC_ERR_INVALID_ARGUMENT:
        return sc_status_invalid_argument(key);
    case SC_ERR_NO_MEMORY:
        return sc_status_no_memory();
    case SC_ERR_IO:
        return sc_status_io(key);
    case SC_ERR_PARSE:
        return sc_status_parse(key);
    case SC_ERR_HTTP:
        return sc_status_http(key);
    case SC_ERR_SECURITY_DENIED:
        return sc_status_security_denied(key);
    case SC_ERR_UNSUPPORTED:
        return sc_status_unsupported(key);
    case SC_ERR_TIMEOUT:
        return sc_status_timeout(key);
    case SC_ERR_CANCELLED:
        return sc_status_cancelled(key);
    }
    return sc_status_invalid_argument("sc.plugin_loader.wasm_status_invalid");
}
#endif

sc_allocator *sc_plugin_script_allocator(const sc_plugin *plugin)
{
    return plugin == nullptr || plugin->alloc == nullptr ? sc_allocator_heap() : plugin->alloc;
}

sc_str sc_plugin_script_artifact_path(const sc_plugin *plugin)
{
    return plugin == nullptr ? sc_str_from_parts(nullptr, 0) : sc_string_as_str(&plugin->manifest.artifact_path);
}

sc_status sc_plugin_script_load_begin(sc_plugin_host *host,
                                      const sc_plugin_manifest *manifest,
                                      uint64_t language_capability,
                                      void *runtime_state,
                                      const sc_script_runtime_vtab *vtab,
                                      sc_plugin **out)
{
    sc_plugin *plugin = nullptr;
    sc_script_plugin_state *state = nullptr;
    sc_status status;

    if (host == nullptr || manifest == nullptr || out == nullptr || runtime_state == nullptr ||
        vtab == nullptr || vtab->struct_size < sizeof(*vtab) || vtab->invoke == nullptr ||
        vtab->release_invoke == nullptr || vtab->destroy == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.script_invalid_argument");
    }
    *out = nullptr;
    status = script_validate_manifest(host,
                                      manifest,
                                      language_capability,
                                      "python");
    if (!sc_status_is_ok(status)) {
        return status;
    }

    plugin = sc_alloc(host->alloc, sizeof(*plugin), _Alignof(sc_plugin));
    state = sc_alloc(host->alloc, sizeof(*state), _Alignof(sc_script_plugin_state));
    if (plugin == nullptr || state == nullptr) {
        if (plugin != nullptr) {
            sc_free(host->alloc, plugin, sizeof(*plugin), _Alignof(sc_plugin));
        }
        if (state != nullptr) {
            sc_free(host->alloc, state, sizeof(*state), _Alignof(sc_script_plugin_state));
        }
        return sc_status_no_memory();
    }

    *plugin = (sc_plugin){.alloc = host->alloc, .host = host, .script_state = state};
    *state = (sc_script_plugin_state){
        .alloc = host->alloc,
        .plugin = plugin,
        .language_capability = language_capability,
        .runtime_state = runtime_state,
        .vtab = vtab,
    };
    sc_ptr_vec_init(&state->tools, host->alloc);

    status = sc_plugin_manifest_copy(host->alloc, manifest, &plugin->manifest);
    if (!sc_status_is_ok(status)) {
        plugin_script_shutdown(plugin, false);
        sc_free(host->alloc, plugin, sizeof(*plugin), _Alignof(sc_plugin));
        return status;
    }
    *out = plugin;
    return sc_status_ok();
}

sc_status sc_plugin_script_register_tool(sc_plugin *plugin, sc_str spec_json, void *invoke_ref)
{
    sc_script_tool_entry *tool = nullptr;
    sc_status status;

    if (plugin == nullptr || plugin->script_state == nullptr || invoke_ref == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.script_tool_invalid_argument");
    }
    status = script_parse_tool_spec(plugin->script_state, spec_json, invoke_ref, &tool);
    if (sc_status_is_ok(status)) {
        status = script_register_tool_entry(tool);
    }
    if (sc_status_is_ok(status)) {
        status = sc_ptr_vec_push(&plugin->script_state->tools, tool);
    }
    if (!sc_status_is_ok(status)) {
        if (tool != nullptr && tool->state != nullptr) {
            remove_plugin_registrations(tool->state->plugin->host, tool->state->plugin);
        }
        script_tool_entry_destroy(tool);
    }
    return status;
}

sc_status sc_plugin_script_load_finish(sc_plugin *plugin, sc_plugin **out)
{
    sc_status status;

    if (plugin == nullptr || plugin->host == nullptr || plugin->script_state == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.script_invalid_argument");
    }
    if (plugin->script_state->tools.inner.len == 0) {
        return sc_status_invalid_argument("sc.plugin_loader.script_no_tools_registered");
    }
    plugin->initialized = true;
    status = sc_ptr_vec_push(&plugin->host->loaded_plugins, plugin);
    if (!sc_status_is_ok(status)) {
        plugin->initialized = false;
        return status;
    }
    *out = plugin;
    return sc_status_ok();
}

void sc_plugin_script_load_abort(sc_plugin *plugin)
{
    if (plugin == nullptr) {
        return;
    }
    remove_plugin_registrations(plugin->host, plugin);
    plugin_script_shutdown(plugin, false);
    sc_plugin_manifest_clear(&plugin->manifest);
    sc_free(plugin->alloc, plugin, sizeof(*plugin), _Alignof(sc_plugin));
}

static void plugin_script_shutdown(sc_plugin *plugin, bool call_runtime_shutdown)
{
    sc_script_plugin_state *state = plugin == nullptr ? nullptr : plugin->script_state;

    if (state == nullptr) {
        return;
    }
    if (call_runtime_shutdown && state->vtab != nullptr && state->vtab->shutdown != nullptr) {
        state->vtab->shutdown(state->runtime_state);
    }
    for (size_t i = 0; i < state->tools.inner.len; i += 1) {
        void *const *slot = sc_vec_at_const(&state->tools.inner, i);
        script_tool_entry_destroy(slot == nullptr ? nullptr : *slot);
    }
    sc_ptr_vec_clear(&state->tools);
    if (state->vtab != nullptr && state->vtab->destroy != nullptr) {
        state->vtab->destroy(state->runtime_state);
    }
    sc_free(state->alloc, state, sizeof(*state), _Alignof(sc_script_plugin_state));
    plugin->script_state = nullptr;
    plugin->state = nullptr;
}

static sc_status script_validate_manifest(const sc_plugin_host *host,
                                          const sc_plugin_manifest *manifest,
                                          uint64_t language_capability,
                                          const char *capability_key)
{
    uint64_t allowed_capabilities = language_capability | SC_PLUGIN_CAP_TOOL;
    uint64_t denied_permissions = 0;

    if (host == nullptr || manifest == nullptr || capability_key == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.script_invalid_argument");
    }
    if ((manifest->capabilities & language_capability) == 0) {
        return sc_status_invalid_argument("sc.plugin_loader.python_capability_missing");
    }
    if ((manifest->capabilities & ~allowed_capabilities) != 0) {
        return sc_status_unsupported("sc.plugin_loader.script_capability_unsupported");
    }
    if ((manifest->capabilities & SC_PLUGIN_CAP_TOOL) == 0) {
        return sc_status_unsupported("sc.plugin_loader.script_tool_required");
    }
    if (manifest->abi_major != SC_ABI_VERSION_MAJOR) {
        return sc_status_invalid_argument("sc.plugin_loader.abi_mismatch");
    }
    if (sc_string_is_empty(&manifest->name) || sc_string_is_empty(&manifest->version)) {
        return sc_status_invalid_argument("sc.plugin_loader.manifest_mismatch");
    }
    if ((manifest->requested_permissions & SC_PLUGIN_PERMISSION_TOOL) == 0) {
        return sc_status_security_denied("sc.plugin.registration.permission_denied");
    }
    denied_permissions = manifest->requested_permissions & ~host->allowed_permissions;
    if (denied_permissions != 0) {
        return sc_status_security_denied("sc.plugin_loader.permission_denied");
    }
    if (plugin_name_loaded(host, sc_string_as_str(&manifest->name))) {
        return sc_status_invalid_argument("sc.plugin_loader.duplicate_plugin");
    }
    return sc_status_ok();
}

static sc_status script_parse_u64_field(const sc_json_value *object,
                                        sc_str key,
                                        uint64_t default_value,
                                        uint64_t *out)
{
    sc_json_value *value = sc_json_object_get(object, key);
    double number = 0.0;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.script_spec_invalid_argument");
    }
    if (value == nullptr || sc_json_is_null(value)) {
        *out = default_value;
        return sc_status_ok();
    }
    if (!sc_json_as_number(value, &number) || number < 0.0 || number > (double)UINT64_MAX) {
        return sc_status_parse("sc.plugin_loader.script_spec_number_invalid");
    }
    *out = (uint64_t)number;
    return sc_status_ok();
}

static sc_status script_parse_tool_spec(sc_script_plugin_state *state,
                                        sc_str spec_json,
                                        void *invoke_ref,
                                        sc_script_tool_entry **out)
{
    sc_json_value *root = nullptr;
    sc_json_value *schema = nullptr;
    sc_script_tool_entry *tool = nullptr;
    uint64_t number = 0;
    sc_status status;

    if (state == nullptr || out == nullptr || invoke_ref == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.script_spec_invalid_argument");
    }
    status = sc_json_parse(state->alloc, spec_json, &root, nullptr);
    if (sc_status_is_ok(status) && sc_json_type_of(root) != SC_JSON_OBJECT) {
        status = sc_status_parse("sc.plugin_loader.script_spec_root_not_object");
    }
    if (!sc_status_is_ok(status)) {
        sc_json_destroy(root);
        return status;
    }
    tool = sc_alloc(state->alloc, sizeof(*tool), _Alignof(sc_script_tool_entry));
    if (tool == nullptr) {
        sc_json_destroy(root);
        return sc_status_no_memory();
    }
    *tool = (sc_script_tool_entry){.state = state, .invoke_ref = invoke_ref};
    status = copy_json_string(state->alloc, root, sc_str_from_cstr("name"), true, &tool->name);
    if (sc_status_is_ok(status)) {
        status = copy_json_string(state->alloc, root, sc_str_from_cstr("description"), true, &tool->description);
    }
    if (sc_status_is_ok(status)) {
        status = copy_json_string(state->alloc, root, sc_str_from_cstr("catalog_metadata_key"), false, &tool->catalog_metadata_key);
    }
    if (sc_status_is_ok(status)) {
        status = script_parse_u64_field(root, sc_str_from_cstr("capabilities"), SC_CONTRACT_CAP_NONE, &tool->capabilities);
    }
    if (sc_status_is_ok(status)) {
        status = script_parse_u64_field(root, sc_str_from_cstr("risk"), SC_TOOL_RISK_READONLY, &number);
        tool->risk = (sc_tool_risk)number;
    }
    if (sc_status_is_ok(status)) {
        status = script_parse_u64_field(root, sc_str_from_cstr("capability_category"), SC_TOOL_CAPABILITY_NONE, &number);
        tool->capability_category = (sc_tool_capability_category)number;
    }
    if (sc_status_is_ok(status)) {
        status = script_parse_u64_field(root, sc_str_from_cstr("side_effect"), SC_TOOL_SIDE_EFFECT_NONE, &number);
        tool->side_effect = (sc_tool_side_effect)number;
    }
    if (sc_status_is_ok(status)) {
        status = script_parse_u64_field(root, sc_str_from_cstr("default_autonomy"), SC_AUTONOMY_READ_ONLY, &number);
        tool->default_autonomy = (sc_autonomy_level)number;
    }
    schema = sc_json_object_get(root, sc_str_from_cstr("input_schema"));
    if (sc_status_is_ok(status) && schema != nullptr && !sc_json_is_null(schema)) {
        status = sc_json_clone(schema, state->alloc, &tool->input_schema);
    }
    schema = sc_json_object_get(root, sc_str_from_cstr("output_schema"));
    if (sc_status_is_ok(status) && schema != nullptr && !sc_json_is_null(schema)) {
        status = sc_json_clone(schema, state->alloc, &tool->output_schema);
    }
    if (sc_status_is_ok(status)) {
        tool->vtab = (sc_tool_vtab){
            .struct_size = sizeof(sc_tool_vtab),
            .abi_major = SC_ABI_VERSION_MAJOR,
            .name = tool->name.ptr,
            .display_name = tool->name.ptr,
            .feature_flag = script_feature_flag(state->language_capability),
            .capabilities = tool->capabilities,
            .stability = SC_STABILITY_EXPERIMENTAL,
            .spec = script_tool_spec,
            .invoke = script_tool_invoke,
            .destroy = script_tool_destroy,
        };
        if (!sc_tool_vtab_valid(&tool->vtab)) {
            status = sc_status_invalid_argument("sc.plugin_loader.script_tool_invalid_vtab");
        }
    }
    sc_json_destroy(root);
    if (!sc_status_is_ok(status)) {
        script_tool_entry_destroy(tool);
        return status;
    }
    *out = tool;
    return sc_status_ok();
}

static sc_status script_register_tool_entry(sc_script_tool_entry *tool)
{
    sc_plugin_tool_entry entry = {0};

    if (tool == nullptr || tool->state == nullptr || tool->state->plugin == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.script_tool_invalid_argument");
    }
    if (tool_registered(tool->state->plugin->host, sc_string_as_str(&tool->name))) {
        return sc_status_invalid_argument("sc.plugin.tool.duplicate");
    }
    entry = (sc_plugin_tool_entry){.plugin = tool->state->plugin, .vtab = &tool->vtab, .factory = script_tool_factory};
    return sc_vec_push(&tool->state->plugin->host->tool_entries, &entry);
}

static sc_status script_create_tool_for_vtab(sc_plugin *plugin, const sc_tool_vtab *vtab, sc_allocator *alloc, sc_tool **out)
{
    if (plugin == nullptr || plugin->script_state == nullptr || vtab == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.script_tool_invalid_argument");
    }
    for (size_t i = 0; i < plugin->script_state->tools.inner.len; i += 1) {
        void *const *slot = sc_vec_at_const(&plugin->script_state->tools.inner, i);
        sc_script_tool_entry *tool = slot == nullptr ? nullptr : *slot;
        if (tool != nullptr && &tool->vtab == vtab) {
            return sc_tool_new(alloc, &tool->vtab, tool, out);
        }
    }
    return sc_status_invalid_argument("sc.plugin_loader.script_tool_not_found");
}

static sc_status script_tool_factory(sc_allocator *alloc, sc_tool **out)
{
    (void)alloc;
    (void)out;
    return sc_status_invalid_argument("sc.plugin_loader.script_factory_unbound");
}

static sc_status script_tool_spec(void *impl, sc_tool_spec *out)
{
    sc_script_tool_entry *tool = impl;

    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.script_tool_invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(sc_tool_spec),
        .name = sc_string_as_str(&tool->name),
        .description = sc_string_as_str(&tool->description),
        .input_schema = tool->input_schema,
        .capabilities = tool->capabilities,
        .risk = tool->risk,
        .output_schema = tool->output_schema,
        .capability_category = tool->capability_category,
        .side_effect = tool->side_effect,
        .default_autonomy = tool->default_autonomy,
        .catalog_metadata_key = sc_string_as_str(&tool->catalog_metadata_key),
    };
    return sc_status_ok();
}

static sc_status script_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_script_tool_entry *tool = impl;
    sc_string args_json = {0};
    sc_status status;

    if (tool == nullptr || call == nullptr || out == nullptr || tool->state == nullptr ||
        tool->state->vtab == nullptr || tool->state->vtab->invoke == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.script_tool_invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    *out = (sc_tool_result){.struct_size = sizeof(*out)};
    if (call->args != nullptr) {
        status = sc_json_serialize(call->args, alloc, &args_json);
    } else {
        status = sc_string_from_cstr(alloc, "{}", &args_json);
    }
    if (sc_status_is_ok(status)) {
        status = tool->state->vtab->invoke(tool->state->runtime_state,
                                           tool->invoke_ref,
                                           call,
                                           sc_string_as_str(&args_json),
                                           alloc,
                                           out);
    }
    sc_string_clear(&args_json);
    if (!sc_status_is_ok(status)) {
        sc_tool_result_clear(out);
    }
    return status;
}

static void script_tool_destroy(void *impl)
{
    (void)impl;
}

static void script_tool_entry_destroy(sc_script_tool_entry *tool)
{
    sc_allocator *alloc = nullptr;

    if (tool == nullptr) {
        return;
    }
    alloc = tool->state == nullptr || tool->state->alloc == nullptr ? sc_allocator_heap() : tool->state->alloc;
    if (tool->state != nullptr && tool->state->vtab != nullptr && tool->state->vtab->release_invoke != nullptr &&
        tool->invoke_ref != nullptr) {
        tool->state->vtab->release_invoke(tool->state->runtime_state, tool->invoke_ref);
    }
    sc_string_clear(&tool->name);
    sc_string_clear(&tool->description);
    sc_string_clear(&tool->catalog_metadata_key);
    sc_json_destroy(tool->input_schema);
    sc_json_destroy(tool->output_schema);
    sc_free(alloc, tool, sizeof(*tool), _Alignof(sc_script_tool_entry));
}

static const char *script_feature_flag(uint64_t language_capability)
{
    if (language_capability == SC_PLUGIN_CAP_PYTHON) {
        return "SC_ENABLE_PYTHON_PLUGINS";
    }
    return "SC_ENABLE_PLUGINS";
}

static sc_status register_observer_api(const sc_plugin_host_api *api,
                                       const sc_observer_vtab *vtab,
                                       sc_observer_factory factory)
{
    sc_plugin_registration_context *context = api == nullptr ? nullptr : api->userdata;
    sc_plugin_observer_adapter *adapter = nullptr;
    sc_observer *inner = nullptr;
    sc_observer *wrapped = nullptr;
    sc_status status;

    if (context == nullptr || context->host == nullptr || context->plugin == nullptr ||
        context->host->observers == nullptr || factory == nullptr || !sc_observer_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.plugin.observer.invalid_registration");
    }
    status = register_permission_check(context->plugin, SC_PLUGIN_PERMISSION_OBSERVER, SC_PLUGIN_CAP_OBSERVER);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    status = factory(context->host->alloc, &inner);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    adapter = sc_alloc(context->host->alloc, sizeof(*adapter), _Alignof(sc_plugin_observer_adapter));
    if (adapter == nullptr) {
        sc_observer_destroy(inner);
        return sc_status_no_memory();
    }
    *adapter = (sc_plugin_observer_adapter){.alloc = context->host->alloc, .plugin = context->plugin, .inner = inner, .vtab = *vtab};
    adapter->vtab.emit = observer_adapter_emit;
    adapter->vtab.flush = observer_adapter_flush;
    adapter->vtab.destroy = observer_adapter_destroy;
    context->plugin->active_refs += 1;
    status = sc_observer_new(context->host->alloc, &adapter->vtab, adapter, &wrapped);
    if (sc_status_is_ok(status)) {
        status = sc_observer_list_add(context->host->observers, wrapped);
    }
    if (!sc_status_is_ok(status)) {
        if (wrapped != nullptr) {
            sc_observer_destroy(wrapped);
        } else {
            sc_plugin_release(context->plugin);
            sc_observer_destroy(inner);
            sc_free(context->host->alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_observer_adapter));
        }
    }
    return status;
}

static sc_status register_peripheral_api(const sc_plugin_host_api *api,
                                         const sc_peripheral_vtab *vtab,
                                         sc_peripheral_factory factory)
{
    sc_plugin_registration_context *context = api == nullptr ? nullptr : api->userdata;
    sc_plugin_peripheral_entry entry = {0};
    sc_status status;

    if (context == nullptr || context->host == nullptr || context->plugin == nullptr ||
        factory == nullptr || !sc_peripheral_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.plugin.peripheral.invalid_registration");
    }
    status = register_permission_check(context->plugin, SC_PLUGIN_PERMISSION_PERIPHERAL, SC_PLUGIN_CAP_PERIPHERAL);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (peripheral_registered(context->host, sc_str_from_cstr(vtab->name))) {
        return sc_status_invalid_argument("sc.plugin.peripheral.duplicate");
    }
    entry = (sc_plugin_peripheral_entry){.plugin = context->plugin, .vtab = vtab, .factory = factory};
    return sc_vec_push(&context->host->peripheral_entries, &entry);
}

static sc_status plugin_load_descriptor_with_handle(sc_plugin_host *host,
                                                    const sc_plugin_descriptor *descriptor,
                                                    const sc_plugin_manifest *manifest,
                                                    void *dynamic_handle,
                                                    sc_plugin **out)
{
    sc_plugin *plugin = nullptr;
    sc_plugin_registration_context context;
    sc_plugin_host_api api = {0};
    sc_status status;

    if (host == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.invalid_argument");
    }
    status = validate_descriptor_and_manifest(host, descriptor, manifest);
    if (!sc_status_is_ok(status)) {
        return status;
    }

    plugin = sc_alloc(host->alloc, sizeof(*plugin), _Alignof(sc_plugin));
    if (plugin == nullptr) {
        return sc_status_no_memory();
    }
    *plugin = (sc_plugin){
        .alloc = host->alloc,
        .host = host,
        .descriptor = descriptor,
        .dynamic_handle = dynamic_handle,
    };
    status = sc_plugin_manifest_copy(host->alloc, manifest, &plugin->manifest);
    if (sc_status_is_ok(status)) {
        context = (sc_plugin_registration_context){.host = host, .plugin = plugin};
        status = make_host_api(&context, &api);
    }
    if (sc_status_is_ok(status)) {
        status = descriptor->init(&api, &plugin->state);
    }
    if (sc_status_is_ok(status)) {
        plugin->initialized = true;
        status = sc_ptr_vec_push(&host->loaded_plugins, plugin);
    }
    if (!sc_status_is_ok(status)) {
        remove_plugin_registrations(host, plugin);
        if ((plugin->initialized || plugin->state != nullptr) && descriptor->shutdown != nullptr) {
            descriptor->shutdown(plugin->state);
        }
        sc_plugin_manifest_clear(&plugin->manifest);
        sc_free(host->alloc, plugin, sizeof(*plugin), _Alignof(sc_plugin));
        return status;
    }
    *out = plugin;
    return sc_status_ok();
}

static bool descriptor_valid(const sc_plugin_descriptor *descriptor)
{
    size_t minimum_size = offsetof(sc_plugin_descriptor, shutdown) + sizeof(descriptor->shutdown);

    return descriptor != nullptr &&
           descriptor->struct_size >= minimum_size &&
           descriptor->abi_major == SC_ABI_VERSION_MAJOR &&
           descriptor->name != nullptr &&
           descriptor->version != nullptr &&
           sc_contract_name_is_valid(sc_str_from_cstr(descriptor->name)) &&
           descriptor->init != nullptr;
}

static sc_status validate_descriptor_and_manifest(const sc_plugin_host *host,
                                                  const sc_plugin_descriptor *descriptor,
                                                  const sc_plugin_manifest *manifest)
{
    uint64_t denied_permissions = 0;

    if (host == nullptr || manifest == nullptr) {
        return sc_status_invalid_argument("sc.plugin_loader.invalid_argument");
    }
    if (!descriptor_valid(descriptor)) {
        return sc_status_invalid_argument("sc.plugin_loader.invalid_descriptor");
    }
    if (descriptor_field_available(descriptor,
                                   offsetof(sc_plugin_descriptor, minimum_host_descriptor_size) +
                                       sizeof(descriptor->minimum_host_descriptor_size)) &&
        descriptor->minimum_host_descriptor_size > sizeof(sc_plugin_descriptor)) {
        return sc_status_unsupported("sc.plugin_loader.host_descriptor_too_small");
    }
    if (manifest->abi_major != SC_ABI_VERSION_MAJOR || manifest->abi_major != descriptor->abi_major) {
        return sc_status_invalid_argument("sc.plugin_loader.abi_mismatch");
    }
    if (!sc_str_equal(sc_string_as_str(&manifest->name), sc_str_from_cstr(descriptor->name)) ||
        !sc_str_equal(sc_string_as_str(&manifest->version), sc_str_from_cstr(descriptor->version))) {
        return sc_status_invalid_argument("sc.plugin_loader.manifest_mismatch");
    }
    if (plugin_name_loaded(host, sc_string_as_str(&manifest->name))) {
        return sc_status_invalid_argument("sc.plugin_loader.duplicate_plugin");
    }
    if (descriptor_requested_capabilities(descriptor) != 0 &&
        descriptor_requested_capabilities(descriptor) != manifest->capabilities) {
        return sc_status_security_denied("sc.plugin_loader.capability_mismatch");
    }
    if (descriptor_requested_permissions(descriptor) != 0 &&
        descriptor_requested_permissions(descriptor) != manifest->requested_permissions) {
        return sc_status_security_denied("sc.plugin_loader.permission_mismatch");
    }
    denied_permissions = manifest->requested_permissions & ~host->allowed_permissions;
    if (denied_permissions != 0) {
        return sc_status_security_denied("sc.plugin_loader.permission_denied");
    }
    return sc_status_ok();
}

static sc_status copy_json_string(sc_allocator *alloc,
                                  const sc_json_value *object,
                                  sc_str key,
                                  bool required,
                                  sc_string *out)
{
    sc_json_value *value = sc_json_object_get(object, key);
    sc_str text = {0};

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_manifest.invalid_argument");
    }
    if (value == nullptr || sc_json_is_null(value)) {
        if (required) {
            return sc_status_parse("sc.plugin_manifest.missing_string");
        }
        return sc_string_from_cstr(alloc, "", out);
    }
    if (!sc_json_as_str(value, &text)) {
        return sc_status_parse("sc.plugin_manifest.string_type");
    }
    if (required && text.len == 0) {
        return sc_status_parse("sc.plugin_manifest.empty_string");
    }
    return sc_string_from_str(alloc, text, out);
}

static sc_status parse_u32_field(const sc_json_value *object, sc_str key, bool required, uint32_t *out)
{
    sc_json_value *value = sc_json_object_get(object, key);
    double number = 0.0;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.plugin_manifest.invalid_argument");
    }
    if (value == nullptr || sc_json_is_null(value)) {
        if (required) {
            return sc_status_parse("sc.plugin_manifest.missing_number");
        }
        return sc_status_ok();
    }
    if (!sc_json_as_number(value, &number) || number < 0.0 || number > 4294967295.0 ||
        (double)(uint32_t)number != number) {
        return sc_status_parse("sc.plugin_manifest.number_type");
    }
    *out = (uint32_t)number;
    return sc_status_ok();
}

static sc_status parse_string_flags(const sc_json_value *object,
                                    sc_str key,
                                    uint64_t (*flag_from_name)(sc_str name),
                                    uint64_t *out)
{
    sc_json_value *array = sc_json_object_get(object, key);

    if (out == nullptr || flag_from_name == nullptr) {
        return sc_status_invalid_argument("sc.plugin_manifest.invalid_argument");
    }
    *out = 0;
    if (array == nullptr || sc_json_is_null(array)) {
        return sc_status_ok();
    }
    if (sc_json_type_of(array) != SC_JSON_ARRAY) {
        return sc_status_parse("sc.plugin_manifest.flags_not_array");
    }
    for (size_t i = 0; i < sc_json_array_len(array); i += 1) {
        sc_str name = {0};
        uint64_t flag = 0;
        if (!sc_json_as_str(sc_json_array_get(array, i), &name)) {
            return sc_status_parse("sc.plugin_manifest.flag_not_string");
        }
        flag = flag_from_name(name);
        if (flag == 0) {
            return sc_status_parse("sc.plugin_manifest.unknown_flag");
        }
        *out |= flag;
    }
    return sc_status_ok();
}

static uint64_t capability_from_name(sc_str name)
{
    if (sc_str_equal(name, sc_str_from_cstr("provider"))) {
        return SC_PLUGIN_CAP_PROVIDER;
    }
    if (sc_str_equal(name, sc_str_from_cstr("channel"))) {
        return SC_PLUGIN_CAP_CHANNEL;
    }
    if (sc_str_equal(name, sc_str_from_cstr("tool"))) {
        return SC_PLUGIN_CAP_TOOL;
    }
    if (sc_str_equal(name, sc_str_from_cstr("memory"))) {
        return SC_PLUGIN_CAP_MEMORY;
    }
    if (sc_str_equal(name, sc_str_from_cstr("wasm"))) {
        return SC_PLUGIN_CAP_WASM;
    }
    if (sc_str_equal(name, sc_str_from_cstr("observer"))) {
        return SC_PLUGIN_CAP_OBSERVER;
    }
    if (sc_str_equal(name, sc_str_from_cstr("peripheral"))) {
        return SC_PLUGIN_CAP_PERIPHERAL;
    }
    if (sc_str_equal(name, sc_str_from_cstr("python"))) {
        return SC_PLUGIN_CAP_PYTHON;
    }
    return 0;
}

static uint64_t permission_from_name(sc_str name)
{
    if (sc_str_equal(name, sc_str_from_cstr("filesystem"))) {
        return SC_PLUGIN_PERMISSION_FILESYSTEM;
    }
    if (sc_str_equal(name, sc_str_from_cstr("network"))) {
        return SC_PLUGIN_PERMISSION_NETWORK;
    }
    if (sc_str_equal(name, sc_str_from_cstr("shell"))) {
        return SC_PLUGIN_PERMISSION_SHELL;
    }
    if (sc_str_equal(name, sc_str_from_cstr("hardware"))) {
        return SC_PLUGIN_PERMISSION_HARDWARE;
    }
    if (sc_str_equal(name, sc_str_from_cstr("memory"))) {
        return SC_PLUGIN_PERMISSION_MEMORY;
    }
    if (sc_str_equal(name, sc_str_from_cstr("config"))) {
        return SC_PLUGIN_PERMISSION_CONFIG;
    }
    if (sc_str_equal(name, sc_str_from_cstr("secrets"))) {
        return SC_PLUGIN_PERMISSION_SECRETS;
    }
    if (sc_str_equal(name, sc_str_from_cstr("provider"))) {
        return SC_PLUGIN_PERMISSION_PROVIDER;
    }
    if (sc_str_equal(name, sc_str_from_cstr("channel"))) {
        return SC_PLUGIN_PERMISSION_CHANNEL;
    }
    if (sc_str_equal(name, sc_str_from_cstr("tool"))) {
        return SC_PLUGIN_PERMISSION_TOOL;
    }
    if (sc_str_equal(name, sc_str_from_cstr("observer"))) {
        return SC_PLUGIN_PERMISSION_OBSERVER;
    }
    if (sc_str_equal(name, sc_str_from_cstr("peripheral"))) {
        return SC_PLUGIN_PERMISSION_PERIPHERAL;
    }
    return 0;
}

static sc_status register_permission_check(const sc_plugin *plugin, uint64_t permission, uint64_t capability)
{
    if (plugin == nullptr) {
        return sc_status_invalid_argument("sc.plugin.invalid_argument");
    }
    if ((plugin->manifest.requested_permissions & permission) == 0 ||
        (plugin->manifest.capabilities & capability) == 0) {
        return sc_status_security_denied("sc.plugin.registration.permission_denied");
    }
    return sc_status_ok();
}

static bool provider_registered(const sc_plugin_host *host, sc_str name)
{
    if (host == nullptr) {
        return false;
    }
    if (host->providers != nullptr && sc_provider_registry_find(host->providers, name) != nullptr) {
        return true;
    }
    return find_provider_entry(host, name) != nullptr;
}

static bool channel_registered(const sc_plugin_host *host, sc_str name)
{
    if (host == nullptr) {
        return false;
    }
    if (host->channels != nullptr && sc_channel_registry_find(host->channels, name) != nullptr) {
        return true;
    }
    return find_channel_entry(host, name) != nullptr;
}

static bool tool_registered(const sc_plugin_host *host, sc_str name)
{
    if (host == nullptr) {
        return false;
    }
    if (host->tools != nullptr && sc_tool_registry_find(host->tools, name) != nullptr) {
        return true;
    }
    return find_tool_entry(host, name) != nullptr;
}

static bool memory_registered(const sc_plugin_host *host, sc_str name)
{
    if (host == nullptr) {
        return false;
    }
    if (host->memory_backends != nullptr && sc_memory_registry_find(host->memory_backends, name) != nullptr) {
        return true;
    }
    return find_memory_entry(host, name) != nullptr;
}

static bool peripheral_registered(const sc_plugin_host *host, sc_str name)
{
    if (host == nullptr) {
        return false;
    }
    if (host->peripherals != nullptr && sc_peripheral_registry_find(host->peripherals, name) != nullptr) {
        return true;
    }
    return find_peripheral_entry(host, name) != nullptr;
}

static const sc_plugin_tool_entry *find_tool_entry(const sc_plugin_host *host, sc_str name)
{
    if (host == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < host->tool_entries.len; i += 1) {
        const sc_plugin_tool_entry *entry = sc_vec_at_const(&host->tool_entries, i);
        if (entry != nullptr && name_matches(entry->vtab->name, name)) {
            return entry;
        }
    }
    return nullptr;
}

static const sc_plugin_channel_entry *find_channel_entry(const sc_plugin_host *host, sc_str name)
{
    if (host == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < host->channel_entries.len; i += 1) {
        const sc_plugin_channel_entry *entry = sc_vec_at_const(&host->channel_entries, i);
        if (entry != nullptr && name_matches(entry->vtab->name, name)) {
            return entry;
        }
    }
    return nullptr;
}

static const sc_plugin_provider_entry *find_provider_entry(const sc_plugin_host *host, sc_str name)
{
    if (host == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < host->provider_entries.len; i += 1) {
        const sc_plugin_provider_entry *entry = sc_vec_at_const(&host->provider_entries, i);
        if (entry != nullptr && name_matches(entry->vtab->name, name)) {
            return entry;
        }
    }
    return nullptr;
}

static const sc_plugin_memory_entry *find_memory_entry(const sc_plugin_host *host, sc_str name)
{
    if (host == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < host->memory_entries.len; i += 1) {
        const sc_plugin_memory_entry *entry = sc_vec_at_const(&host->memory_entries, i);
        if (entry != nullptr && name_matches(entry->vtab->name, name)) {
            return entry;
        }
    }
    return nullptr;
}

static const sc_plugin_peripheral_entry *find_peripheral_entry(const sc_plugin_host *host, sc_str name)
{
    if (host == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < host->peripheral_entries.len; i += 1) {
        const sc_plugin_peripheral_entry *entry = sc_vec_at_const(&host->peripheral_entries, i);
        if (entry != nullptr && name_matches(entry->vtab->name, name)) {
            return entry;
        }
    }
    return nullptr;
}

#define REMOVE_PLUGIN_ENTRIES(vec_ptr, entry_type)                                                                    \
    do {                                                                                                              \
        size_t write_index__ = 0;                                                                                     \
        for (size_t read_index__ = 0; read_index__ < (vec_ptr)->len; read_index__ += 1) {                             \
            const entry_type *entry__ = sc_vec_at_const((vec_ptr), read_index__);                                      \
            if (entry__ != nullptr && entry__->plugin != plugin) {                                                       \
                if (write_index__ != read_index__) {                                                                  \
                    entry_type *target__ = sc_vec_at((vec_ptr), write_index__);                                        \
                    *target__ = *entry__;                                                                             \
                }                                                                                                     \
                write_index__ += 1;                                                                                   \
            }                                                                                                         \
        }                                                                                                             \
        (vec_ptr)->len = write_index__;                                                                               \
    } while (0)

static void remove_plugin_registrations(sc_plugin_host *host, const sc_plugin *plugin)
{
    if (host == nullptr || plugin == nullptr) {
        return;
    }
    REMOVE_PLUGIN_ENTRIES(&host->provider_entries, sc_plugin_provider_entry);
    REMOVE_PLUGIN_ENTRIES(&host->channel_entries, sc_plugin_channel_entry);
    REMOVE_PLUGIN_ENTRIES(&host->tool_entries, sc_plugin_tool_entry);
    REMOVE_PLUGIN_ENTRIES(&host->memory_entries, sc_plugin_memory_entry);
    REMOVE_PLUGIN_ENTRIES(&host->peripheral_entries, sc_plugin_peripheral_entry);
}

#undef REMOVE_PLUGIN_ENTRIES

static void remove_plugin_pointer(sc_plugin_host *host, const sc_plugin *plugin)
{
    size_t write_index = 0;

    if (host == nullptr || plugin == nullptr) {
        return;
    }
    for (size_t read_index = 0; read_index < host->loaded_plugins.inner.len; read_index += 1) {
        void **slot = sc_vec_at(&host->loaded_plugins.inner, read_index);
        if (slot != nullptr && *slot != plugin) {
            if (write_index != read_index) {
                void **target = sc_vec_at(&host->loaded_plugins.inner, write_index);
                *target = *slot;
            }
            write_index += 1;
        }
    }
    host->loaded_plugins.inner.len = write_index;
}

static bool name_matches(const char *entry_name, sc_str name)
{
    if (name.len > 0 && name.ptr == nullptr) {
        return false;
    }
    return entry_name != nullptr && strlen(entry_name) == name.len &&
           (name.len == 0 || memcmp(entry_name, name.ptr, name.len) == 0);
}

static bool descriptor_field_available(const sc_plugin_descriptor *descriptor, size_t required_size)
{
    return descriptor != nullptr && descriptor->struct_size >= required_size;
}

static uint64_t descriptor_requested_capabilities(const sc_plugin_descriptor *descriptor)
{
    if (!descriptor_field_available(descriptor,
                                    offsetof(sc_plugin_descriptor, requested_capabilities) +
                                        sizeof(descriptor->requested_capabilities))) {
        return 0;
    }
    return descriptor->requested_capabilities;
}

static uint64_t descriptor_requested_permissions(const sc_plugin_descriptor *descriptor)
{
    if (!descriptor_field_available(descriptor,
                                    offsetof(sc_plugin_descriptor, requested_permissions) +
                                        sizeof(descriptor->requested_permissions))) {
        return 0;
    }
    return descriptor->requested_permissions;
}

static bool plugin_name_loaded(const sc_plugin_host *host, sc_str name)
{
    if (host == nullptr) {
        return false;
    }
    for (size_t i = 0; i < host->loaded_plugins.inner.len; i += 1) {
        void *const *slot = sc_vec_at_const(&host->loaded_plugins.inner, i);
        const sc_plugin *plugin = slot == nullptr ? nullptr : *slot;
        if (plugin != nullptr && sc_str_equal(sc_string_as_str(&plugin->manifest.name), name)) {
            return true;
        }
    }
    return false;
}

static bool path_has_extension(sc_str path, sc_str extension)
{
    if (path.ptr == nullptr || extension.ptr == nullptr || path.len < extension.len) {
        return false;
    }
    return memcmp(path.ptr + path.len - extension.len, extension.ptr, extension.len) == 0;
}

static bool path_extension_allowed(const sc_plugin_host *host, sc_str path)
{
    static const char *default_extensions[] = {".so", ".dylib", ".wasm", ".py"};

    if (host != nullptr && host->allowed_extension_count > 0) {
        for (size_t i = 0; i < host->allowed_extension_count; i += 1) {
            if (path_has_extension(path, host->allowed_extensions[i])) {
                return true;
            }
        }
        return false;
    }
    for (size_t i = 0; i < SC_ARRAY_LEN(default_extensions); i += 1) {
        if (path_has_extension(path, sc_str_from_cstr(default_extensions[i]))) {
            return true;
        }
    }
    return false;
}

static sc_status validate_plugin_path(const sc_plugin_host *host, const sc_plugin_manifest *manifest)
{
    char resolved_path[PATH_MAX] = {0};
    struct stat st = {0};
    sc_str artifact = manifest == nullptr ? sc_str_from_parts(nullptr, 0) : sc_string_as_str(&manifest->artifact_path);

    if (host == nullptr || manifest == nullptr || artifact.ptr == nullptr || artifact.len == 0) {
        return sc_status_invalid_argument("sc.plugin_loader.invalid_argument");
    }
    if (!path_extension_allowed(host, artifact)) {
        return sc_status_security_denied("sc.plugin_loader.extension_denied");
    }
    if (realpath(manifest->artifact_path.ptr, resolved_path) == nullptr || stat(resolved_path, &st) != 0) {
        return sc_status_io("sc.plugin_loader.path_not_found");
    }
    if (!S_ISREG(st.st_mode)) {
        return sc_status_security_denied("sc.plugin_loader.not_regular_file");
    }
    if (host->plugin_root_count > 0) {
        bool allowed = false;
        for (size_t i = 0; i < host->plugin_root_count; i += 1) {
            char resolved_root[PATH_MAX] = {0};
            size_t root_len = 0;
            if (host->plugin_roots[i].ptr == nullptr ||
                realpath(host->plugin_roots[i].ptr, resolved_root) == nullptr) {
                continue;
            }
            root_len = strlen(resolved_root);
            if (strncmp(resolved_path, resolved_root, root_len) == 0 &&
                (resolved_path[root_len] == '\0' || resolved_path[root_len] == '/')) {
                allowed = true;
                break;
            }
        }
        if (!allowed) {
            return sc_status_security_denied("sc.plugin_loader.path_denied");
        }
    }
    return sc_status_ok();
}

static sc_status make_host_api(sc_plugin_registration_context *context, sc_plugin_host_api *out)
{
    if (context == nullptr || out == nullptr || context->host == nullptr) {
        return sc_status_invalid_argument("sc.plugin_host_api.invalid_argument");
    }
    *out = (sc_plugin_host_api){
        .struct_size = sizeof(*out),
        .userdata = context,
        .abi_major = SC_ABI_VERSION_MAJOR,
        .abi_minor = SC_ABI_VERSION_MINOR,
        .allowed_permissions = context->host->allowed_permissions,
        .register_provider = register_provider_api,
        .register_channel = register_channel_api,
        .register_tool = register_tool_api,
        .register_memory = register_memory_api,
        .register_observer = register_observer_api,
        .register_peripheral = register_peripheral_api,
    };
    return sc_status_ok();
}

static sc_status provider_adapter_generate(void *impl,
                                           const sc_provider_request *request,
                                           sc_allocator *alloc,
                                           sc_provider_response *out)
{
    sc_plugin_provider_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_provider.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_PROVIDER, SC_PLUGIN_CAP_PROVIDER);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_provider_generate(adapter->inner, request, alloc, out);
}

static sc_status provider_adapter_stream(void *impl,
                                         const sc_provider_request *request,
                                         sc_allocator *alloc,
                                         sc_provider_stream_callback callback,
                                         void *callback_user_data)
{
    sc_plugin_provider_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_provider.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_PROVIDER, SC_PLUGIN_CAP_PROVIDER);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_provider_stream(adapter->inner, request, alloc, callback, callback_user_data);
}

static void provider_adapter_destroy(void *impl)
{
    sc_plugin_provider_adapter *adapter = impl;
    if (adapter == nullptr) {
        return;
    }
    sc_provider_destroy(adapter->inner);
    sc_plugin_release(adapter->plugin);
    sc_free(adapter->alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_provider_adapter));
}

static sc_status tool_adapter_spec(void *impl, sc_tool_spec *out)
{
    sc_plugin_tool_adapter *adapter = impl;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_tool.invalid_argument");
    }
    return sc_tool_spec_get(adapter->inner, out);
}

static sc_status tool_adapter_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    sc_plugin_tool_adapter *adapter = impl;
    sc_tool_spec spec = {0};
    sc_security_tool_request request = {0};
    bool approval_required = false;
    sc_status status;

    if (adapter == nullptr || adapter->inner == nullptr || call == nullptr || out == nullptr ||
        adapter->context.policy == nullptr) {
        return sc_status_invalid_argument("sc.plugin_tool.invalid_argument");
    }
    status = sc_tool_spec_get(adapter->inner, &spec);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    request = (sc_security_tool_request){
        .struct_size = sizeof(request),
        .tool_name = spec.name,
        .risk = spec.risk,
    };
    status = sc_security_validate_request(adapter->context.policy,
                                          adapter->context.estop,
                                          &request,
                                          &approval_required);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (approval_required) {
        return sc_status_cancelled("sc.plugin_tool.approval_required");
    }
    return sc_tool_invoke(adapter->inner, call, alloc, out);
}

static void tool_adapter_destroy(void *impl)
{
    sc_plugin_tool_adapter *adapter = impl;
    if (adapter == nullptr) {
        return;
    }
    sc_tool_destroy(adapter->inner);
    sc_plugin_release(adapter->plugin);
    sc_free(adapter->alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_tool_adapter));
}

static sc_status channel_adapter_send(void *impl, const sc_channel_message *message)
{
    sc_plugin_channel_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_channel.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_CHANNEL, SC_PLUGIN_CAP_CHANNEL);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_channel_send(adapter->inner, message);
}

static sc_status channel_adapter_listen(void *impl, sc_allocator *alloc, sc_channel_inbound *out)
{
    sc_plugin_channel_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_channel.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_CHANNEL, SC_PLUGIN_CAP_CHANNEL);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_channel_listen(adapter->inner, alloc, out);
}

static sc_status channel_adapter_health(void *impl, sc_allocator *alloc, sc_channel_health *out)
{
    sc_plugin_channel_adapter *adapter = impl;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_channel.invalid_argument");
    }
    return sc_channel_health_check(adapter->inner, alloc, out);
}

static sc_status channel_adapter_approval(void *impl,
                                          const sc_channel_approval_request *request,
                                          sc_allocator *alloc,
                                          sc_channel_approval_response *out)
{
    sc_plugin_channel_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_channel.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_CHANNEL, SC_PLUGIN_CAP_CHANNEL);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_channel_request_approval(adapter->inner, request, alloc, out);
}

static void channel_adapter_destroy(void *impl)
{
    sc_plugin_channel_adapter *adapter = impl;
    if (adapter == nullptr) {
        return;
    }
    sc_channel_destroy(adapter->inner);
    sc_plugin_release(adapter->plugin);
    sc_free(adapter->alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_channel_adapter));
}

static sc_status memory_adapter_put(void *impl, const sc_memory_record *record)
{
    sc_plugin_memory_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_memory.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_MEMORY, SC_PLUGIN_CAP_MEMORY);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_memory_put(adapter->inner, record);
}

static sc_status memory_adapter_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out)
{
    sc_plugin_memory_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_memory.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_MEMORY, SC_PLUGIN_CAP_MEMORY);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_memory_get(adapter->inner, namespace_name, key, alloc, out);
}

static sc_status memory_adapter_search(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_memory_result *out)
{
    sc_plugin_memory_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_memory.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_MEMORY, SC_PLUGIN_CAP_MEMORY);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_memory_search(adapter->inner, query, alloc, out);
}

static sc_status memory_adapter_forget(void *impl, sc_str namespace_name, sc_str key)
{
    sc_plugin_memory_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_memory.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_MEMORY, SC_PLUGIN_CAP_MEMORY);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_memory_forget(adapter->inner, namespace_name, key);
}

static sc_status memory_adapter_purge_namespace(void *impl, sc_str namespace_name)
{
    sc_plugin_memory_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_memory.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_MEMORY, SC_PLUGIN_CAP_MEMORY);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_memory_purge_namespace(adapter->inner, namespace_name);
}

static sc_status memory_adapter_purge_session(void *impl, sc_str namespace_name, sc_str session_id)
{
    sc_plugin_memory_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_memory.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_MEMORY, SC_PLUGIN_CAP_MEMORY);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_memory_purge_session(adapter->inner, namespace_name, session_id);
}

static sc_status memory_adapter_export_snapshot(void *impl, const sc_memory_query *query, sc_allocator *alloc, sc_string *out)
{
    sc_plugin_memory_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_memory.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_MEMORY, SC_PLUGIN_CAP_MEMORY);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_memory_export_snapshot(adapter->inner, query, alloc, out);
}

static sc_status memory_adapter_redact(void *impl, sc_str namespace_name, sc_str key)
{
    sc_plugin_memory_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_memory.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_MEMORY, SC_PLUGIN_CAP_MEMORY);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_memory_redact(adapter->inner, namespace_name, key);
}

static sc_status memory_adapter_export_snapshot_ex(void *impl,
                                                   const sc_memory_query *query,
                                                   const sc_memory_export_options *options,
                                                   sc_allocator *alloc,
                                                   sc_string *out)
{
    sc_plugin_memory_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_memory.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_MEMORY, SC_PLUGIN_CAP_MEMORY);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_memory_export_snapshot_ex(adapter->inner, query, options, alloc, out);
}

static void memory_adapter_destroy(void *impl)
{
    sc_plugin_memory_adapter *adapter = impl;
    if (adapter == nullptr) {
        return;
    }
    sc_memory_destroy(adapter->inner);
    sc_plugin_release(adapter->plugin);
    sc_free(adapter->alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_memory_adapter));
}

static sc_status observer_adapter_emit(void *impl, const sc_observer_event *event)
{
    sc_plugin_observer_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_observer.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_OBSERVER, SC_PLUGIN_CAP_OBSERVER);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_observer_emit(adapter->inner, event);
}

static sc_status observer_adapter_flush(void *impl)
{
    sc_plugin_observer_adapter *adapter = impl;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_observer.invalid_argument");
    }
    return sc_observer_flush(adapter->inner);
}

static void observer_adapter_destroy(void *impl)
{
    sc_plugin_observer_adapter *adapter = impl;
    if (adapter == nullptr) {
        return;
    }
    sc_observer_destroy(adapter->inner);
    sc_plugin_release(adapter->plugin);
    sc_free(adapter->alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_observer_adapter));
}

static sc_status peripheral_adapter_health(void *impl, sc_allocator *alloc, sc_peripheral_health *out)
{
    sc_plugin_peripheral_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_peripheral.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_PERIPHERAL, SC_PLUGIN_CAP_PERIPHERAL);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_peripheral_health_check(adapter->inner, alloc, out);
}

static sc_status peripheral_adapter_command(void *impl,
                                            const sc_peripheral_command *command,
                                            sc_allocator *alloc,
                                            sc_peripheral_result *out)
{
    sc_plugin_peripheral_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_peripheral.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_PERIPHERAL, SC_PLUGIN_CAP_PERIPHERAL);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_peripheral_command_send(adapter->inner, command, alloc, out);
}

static sc_status peripheral_adapter_describe_context(void *impl, sc_allocator *alloc, sc_peripheral_context *out)
{
    sc_plugin_peripheral_adapter *adapter = impl;
    sc_status status;
    if (adapter == nullptr || adapter->inner == nullptr) {
        return sc_status_invalid_argument("sc.plugin_peripheral.invalid_argument");
    }
    status = register_permission_check(adapter->plugin, SC_PLUGIN_PERMISSION_PERIPHERAL, SC_PLUGIN_CAP_PERIPHERAL);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_peripheral_describe_context(adapter->inner, alloc, out);
}

static void peripheral_adapter_destroy(void *impl)
{
    sc_plugin_peripheral_adapter *adapter = impl;
    if (adapter == nullptr) {
        return;
    }
    sc_peripheral_destroy(adapter->inner);
    sc_plugin_release(adapter->plugin);
    sc_free(adapter->alloc, adapter, sizeof(*adapter), _Alignof(sc_plugin_peripheral_adapter));
}
