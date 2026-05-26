#include "sc/plugin.h"
#include "sc/json.h"
#include "sc/version.h"
#include "test_helpers.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef SC_PLUGIN_MISSING_DESCRIPTOR_PATH
#define SC_PLUGIN_MISSING_DESCRIPTOR_PATH ""
#endif

#include "../fixtures/plugin_wasm_tool.inc"

typedef struct plugin_counts {
    int init_calls;
    int shutdown_calls;
    int tool_invokes;
    int channel_sends;
    int provider_generates;
    int observer_emits;
    int peripheral_commands;
    int destroys;
} plugin_counts;

typedef struct plugin_tool_state {
    sc_allocator *alloc;
} plugin_tool_state;

typedef struct plugin_channel_state {
    sc_allocator *alloc;
} plugin_channel_state;

typedef struct plugin_memory_state {
    sc_allocator *alloc;
} plugin_memory_state;

typedef struct plugin_provider_state {
    sc_allocator *alloc;
} plugin_provider_state;

typedef struct plugin_observer_state {
    sc_allocator *alloc;
} plugin_observer_state;

typedef struct plugin_peripheral_state {
    sc_allocator *alloc;
} plugin_peripheral_state;

static plugin_counts counts;

static int test_manifest_parse(void);
static int test_valid_plugin_and_wrapped_tool(void);
static int test_abi_and_permission_failures(void);
static int test_duplicate_registration(void);
static int test_extended_plugin_rules(void);
static int test_missing_symbol_loader(void);
static int test_wasm_tool_plugin(void);
static int test_python_tool_plugin(void);
static sc_status make_manifest(const char *json, sc_plugin_manifest *out);
static int write_temp_script(const char *suffix, const char *source, char *out, size_t out_len);
static void reset_counts(void);

static sc_status plugin_tool_spec(void *impl, sc_tool_spec *out);
static sc_status plugin_tool_invoke(void *impl,
                                     const sc_tool_call *call,
                                     sc_allocator *alloc,
                                     sc_tool_result *out);
static void plugin_tool_destroy(void *impl);
static sc_status plugin_tool_factory(sc_allocator *alloc, sc_tool **out);
static sc_status plugin_channel_send(void *impl, const sc_channel_message *message);
static void plugin_channel_destroy(void *impl);
static sc_status plugin_channel_factory(sc_allocator *alloc, sc_channel **out);
static sc_status plugin_memory_put(void *impl, const sc_memory_record *record);
static sc_status plugin_memory_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out);
static void plugin_memory_destroy(void *impl);
static sc_status plugin_memory_factory(sc_allocator *alloc, sc_memory **out);
static sc_status plugin_provider_generate(void *impl,
                                           const sc_provider_request *request,
                                           sc_allocator *alloc,
                                           sc_provider_response *out);
static void plugin_provider_destroy(void *impl);
static sc_status plugin_provider_factory(sc_allocator *alloc, sc_provider **out);
static sc_status plugin_observer_emit(void *impl, const sc_observer_event *event);
static void plugin_observer_destroy(void *impl);
static sc_status plugin_observer_factory(sc_allocator *alloc, sc_observer **out);
static sc_status plugin_peripheral_command(void *impl,
                                            const sc_peripheral_command *command,
                                            sc_allocator *alloc,
                                            sc_peripheral_result *out);
static void plugin_peripheral_destroy(void *impl);
static sc_status plugin_peripheral_factory(sc_allocator *alloc, sc_peripheral **out);
static sc_status valid_plugin_init(const sc_plugin_host_api *api, void **plugin_state);
static sc_status memory_plugin_init(const sc_plugin_host_api *api, void **plugin_state);
static sc_status provider_plugin_init(const sc_plugin_host_api *api, void **plugin_state);
static sc_status observer_peripheral_plugin_init(const sc_plugin_host_api *api, void **plugin_state);
static sc_status partial_fail_plugin_init(const sc_plugin_host_api *api, void **plugin_state);
static void plugin_release_memory(void *plugin_state, void *ptr, size_t len);
static void plugin_shutdown(void *plugin_state);

static const sc_tool_vtab plugin_tool_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "plugin-tool",
    .display_name = "Phase 16 tool",
    .feature_flag = "SC_ENABLE_PLUGINS",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = plugin_tool_spec,
    .invoke = plugin_tool_invoke,
    .destroy = plugin_tool_destroy,
};

static const sc_channel_vtab plugin_channel_vtab = {
    .struct_size = sizeof(sc_channel_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "plugin-channel",
    .display_name = "Phase 16 channel",
    .feature_flag = "SC_ENABLE_PLUGINS",
    .description_key = "channel.plugin.description",
    .config_schema_ref = "sc.schema.channels.plugin",
    .inbound_event_types = SC_CHANNEL_INBOUND_TEXT,
    .media_capabilities = SC_CHANNEL_MEDIA_NONE,
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .send = plugin_channel_send,
    .destroy = plugin_channel_destroy,
};

static const sc_memory_vtab plugin_memory_vtab = {
    .struct_size = sizeof(sc_memory_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "plugin-memory",
    .display_name = "Phase 16 memory",
    .feature_flag = "SC_ENABLE_PLUGINS",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .put = plugin_memory_put,
    .get = plugin_memory_get,
    .destroy = plugin_memory_destroy,
};

static const sc_provider_vtab plugin_provider_vtab = {
    .struct_size = sizeof(sc_provider_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "plugin-provider",
    .display_name = "Phase 16 provider",
    .feature_flag = "SC_ENABLE_PLUGINS",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .generate = plugin_provider_generate,
    .destroy = plugin_provider_destroy,
    .provider_modes = SC_PROVIDER_MODE_CHAT,
};

static const sc_observer_vtab plugin_observer_vtab = {
    .struct_size = sizeof(sc_observer_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "plugin-observer",
    .display_name = "Phase 16 observer",
    .feature_flag = "SC_ENABLE_PLUGINS",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .emit = plugin_observer_emit,
    .destroy = plugin_observer_destroy,
};

static const sc_peripheral_vtab plugin_peripheral_vtab = {
    .struct_size = sizeof(sc_peripheral_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "plugin-peripheral",
    .display_name = "Phase 16 peripheral",
    .feature_flag = "SC_ENABLE_PLUGINS",
    .capabilities = SC_CONTRACT_CAP_NONE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .command = plugin_peripheral_command,
    .destroy = plugin_peripheral_destroy,
};

static const sc_plugin_descriptor valid_descriptor = {
    .struct_size = sizeof(sc_plugin_descriptor),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .abi_minor = SC_ABI_VERSION_MINOR,
    .name = "plugin-plugin",
    .version = "1.0.0",
    .init = valid_plugin_init,
    .shutdown = plugin_shutdown,
    .metadata_json = "{\"kind\":\"test\"}",
    .requested_capabilities = SC_PLUGIN_CAP_TOOL | SC_PLUGIN_CAP_CHANNEL,
    .requested_permissions = SC_PLUGIN_PERMISSION_TOOL | SC_PLUGIN_PERMISSION_CHANNEL,
    .manifest_schema_ref = "sc.schema.plugins.plugin",
    .release_memory = plugin_release_memory,
};

static const sc_plugin_descriptor memory_descriptor = {
    .struct_size = sizeof(sc_plugin_descriptor),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .abi_minor = SC_ABI_VERSION_MINOR,
    .name = "plugin-memory-plugin",
    .version = "1.0.0",
    .init = memory_plugin_init,
    .shutdown = plugin_shutdown,
};

static const sc_plugin_descriptor provider_descriptor = {
    .struct_size = sizeof(sc_plugin_descriptor),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .abi_minor = SC_ABI_VERSION_MINOR,
    .name = "plugin-provider-plugin",
    .version = "1.0.0",
    .init = provider_plugin_init,
    .shutdown = plugin_shutdown,
    .requested_capabilities = SC_PLUGIN_CAP_PROVIDER,
    .requested_permissions = SC_PLUGIN_PERMISSION_PROVIDER,
};

static const sc_plugin_descriptor observer_peripheral_descriptor = {
    .struct_size = sizeof(sc_plugin_descriptor),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .abi_minor = SC_ABI_VERSION_MINOR,
    .name = "plugin-op-plugin",
    .version = "1.0.0",
    .init = observer_peripheral_plugin_init,
    .shutdown = plugin_shutdown,
    .requested_capabilities = SC_PLUGIN_CAP_OBSERVER | SC_PLUGIN_CAP_PERIPHERAL,
    .requested_permissions = SC_PLUGIN_PERMISSION_OBSERVER | SC_PLUGIN_PERMISSION_PERIPHERAL,
};

static const sc_plugin_descriptor partial_fail_descriptor = {
    .struct_size = sizeof(sc_plugin_descriptor),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .abi_minor = SC_ABI_VERSION_MINOR,
    .name = "plugin-partial-fail-plugin",
    .version = "1.0.0",
    .init = partial_fail_plugin_init,
    .shutdown = plugin_shutdown,
};

int main(void)
{
    int failures = 0;

    failures += test_manifest_parse();
    failures += test_valid_plugin_and_wrapped_tool();
    failures += test_abi_and_permission_failures();
    failures += test_duplicate_registration();
    failures += test_extended_plugin_rules();
    failures += test_missing_symbol_loader();
    failures += test_wasm_tool_plugin();
    failures += test_python_tool_plugin();

    return failures == 0 ? 0 : 1;
}

static int test_manifest_parse(void)
{
    int failures = 0;
    sc_plugin_manifest manifest = {0};

    failures += sc_test_expect_status("manifest parse",
                              make_manifest("{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"abi_minor\":1,"
                                            "\"capabilities\":[\"tool\",\"channel\"],"
                                            "\"requested_permissions\":[\"tool\",\"channel\",\"filesystem\"],"
                                            "\"entry_artifact\":\"self\","
                                            "\"signature_metadata\":\"unsigned-test\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_true("manifest name", strcmp(manifest.name.ptr, "plugin-plugin") == 0);
    failures += sc_test_expect_true("manifest tool cap", (manifest.capabilities & SC_PLUGIN_CAP_TOOL) != 0);
    failures += sc_test_expect_true("manifest fs permission",
                            (manifest.requested_permissions & SC_PLUGIN_PERMISSION_FILESYSTEM) != 0);
    sc_plugin_manifest_clear(&manifest);

    failures += sc_test_expect_status("script manifest",
                              make_manifest("{\"name\":\"script-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"python\",\"tool\"],"
                                            "\"requested_permissions\":[\"tool\"],"
                                            "\"entry_artifact\":\"script.py\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_true("manifest python cap", (manifest.capabilities & SC_PLUGIN_CAP_PYTHON) != 0);
    sc_plugin_manifest_clear(&manifest);

    failures += sc_test_expect_status("node capability rejected",
                              make_manifest("{\"name\":\"node-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"node\",\"tool\"],"
                                            "\"requested_permissions\":[\"tool\"],"
                                            "\"entry_artifact\":\"script.js\"}",
                                            &manifest),
                              SC_ERR_PARSE);

    failures += sc_test_expect_status("unknown permission rejected",
                              make_manifest("{\"name\":\"bad\",\"version\":\"1\",\"abi_major\":0,"
                                            "\"capabilities\":[],\"requested_permissions\":[\"root\"],"
                                            "\"entry_artifact\":\"x\"}",
                                            &manifest),
                              SC_ERR_PARSE);
    return failures;
}

static int test_valid_plugin_and_wrapped_tool(void)
{
    int failures = 0;
    sc_plugin_host *host = nullptr;
    sc_plugin *plugin = nullptr;
    sc_tool *tool = nullptr;
    sc_channel *channel = nullptr;
    sc_security_policy policy = {0};
    sc_tool_context tool_context = {0};
    sc_tool_result tool_result = {0};
    sc_tool_call call = {.struct_size = sizeof(call), .call_id = sc_str_from_cstr("call")};
    sc_channel_message message = {
        .struct_size = sizeof(message),
        .conversation_id = sc_str_from_cstr("c"),
        .text = sc_str_from_cstr("hello"),
    };
    sc_plugin_manifest manifest = {0};
    sc_plugin_host_options options = {
        .struct_size = sizeof(options),
        .allowed_permissions = SC_PLUGIN_PERMISSION_TOOL | SC_PLUGIN_PERMISSION_CHANNEL,
    };

    reset_counts();
    failures += sc_test_expect_status("valid manifest",
                              make_manifest("{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"abi_minor\":1,"
                                            "\"capabilities\":[\"tool\",\"channel\"],"
                                            "\"requested_permissions\":[\"tool\",\"channel\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_status("plugin host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("load valid plugin",
                              sc_plugin_load_descriptor(host, &valid_descriptor, &manifest, &plugin),
                              SC_OK);
    failures += sc_test_expect_true("registrations", sc_plugin_host_tool_count(host) == 1 && sc_plugin_host_channel_count(host) == 1);
    failures += sc_test_expect_true("init called", counts.init_calls == 1);

    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("deny plugin tool",
                              sc_security_policy_add_denied_tool(&policy, sc_str_from_cstr("plugin-tool")),
                              SC_OK);
    tool_context = (sc_tool_context){.struct_size = sizeof(tool_context), .policy = &policy};
    failures += sc_test_expect_status("create wrapped tool",
                              sc_plugin_host_create_tool(host,
                                                         sc_str_from_cstr("plugin-tool"),
                                                         &tool_context,
                                                         sc_allocator_heap(),
                                                         &tool),
                              SC_OK);
    failures += sc_test_expect_true("tool active ref", sc_plugin_active_references(plugin) == 1);
    failures += sc_test_expect_status("unload blocked by active ref", sc_plugin_unload(plugin), SC_ERR_CANCELLED);
    failures += sc_test_expect_status("wrapper denies before invoke",
                              sc_tool_invoke(tool, &call, sc_allocator_heap(), &tool_result),
                              SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_true("inner not invoked", counts.tool_invokes == 0);
    sc_tool_destroy(tool);
    tool = nullptr;
    failures += sc_test_expect_true("tool ref released", sc_plugin_active_references(plugin) == 0);

    sc_security_policy_clear(&policy);
    failures += sc_test_expect_status("policy defaults 2", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    tool_context.policy = &policy;
    failures += sc_test_expect_status("create wrapped tool 2",
                              sc_plugin_host_create_tool(host,
                                                         sc_str_from_cstr("plugin-tool"),
                                                         &tool_context,
                                                         sc_allocator_heap(),
                                                         &tool),
                              SC_OK);
    failures += sc_test_expect_status("wrapper allows invoke", sc_tool_invoke(tool, &call, sc_allocator_heap(), &tool_result), SC_OK);
    failures += sc_test_expect_true("inner invoked", counts.tool_invokes == 1 && strcmp(tool_result.output.ptr, "plugin output") == 0);
    sc_tool_result_clear(&tool_result);
    sc_tool_destroy(tool);
    tool = nullptr;

    failures += sc_test_expect_status("create wrapped channel",
                              sc_plugin_host_create_channel(host,
                                                           sc_str_from_cstr("plugin-channel"),
                                                           sc_allocator_heap(),
                                                           &channel),
                              SC_OK);
    failures += sc_test_expect_true("channel descriptor preserved",
                            sc_channel_vtab_of(channel) != nullptr &&
                                strcmp(sc_channel_vtab_of(channel)->name, "plugin-channel") == 0 &&
                                strcmp(sc_channel_vtab_of(channel)->description_key, "channel.plugin.description") == 0 &&
                                (sc_channel_vtab_of(channel)->inbound_event_types & SC_CHANNEL_INBOUND_TEXT) != 0);
    failures += sc_test_expect_status("channel send", sc_channel_send(channel, &message), SC_OK);
    failures += sc_test_expect_true("channel delegated", counts.channel_sends == 1);
    sc_channel_destroy(channel);
    channel = nullptr;

    failures += sc_test_expect_status("unload valid plugin", sc_plugin_unload(plugin), SC_OK);
    plugin = nullptr;
    failures += sc_test_expect_true("shutdown called", counts.shutdown_calls == 1);

    sc_security_policy_clear(&policy);
    sc_plugin_host_destroy(host);
    sc_plugin_manifest_clear(&manifest);
    return failures;
}

static int test_abi_and_permission_failures(void)
{
    int failures = 0;
    sc_plugin_host *host = nullptr;
    sc_plugin *plugin = nullptr;
    sc_plugin_manifest manifest = {0};
    sc_plugin_descriptor bad_descriptor = valid_descriptor;
    sc_plugin_host_options tool_only = {
        .struct_size = sizeof(tool_only),
        .allowed_permissions = SC_PLUGIN_PERMISSION_TOOL,
    };
    sc_plugin_host_options memory_allowed = {
        .struct_size = sizeof(memory_allowed),
        .allowed_permissions = SC_PLUGIN_PERMISSION_MEMORY,
        .allow_memory_backends = false,
    };

    failures += sc_test_expect_status("host tool only", sc_plugin_host_new(sc_allocator_heap(), &tool_only, &host), SC_OK);
    failures += sc_test_expect_status("permission manifest",
                              make_manifest("{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"tool\"],"
                                            "\"requested_permissions\":[\"tool\",\"network\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_status("requested permission denied",
                              sc_plugin_load_descriptor(host, &valid_descriptor, &manifest, &plugin),
                              SC_ERR_SECURITY_DENIED);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    host = nullptr;

    failures += sc_test_expect_status("host abi", sc_plugin_host_new(sc_allocator_heap(), &tool_only, &host), SC_OK);
    failures += sc_test_expect_status("abi manifest",
                              make_manifest("{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":1,\"capabilities\":[\"tool\"],"
                                            "\"requested_permissions\":[\"tool\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_status("abi mismatch",
                              sc_plugin_load_descriptor(host, &valid_descriptor, &manifest, &plugin),
                              SC_ERR_INVALID_ARGUMENT);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    host = nullptr;

    failures += sc_test_expect_status("host bad descriptor", sc_plugin_host_new(sc_allocator_heap(), &tool_only, &host), SC_OK);
    failures += sc_test_expect_status("descriptor manifest",
                              make_manifest("{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"tool\"],"
                                            "\"requested_permissions\":[\"tool\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    bad_descriptor.abi_major = SC_ABI_VERSION_MAJOR + 1u;
    failures += sc_test_expect_status("bad descriptor",
                              sc_plugin_load_descriptor(host, &bad_descriptor, &manifest, &plugin),
                              SC_ERR_INVALID_ARGUMENT);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    host = nullptr;

    failures += sc_test_expect_status("host memory", sc_plugin_host_new(sc_allocator_heap(), &memory_allowed, &host), SC_OK);
    failures += sc_test_expect_status("memory manifest",
                              make_manifest("{\"name\":\"plugin-memory-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"memory\"],"
                                            "\"requested_permissions\":[\"memory\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_status("memory backend denied",
                              sc_plugin_load_descriptor(host, &memory_descriptor, &manifest, &plugin),
                              SC_ERR_SECURITY_DENIED);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    return failures;
}

static int test_duplicate_registration(void)
{
    int failures = 0;
    sc_plugin_host *host = nullptr;
    sc_plugin *first = nullptr;
    sc_plugin *second = nullptr;
    sc_plugin_manifest manifest = {0};
    sc_plugin_host_options options = {
        .struct_size = sizeof(options),
        .allowed_permissions = SC_PLUGIN_PERMISSION_TOOL | SC_PLUGIN_PERMISSION_CHANNEL,
    };

    reset_counts();
    failures += sc_test_expect_status("duplicate host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("duplicate manifest",
                              make_manifest("{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"tool\",\"channel\"],"
                                            "\"requested_permissions\":[\"tool\",\"channel\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_status("first load", sc_plugin_load_descriptor(host, &valid_descriptor, &manifest, &first), SC_OK);
    failures += sc_test_expect_status("second load duplicate",
                              sc_plugin_load_descriptor(host, &valid_descriptor, &manifest, &second),
                              SC_ERR_INVALID_ARGUMENT);
    failures += sc_test_expect_true("only one registration", sc_plugin_host_tool_count(host) == 1);

    failures += sc_test_expect_status("unload first", sc_plugin_unload(first), SC_OK);
    sc_plugin_host_destroy(host);
    sc_plugin_manifest_clear(&manifest);
    return failures;
}

static int test_extended_plugin_rules(void)
{
    int failures = 0;
    sc_plugin_host *host = nullptr;
    sc_plugin *plugin = nullptr;
    sc_provider *provider = nullptr;
    sc_memory *memory = nullptr;
    sc_peripheral *peripheral = nullptr;
    sc_observer_list observers = {0};
    sc_provider_response provider_response = {0};
    sc_peripheral_result peripheral_result = {0};
    sc_plugin_manifest manifest = {0};
    sc_plugin_descriptor small_descriptor = valid_descriptor;
    sc_plugin_descriptor mismatched_descriptor = valid_descriptor;
    char python_path[256] = {0};
    char js_path[256] = {0};
    sc_plugin_host_options options = {
        .struct_size = sizeof(options),
        .allowed_permissions = SC_PLUGIN_PERMISSION_TOOL | SC_PLUGIN_PERMISSION_CHANNEL |
                               SC_PLUGIN_PERMISSION_PROVIDER | SC_PLUGIN_PERMISSION_MEMORY |
                               SC_PLUGIN_PERMISSION_OBSERVER | SC_PLUGIN_PERMISSION_PERIPHERAL,
        .allow_memory_backends = true,
    };

    reset_counts();
    failures += sc_test_expect_status("small descriptor host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("small descriptor manifest",
                              make_manifest("{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"tool\",\"channel\"],"
                                            "\"requested_permissions\":[\"tool\",\"channel\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    small_descriptor.struct_size = offsetof(sc_plugin_descriptor, shutdown) + sizeof(small_descriptor.shutdown);
    failures += sc_test_expect_status("small descriptor load", sc_plugin_load_descriptor(host, &small_descriptor, &manifest, &plugin), SC_OK);
    failures += sc_test_expect_status("small descriptor unload", sc_plugin_unload(plugin), SC_OK);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    host = nullptr;
    plugin = nullptr;

    failures += sc_test_expect_status("mismatch host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("mismatch manifest",
                              make_manifest("{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"tool\"],"
                                            "\"requested_permissions\":[\"tool\",\"channel\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    mismatched_descriptor.requested_capabilities = SC_PLUGIN_CAP_TOOL | SC_PLUGIN_CAP_CHANNEL;
    failures += sc_test_expect_status("descriptor capability mismatch",
                              sc_plugin_load_descriptor(host, &mismatched_descriptor, &manifest, &plugin),
                              SC_ERR_SECURITY_DENIED);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    host = nullptr;

    failures += sc_test_expect_status("partial host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("partial manifest",
                              make_manifest("{\"name\":\"plugin-partial-fail-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"tool\"],"
                                            "\"requested_permissions\":[\"tool\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_status("partial init fails",
                              sc_plugin_load_descriptor(host, &partial_fail_descriptor, &manifest, &plugin),
                              SC_ERR_IO);
    failures += sc_test_expect_true("partial registration removed", sc_plugin_host_tool_count(host) == 0);
    failures += sc_test_expect_true("partial shutdown called", counts.shutdown_calls >= 1);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    host = nullptr;

    failures += sc_test_expect_status("provider host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("provider manifest",
                              make_manifest("{\"name\":\"plugin-provider-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"provider\"],"
                                            "\"requested_permissions\":[\"provider\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_status("provider load", sc_plugin_load_descriptor(host, &provider_descriptor, &manifest, &plugin), SC_OK);
    failures += sc_test_expect_status("create provider",
                              sc_plugin_host_create_provider(host, sc_str_from_cstr("plugin-provider"), sc_allocator_heap(), &provider),
                              SC_OK);
    failures += sc_test_expect_true("provider active ref", sc_plugin_active_references(plugin) == 1);
    failures += sc_test_expect_status("provider unload blocked", sc_plugin_unload(plugin), SC_ERR_CANCELLED);
    failures += sc_test_expect_status("provider generate",
                              sc_provider_generate(provider,
                                                   &(sc_provider_request){.struct_size = sizeof(sc_provider_request),
                                                                          .prompt = sc_str_from_cstr("hi")},
                                                   sc_allocator_heap(),
                                                   &provider_response),
                              SC_OK);
    failures += sc_test_expect_true("provider output", strcmp(provider_response.text.ptr, "plugin provider output") == 0);
    sc_provider_response_clear(&provider_response);
    sc_provider_destroy(provider);
    provider = nullptr;
    failures += sc_test_expect_true("provider ref released", sc_plugin_active_references(plugin) == 0);
    failures += sc_test_expect_status("provider unload", sc_plugin_unload(plugin), SC_OK);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    host = nullptr;
    plugin = nullptr;

    failures += sc_test_expect_status("memory host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("memory manifest 2",
                              make_manifest("{\"name\":\"plugin-memory-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"memory\"],"
                                            "\"requested_permissions\":[\"memory\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_status("memory load", sc_plugin_load_descriptor(host, &memory_descriptor, &manifest, &plugin), SC_OK);
    failures += sc_test_expect_status("create memory",
                              sc_plugin_host_create_memory(host, sc_str_from_cstr("plugin-memory"), sc_allocator_heap(), &memory),
                              SC_OK);
    failures += sc_test_expect_true("memory active ref", sc_plugin_active_references(plugin) == 1);
    failures += sc_test_expect_status("memory unload blocked", sc_plugin_unload(plugin), SC_ERR_CANCELLED);
    failures += sc_test_expect_status("memory put",
                              sc_memory_put(memory,
                                            &(sc_memory_record){.struct_size = sizeof(sc_memory_record),
                                                               .namespace_name = sc_str_from_cstr("n"),
                                                               .key = sc_str_from_cstr("k"),
                                                               .value = sc_str_from_cstr("v")}),
                              SC_OK);
    sc_memory_destroy(memory);
    memory = nullptr;
    failures += sc_test_expect_true("memory ref released", sc_plugin_active_references(plugin) == 0);
    failures += sc_test_expect_status("memory unload", sc_plugin_unload(plugin), SC_OK);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    host = nullptr;
    plugin = nullptr;

    sc_observer_list_init(&observers, sc_allocator_heap());
    options.observers = &observers;
    failures += sc_test_expect_status("observer peripheral host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("observer peripheral manifest",
                              make_manifest("{\"name\":\"plugin-op-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"observer\",\"peripheral\"],"
                                            "\"requested_permissions\":[\"observer\",\"peripheral\"],"
                                            "\"entry_artifact\":\"self\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_status("observer peripheral load",
                              sc_plugin_load_descriptor(host, &observer_peripheral_descriptor, &manifest, &plugin),
                              SC_OK);
    failures += sc_test_expect_true("observer registered", sc_observer_list_len(&observers) == 1);
    failures += sc_test_expect_status("observer emit",
                              sc_observer_list_emit(&observers,
                                                   &(sc_observer_event){.struct_size = sizeof(sc_observer_event),
                                                                        .target = sc_str_from_cstr("test"),
                                                                        .name = sc_str_from_cstr("plugin.test")}),
                              SC_OK);
    failures += sc_test_expect_true("observer delegated", counts.observer_emits == 1);
    failures += sc_test_expect_status("create peripheral",
                              sc_plugin_host_create_peripheral(host,
                                                               sc_str_from_cstr("plugin-peripheral"),
                                                               sc_allocator_heap(),
                                                               &peripheral),
                              SC_OK);
    failures += sc_test_expect_status("peripheral command",
                              sc_peripheral_command_send(peripheral,
                                                         &(sc_peripheral_command){.struct_size = sizeof(sc_peripheral_command),
                                                                                  .operation = sc_str_from_cstr("ping")},
                                                         sc_allocator_heap(),
                                                         &peripheral_result),
                              SC_OK);
    failures += sc_test_expect_true("peripheral delegated", counts.peripheral_commands == 1);
    sc_peripheral_result_clear(&peripheral_result);
    sc_peripheral_destroy(peripheral);
    peripheral = nullptr;
    sc_observer_list_clear(&observers);
    failures += sc_test_expect_true("observer/peripheral refs released", sc_plugin_active_references(plugin) == 0);
    failures += sc_test_expect_status("observer peripheral unload", sc_plugin_unload(plugin), SC_OK);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    host = nullptr;
    plugin = nullptr;

    failures += sc_test_expect_status("wasm manifest",
                              make_manifest("{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"wasm\"],"
                                            "\"requested_permissions\":[],"
                                            "\"entry_artifact\":\"" SC_PLUGIN_MISSING_DESCRIPTOR_PATH "\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_status("wasm host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("wasm disabled", sc_plugin_load_path(host, &manifest, &plugin), SC_ERR_SECURITY_DENIED);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    host = nullptr;

    {
        sc_str root = sc_str_from_cstr("/tmp");
        sc_str extension = sc_str_from_cstr(".so");
        options.plugin_roots = &root;
        options.plugin_root_count = 1;
        options.allowed_extensions = &extension;
        options.allowed_extension_count = 1;
        failures += sc_test_expect_status("path manifest",
                                  make_manifest("{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                                                "\"abi_major\":0,\"capabilities\":[\"tool\"],"
                                                "\"requested_permissions\":[\"tool\"],"
                                                "\"entry_artifact\":\"" SC_PLUGIN_MISSING_DESCRIPTOR_PATH "\"}",
                                                &manifest),
                                  SC_OK);
        failures += sc_test_expect_status("path host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
        failures += sc_test_expect_status("path outside root", sc_plugin_load_path(host, &manifest, &plugin), SC_ERR_SECURITY_DENIED);
        sc_plugin_manifest_clear(&manifest);
        sc_plugin_host_destroy(host);
    }

    options.plugin_roots = nullptr;
    options.plugin_root_count = 0;
    options.allowed_extensions = nullptr;
    options.allowed_extension_count = 0;
    failures += sc_test_expect_status("bad extension manifest",
                              make_manifest("{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                                            "\"abi_major\":0,\"capabilities\":[\"tool\"],"
                                            "\"requested_permissions\":[\"tool\"],"
                                            "\"entry_artifact\":\"/tmp/plugin-plugin.txt\"}",
                                            &manifest),
                              SC_OK);
    failures += sc_test_expect_status("bad extension host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("extension denied", sc_plugin_load_path(host, &manifest, &plugin), SC_ERR_SECURITY_DENIED);
    sc_plugin_manifest_clear(&manifest);
    sc_plugin_host_destroy(host);
    host = nullptr;

    failures += write_temp_script(".py", "def sc_plugin_init(host):\n    pass\n", python_path, sizeof(python_path));
    failures += write_temp_script(".js", "exports.init = function(host) {}\n", js_path, sizeof(js_path));
    if (python_path[0] != '\0') {
        char json[512] = {0};
        int written = snprintf(json,
                               sizeof(json),
                               "{\"name\":\"plugin-python-disabled\",\"version\":\"1.0.0\","
                               "\"abi_major\":0,\"capabilities\":[\"python\",\"tool\"],"
                               "\"requested_permissions\":[\"tool\"],"
                               "\"entry_artifact\":\"%s\"}",
                               python_path);
        failures += sc_test_expect_true("python disabled json", written > 0 && (size_t)written < sizeof(json));
        failures += sc_test_expect_status("python disabled manifest", make_manifest(json, &manifest), SC_OK);
        failures += sc_test_expect_status("python disabled host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
        failures += sc_test_expect_status("python disabled", sc_plugin_load_path(host, &manifest, &plugin), SC_ERR_SECURITY_DENIED);
        sc_plugin_manifest_clear(&manifest);
        sc_plugin_host_destroy(host);
        host = nullptr;
    }
    if (js_path[0] != '\0') {
        char json[512] = {0};
        int written = snprintf(json,
                               sizeof(json),
                               "{\"name\":\"plugin-js-denied\",\"version\":\"1.0.0\","
                               "\"abi_major\":0,\"capabilities\":[\"tool\"],"
                               "\"requested_permissions\":[\"tool\"],"
                               "\"entry_artifact\":\"%s\"}",
                               js_path);
        failures += sc_test_expect_true("js denied json", written > 0 && (size_t)written < sizeof(json));
        failures += sc_test_expect_status("js denied manifest", make_manifest(json, &manifest), SC_OK);
        failures += sc_test_expect_status("js denied host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
        failures += sc_test_expect_status("js extension denied", sc_plugin_load_path(host, &manifest, &plugin), SC_ERR_SECURITY_DENIED);
        sc_plugin_manifest_clear(&manifest);
        sc_plugin_host_destroy(host);
        host = nullptr;
    }
    (void)unlink(python_path);
    (void)unlink(js_path);

    reset_counts();
    sc_plugin_release_memory(plugin, nullptr, 0);
    valid_descriptor.release_memory((void *)&counts, (void *)&failures, 1);
    failures += sc_test_expect_true("release callback counted", counts.destroys == 1);
    return failures;
}

static int test_missing_symbol_loader(void)
{
    int failures = 0;
    sc_plugin_host *host = nullptr;
    sc_plugin *plugin = nullptr;
    sc_plugin_manifest manifest = {0};
    sc_plugin_host_options options = {
        .struct_size = sizeof(options),
        .allowed_permissions = SC_PLUGIN_PERMISSION_TOOL,
    };
    char json[2048] = {0};
    int written = 0;

    if (strlen(SC_PLUGIN_MISSING_DESCRIPTOR_PATH) == 0) {
        return 0;
    }

    written = snprintf(json,
                       sizeof(json),
                       "{\"name\":\"plugin-plugin\",\"version\":\"1.0.0\","
                       "\"abi_major\":0,\"capabilities\":[\"tool\"],"
                       "\"requested_permissions\":[\"tool\"],"
                       "\"entry_artifact\":\"%s\"}",
                       SC_PLUGIN_MISSING_DESCRIPTOR_PATH);
    failures += sc_test_expect_true("missing path json", written > 0 && (size_t)written < sizeof(json));
    failures += sc_test_expect_status("missing symbol manifest", make_manifest(json, &manifest), SC_OK);
    failures += sc_test_expect_status("missing symbol host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("missing descriptor symbol", sc_plugin_load_path(host, &manifest, &plugin), SC_ERR_INVALID_ARGUMENT);
    sc_plugin_host_destroy(host);
    sc_plugin_manifest_clear(&manifest);
    return failures;
}

static int test_wasm_tool_plugin(void)
{
#if SC_ENABLE_WASM_PLUGINS
    int failures = 0;
    sc_plugin_host *host = nullptr;
    sc_plugin *plugin = nullptr;
    sc_tool *tool = nullptr;
    sc_security_policy policy = {0};
    sc_tool_context tool_context = {0};
    sc_tool_result tool_result = {0};
    sc_json_value *args = nullptr;
    sc_json_parse_error parse_error = {0};
    sc_tool_call call = {.struct_size = sizeof(call), .call_id = sc_str_from_cstr("wasm-call")};
    sc_plugin_manifest manifest = {0};
    sc_plugin_host_options options = {
        .struct_size = sizeof(options),
        .allowed_permissions = SC_PLUGIN_PERMISSION_TOOL,
        .wasm_enabled = true,
    };
    char temp_path[] = "/tmp/smolclaw-plugin-wasm-XXXXXX";
    char path[sizeof(temp_path) + 5] = {0};
    char json[2048] = {0};
    int fd = mkstemp(temp_path);
    int written = 0;

    failures += sc_test_expect_true("wasm temp", fd >= 0);
    if (fd < 0) {
        return failures;
    }
    failures += sc_test_expect_true("wasm write",
                            write(fd, plugin_wasm_tool_wasm, plugin_wasm_tool_wasm_len) ==
                                (ssize_t)plugin_wasm_tool_wasm_len);
    (void)close(fd);
    written = snprintf(path, sizeof(path), "%s.wasm", temp_path);
    failures += sc_test_expect_true("wasm path suffix", written > 0 && (size_t)written < sizeof(path));
    failures += sc_test_expect_true("wasm rename", rename(temp_path, path) == 0);

    written = snprintf(json,
                       sizeof(json),
                       "{\"name\":\"plugin-wasm-plugin\",\"version\":\"1.0.0\","
                       "\"abi_major\":0,\"capabilities\":[\"wasm\",\"tool\"],"
                       "\"requested_permissions\":[\"tool\"],"
                       "\"entry_artifact\":\"%s\"}",
                       path);
    failures += sc_test_expect_true("wasm manifest json", written > 0 && (size_t)written < sizeof(json));
    failures += sc_test_expect_status("wasm manifest", make_manifest(json, &manifest), SC_OK);
    failures += sc_test_expect_status("wasm host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("wasm load", sc_plugin_load_path(host, &manifest, &plugin), SC_OK);
    failures += sc_test_expect_true("wasm tool registered", sc_plugin_host_tool_count(host) == 1);
    failures += sc_test_expect_status("wasm policy", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    tool_context = (sc_tool_context){.struct_size = sizeof(tool_context), .policy = &policy};
    failures += sc_test_expect_status("wasm create tool",
                              sc_plugin_host_create_tool(host,
                                                         sc_str_from_cstr("plugin-wasm-tool"),
                                                         &tool_context,
                                                         sc_allocator_heap(),
                                                         &tool),
                              SC_OK);
    failures += sc_test_expect_status("wasm args",
                              sc_json_parse(sc_allocator_heap(),
                                            sc_str_from_cstr("{\"message\":\"hello\"}"),
                                            &args,
                                            &parse_error),
                              SC_OK);
    call.args = args;
    failures += sc_test_expect_true("wasm active ref", sc_plugin_active_references(plugin) == 1);
    failures += sc_test_expect_status("wasm unload blocked", sc_plugin_unload(plugin), SC_ERR_CANCELLED);
    failures += sc_test_expect_status("wasm invoke", sc_tool_invoke(tool, &call, sc_allocator_heap(), &tool_result), SC_OK);
    failures += sc_test_expect_true("wasm output", tool_result.success && strcmp(tool_result.output.ptr, "wasm-ok") == 0);
    sc_tool_result_clear(&tool_result);
    sc_tool_destroy(tool);
    tool = nullptr;
    failures += sc_test_expect_true("wasm ref released", sc_plugin_active_references(plugin) == 0);
    failures += sc_test_expect_status("wasm unload", sc_plugin_unload(plugin), SC_OK);
    plugin = nullptr;
    sc_security_policy_clear(&policy);
    sc_plugin_host_destroy(host);
    sc_json_destroy(args);
    sc_plugin_manifest_clear(&manifest);
    (void)unlink(path);
    (void)unlink(temp_path);
    return failures;
#else
    (void)plugin_wasm_tool_wasm;
    (void)plugin_wasm_tool_wasm_len;
    return 0;
#endif
}

static int test_python_tool_plugin(void)
{
#if SC_ENABLE_PYTHON_PLUGINS
    static const char source[] =
        "def sc_plugin_init(host):\n"
        "    host.register_tool({\n"
        "        \"name\": \"plugin-python-echo\",\n"
        "        \"description\": \"Phase 16 Python echo\",\n"
        "        \"input_schema\": {\"type\": \"object\"},\n"
        "        \"risk\": 0,\n"
        "        \"capability_category\": 0,\n"
        "        \"side_effect\": 0,\n"
        "        \"default_autonomy\": 0\n"
        "    }, invoke)\n"
        "\n"
        "def invoke(args, call):\n"
        "    if args.get(\"fail\"):\n"
        "        return {\"success\": False, \"output\": \"python-failed\"}\n"
        "    return {\"success\": True, \"output\": \"py:\" + args.get(\"message\", \"\") + \":\" + call.get(\"call_id\", \"\")}\n"
        "\n"
        "def sc_plugin_shutdown():\n"
        "    pass\n";
    int failures = 0;
    sc_plugin_host *host = nullptr;
    sc_plugin *plugin = nullptr;
    sc_tool *tool = nullptr;
    sc_security_policy policy = {0};
    sc_tool_context tool_context = {0};
    sc_tool_result tool_result = {0};
    sc_json_value *args = nullptr;
    sc_json_parse_error parse_error = {0};
    sc_tool_call call = {.struct_size = sizeof(call), .call_id = sc_str_from_cstr("python-call")};
    sc_plugin_manifest manifest = {0};
    sc_plugin_host_options options = {
        .struct_size = sizeof(options),
        .allowed_permissions = SC_PLUGIN_PERMISSION_TOOL,
        .python_enabled = true,
    };
    char path[256] = {0};
    char json[512] = {0};
    int written = 0;

    failures += write_temp_script(".py", source, path, sizeof(path));
    if (path[0] == '\0') {
        return failures;
    }
    written = snprintf(json,
                       sizeof(json),
                       "{\"name\":\"plugin-python-plugin\",\"version\":\"1.0.0\","
                       "\"abi_major\":0,\"capabilities\":[\"python\",\"tool\"],"
                       "\"requested_permissions\":[\"tool\"],"
                       "\"entry_artifact\":\"%s\"}",
                       path);
    failures += sc_test_expect_true("python manifest json", written > 0 && (size_t)written < sizeof(json));
    failures += sc_test_expect_status("python manifest", make_manifest(json, &manifest), SC_OK);
    failures += sc_test_expect_status("python host", sc_plugin_host_new(sc_allocator_heap(), &options, &host), SC_OK);
    failures += sc_test_expect_status("python load", sc_plugin_load_path(host, &manifest, &plugin), SC_OK);
    failures += sc_test_expect_true("python tool registered", sc_plugin_host_tool_count(host) == 1);
    failures += sc_test_expect_status("python policy", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    tool_context = (sc_tool_context){.struct_size = sizeof(tool_context), .policy = &policy};
    failures += sc_test_expect_status("python create tool",
                              sc_plugin_host_create_tool(host,
                                                         sc_str_from_cstr("plugin-python-echo"),
                                                         &tool_context,
                                                         sc_allocator_heap(),
                                                         &tool),
                              SC_OK);
    failures += sc_test_expect_status("python args",
                              sc_json_parse(sc_allocator_heap(),
                                            sc_str_from_cstr("{\"message\":\"hello\"}"),
                                            &args,
                                            &parse_error),
                              SC_OK);
    call.args = args;
    failures += sc_test_expect_true("python active ref", sc_plugin_active_references(plugin) == 1);
    failures += sc_test_expect_status("python unload blocked", sc_plugin_unload(plugin), SC_ERR_CANCELLED);
    failures += sc_test_expect_status("python invoke", sc_tool_invoke(tool, &call, sc_allocator_heap(), &tool_result), SC_OK);
    failures += sc_test_expect_true("python output", tool_result.success && strcmp(tool_result.output.ptr, "py:hello:python-call") == 0);
    sc_tool_result_clear(&tool_result);
    sc_tool_destroy(tool);
    tool = nullptr;
    failures += sc_test_expect_true("python ref released", sc_plugin_active_references(plugin) == 0);
    failures += sc_test_expect_status("python unload", sc_plugin_unload(plugin), SC_OK);
    sc_security_policy_clear(&policy);
    sc_plugin_host_destroy(host);
    sc_json_destroy(args);
    sc_plugin_manifest_clear(&manifest);
    (void)unlink(path);
    return failures;
#else
    return 0;
#endif
}

static sc_status make_manifest(const char *json, sc_plugin_manifest *out)
{
    return sc_plugin_manifest_parse(sc_allocator_heap(), sc_str_from_cstr(json), out);
}

static int write_temp_script(const char *suffix, const char *source, char *out, size_t out_len)
{
    char temp_path[] = "/tmp/smolclaw-plugin-script-XXXXXX";
    int failures = 0;
    int fd = mkstemp(temp_path);
    int written = 0;

    if (out != nullptr && out_len > 0) {
        out[0] = '\0';
    }
    failures += sc_test_expect_true("script temp", fd >= 0);
    if (fd < 0) {
        return failures;
    }
    failures += sc_test_expect_true("script write", write(fd, source, strlen(source)) == (ssize_t)strlen(source));
    (void)close(fd);
    if (out == nullptr || suffix == nullptr || out_len == 0) {
        (void)unlink(temp_path);
        return failures + 1;
    }
    written = snprintf(out, out_len, "%s%s", temp_path, suffix);
    failures += sc_test_expect_true("script suffix", written > 0 && (size_t)written < out_len);
    if (written <= 0 || (size_t)written >= out_len) {
        (void)unlink(temp_path);
        out[0] = '\0';
        return failures;
    }
    failures += sc_test_expect_true("script rename", rename(temp_path, out) == 0);
    if (failures != 0) {
        (void)unlink(temp_path);
        (void)unlink(out);
        out[0] = '\0';
    }
    return failures;
}

static void reset_counts(void)
{
    counts = (plugin_counts){0};
}

static sc_status plugin_tool_spec(void *impl, sc_tool_spec *out)
{
    (void)impl;
    if (out == nullptr) {
        return sc_status_invalid_argument("plugin.spec.invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = sc_str_from_cstr("plugin-tool"),
        .description = sc_str_from_cstr("Phase 16 plugin tool"),
        .input_schema = nullptr,
        .capabilities = SC_CONTRACT_CAP_NONE,
        .risk = SC_TOOL_RISK_READONLY,
    };
    return sc_status_ok();
}

static sc_status plugin_tool_invoke(void *impl,
                                     const sc_tool_call *call,
                                     sc_allocator *alloc,
                                     sc_tool_result *out)
{
    (void)impl;
    (void)call;
    counts.tool_invokes += 1;
    *out = (sc_tool_result){.struct_size = sizeof(*out), .success = true};
    return sc_string_from_cstr(alloc, "plugin output", &out->output);
}

static void plugin_tool_destroy(void *impl)
{
    plugin_tool_state *state = impl;
    if (state == nullptr) {
        return;
    }
    counts.destroys += 1;
    sc_free(state->alloc, state, sizeof(*state), _Alignof(plugin_tool_state));
}

static sc_status plugin_tool_factory(sc_allocator *alloc, sc_tool **out)
{
    plugin_tool_state *state = nullptr;
    sc_status status;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(plugin_tool_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (plugin_tool_state){.alloc = alloc};
    status = sc_tool_new(alloc, &plugin_tool_vtab, state, out);
    if (!sc_status_is_ok(status)) {
        sc_free(alloc, state, sizeof(*state), _Alignof(plugin_tool_state));
    }
    return status;
}

static sc_status plugin_channel_send(void *impl, const sc_channel_message *message)
{
    (void)impl;
    (void)message;
    counts.channel_sends += 1;
    return sc_status_ok();
}

static void plugin_channel_destroy(void *impl)
{
    plugin_channel_state *state = impl;
    if (state == nullptr) {
        return;
    }
    counts.destroys += 1;
    sc_free(state->alloc, state, sizeof(*state), _Alignof(plugin_channel_state));
}

static sc_status plugin_channel_factory(sc_allocator *alloc, sc_channel **out)
{
    plugin_channel_state *state = nullptr;
    sc_status status;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(plugin_channel_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (plugin_channel_state){.alloc = alloc};
    status = sc_channel_new(alloc, &plugin_channel_vtab, state, out);
    if (!sc_status_is_ok(status)) {
        sc_free(alloc, state, sizeof(*state), _Alignof(plugin_channel_state));
    }
    return status;
}

static sc_status plugin_memory_put(void *impl, const sc_memory_record *record)
{
    (void)impl;
    (void)record;
    return sc_status_ok();
}

static sc_status plugin_memory_get(void *impl, sc_str namespace_name, sc_str key, sc_allocator *alloc, sc_string *out)
{
    (void)impl;
    (void)namespace_name;
    (void)key;
    return sc_string_from_cstr(alloc, "memory", out);
}

static void plugin_memory_destroy(void *impl)
{
    plugin_memory_state *state = impl;
    if (state == nullptr) {
        return;
    }
    counts.destroys += 1;
    sc_free(state->alloc, state, sizeof(*state), _Alignof(plugin_memory_state));
}

static sc_status plugin_memory_factory(sc_allocator *alloc, sc_memory **out)
{
    plugin_memory_state *state = nullptr;
    sc_status status;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(plugin_memory_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (plugin_memory_state){.alloc = alloc};
    status = sc_memory_new(alloc, &plugin_memory_vtab, state, out);
    if (!sc_status_is_ok(status)) {
        sc_free(alloc, state, sizeof(*state), _Alignof(plugin_memory_state));
    }
    return status;
}

static sc_status plugin_provider_generate(void *impl,
                                           const sc_provider_request *request,
                                           sc_allocator *alloc,
                                           sc_provider_response *out)
{
    (void)impl;
    (void)request;
    counts.provider_generates += 1;
    *out = (sc_provider_response){.struct_size = sizeof(*out)};
    return sc_string_from_cstr(alloc, "plugin provider output", &out->text);
}

static void plugin_provider_destroy(void *impl)
{
    plugin_provider_state *state = impl;
    if (state == nullptr) {
        return;
    }
    counts.destroys += 1;
    sc_free(state->alloc, state, sizeof(*state), _Alignof(plugin_provider_state));
}

static sc_status plugin_provider_factory(sc_allocator *alloc, sc_provider **out)
{
    plugin_provider_state *state = nullptr;
    sc_status status;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(plugin_provider_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (plugin_provider_state){.alloc = alloc};
    status = sc_provider_new(alloc, &plugin_provider_vtab, state, out);
    if (!sc_status_is_ok(status)) {
        sc_free(alloc, state, sizeof(*state), _Alignof(plugin_provider_state));
    }
    return status;
}

static sc_status plugin_observer_emit(void *impl, const sc_observer_event *event)
{
    (void)impl;
    (void)event;
    counts.observer_emits += 1;
    return sc_status_ok();
}

static void plugin_observer_destroy(void *impl)
{
    plugin_observer_state *state = impl;
    if (state == nullptr) {
        return;
    }
    counts.destroys += 1;
    sc_free(state->alloc, state, sizeof(*state), _Alignof(plugin_observer_state));
}

static sc_status plugin_observer_factory(sc_allocator *alloc, sc_observer **out)
{
    plugin_observer_state *state = nullptr;
    sc_status status;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(plugin_observer_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (plugin_observer_state){.alloc = alloc};
    status = sc_observer_new(alloc, &plugin_observer_vtab, state, out);
    if (!sc_status_is_ok(status)) {
        sc_free(alloc, state, sizeof(*state), _Alignof(plugin_observer_state));
    }
    return status;
}

static sc_status plugin_peripheral_command(void *impl,
                                            const sc_peripheral_command *command,
                                            sc_allocator *alloc,
                                            sc_peripheral_result *out)
{
    (void)impl;
    (void)command;
    counts.peripheral_commands += 1;
    *out = (sc_peripheral_result){.struct_size = sizeof(*out)};
    return sc_bytes_from_buf(alloc, sc_buf_from_parts("ok", 2), &out->payload);
}

static void plugin_peripheral_destroy(void *impl)
{
    plugin_peripheral_state *state = impl;
    if (state == nullptr) {
        return;
    }
    counts.destroys += 1;
    sc_free(state->alloc, state, sizeof(*state), _Alignof(plugin_peripheral_state));
}

static sc_status plugin_peripheral_factory(sc_allocator *alloc, sc_peripheral **out)
{
    plugin_peripheral_state *state = nullptr;
    sc_status status;

    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    state = sc_alloc(alloc, sizeof(*state), _Alignof(plugin_peripheral_state));
    if (state == nullptr) {
        return sc_status_no_memory();
    }
    *state = (plugin_peripheral_state){.alloc = alloc};
    status = sc_peripheral_new(alloc, &plugin_peripheral_vtab, state, out);
    if (!sc_status_is_ok(status)) {
        sc_free(alloc, state, sizeof(*state), _Alignof(plugin_peripheral_state));
    }
    return status;
}

static sc_status valid_plugin_init(const sc_plugin_host_api *api, void **plugin_state)
{
    sc_status status;

    counts.init_calls += 1;
    if (plugin_state != nullptr) {
        *plugin_state = &counts;
    }
    if (api == nullptr || api->register_tool == nullptr || api->register_channel == nullptr) {
        return sc_status_invalid_argument("plugin.init.invalid_api");
    }
    status = api->register_tool(api, &plugin_tool_vtab, plugin_tool_factory);
    if (sc_status_is_ok(status)) {
        status = api->register_channel(api, &plugin_channel_vtab, plugin_channel_factory);
    }
    return status;
}

static sc_status memory_plugin_init(const sc_plugin_host_api *api, void **plugin_state)
{
    if (plugin_state != nullptr) {
        *plugin_state = &counts;
    }
    if (api == nullptr || api->register_memory == nullptr) {
        return sc_status_invalid_argument("plugin.memory.invalid_api");
    }
    return api->register_memory(api, &plugin_memory_vtab, plugin_memory_factory);
}

static sc_status provider_plugin_init(const sc_plugin_host_api *api, void **plugin_state)
{
    if (plugin_state != nullptr) {
        *plugin_state = &counts;
    }
    if (api == nullptr || api->register_provider == nullptr) {
        return sc_status_invalid_argument("plugin.provider.invalid_api");
    }
    return api->register_provider(api, &plugin_provider_vtab, plugin_provider_factory);
}

static sc_status observer_peripheral_plugin_init(const sc_plugin_host_api *api, void **plugin_state)
{
    sc_status status;
    if (plugin_state != nullptr) {
        *plugin_state = &counts;
    }
    if (api == nullptr || api->register_observer == nullptr || api->register_peripheral == nullptr) {
        return sc_status_invalid_argument("plugin.observer_peripheral.invalid_api");
    }
    status = api->register_observer(api, &plugin_observer_vtab, plugin_observer_factory);
    if (sc_status_is_ok(status)) {
        status = api->register_peripheral(api, &plugin_peripheral_vtab, plugin_peripheral_factory);
    }
    return status;
}

static sc_status partial_fail_plugin_init(const sc_plugin_host_api *api, void **plugin_state)
{
    sc_status status;
    if (plugin_state != nullptr) {
        *plugin_state = &counts;
    }
    if (api == nullptr || api->register_tool == nullptr) {
        return sc_status_invalid_argument("plugin.partial.invalid_api");
    }
    status = api->register_tool(api, &plugin_tool_vtab, plugin_tool_factory);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return sc_status_io("plugin.partial.failed");
}

// cppcheck-suppress constParameterCallback
static void plugin_release_memory(void *plugin_state, void *ptr, size_t len)
{
    (void)ptr;
    (void)len;
    if (plugin_state != nullptr) {
        counts.destroys += 1;
    }
}

static void plugin_shutdown(void *plugin_state)
{
    (void)plugin_state;
    counts.shutdown_calls += 1;
}
