#pragma once

#include "sc/plugin.h"

SC_BEGIN_DECLS

typedef struct sc_script_runtime_vtab {
    size_t struct_size;
    const char *feature_flag;
    sc_status (*invoke)(void *runtime_state,
                        void *invoke_ref,
                        const sc_tool_call *call,
                        sc_str args_json,
                        sc_allocator *alloc,
                        sc_tool_result *out);
    void (*release_invoke)(void *runtime_state, void *invoke_ref);
    void (*shutdown)(void *runtime_state);
    void (*destroy)(void *runtime_state);
} sc_script_runtime_vtab;

sc_status sc_plugin_script_load_begin(sc_plugin_host *host,
                                      const sc_plugin_manifest *manifest,
                                      uint64_t language_capability,
                                      void *runtime_state,
                                      const sc_script_runtime_vtab *vtab,
                                      sc_plugin **out);
sc_status sc_plugin_script_register_tool(sc_plugin *plugin, sc_str spec_json, void *invoke_ref);
sc_status sc_plugin_script_load_finish(sc_plugin *plugin, sc_plugin **out);
void sc_plugin_script_load_abort(sc_plugin *plugin);
sc_allocator *sc_plugin_script_allocator(const sc_plugin *plugin);
sc_str sc_plugin_script_artifact_path(const sc_plugin *plugin);

sc_status sc_plugin_load_python_path(sc_plugin_host *host, const sc_plugin_manifest *manifest, sc_plugin **out);

SC_END_DECLS
