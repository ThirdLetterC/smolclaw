#include "sc/plugin.h"

static sc_status validation_export_init(const sc_plugin_host_api *api, void **plugin_state);

static const sc_plugin_descriptor validation_export_descriptor = {
    .struct_size = sizeof(sc_plugin_descriptor),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .abi_minor = SC_ABI_VERSION_MINOR,
    .name = "validation-export-plugin",
    .version = "1.0.0",
    .init = validation_export_init,
};

SC_EXPORT const sc_plugin_descriptor *validation_plugin_descriptor(void) __asm__(SC_PLUGIN_DESCRIPTOR_SYMBOL);

const sc_plugin_descriptor *validation_plugin_descriptor(void)
{
    return &validation_export_descriptor;
}

static sc_status validation_export_init(const sc_plugin_host_api *api, void **plugin_state)
{
    (void)api;
    if (plugin_state != nullptr) {
        *plugin_state = nullptr;
    }
    return (sc_status){.code = SC_OK};
}
