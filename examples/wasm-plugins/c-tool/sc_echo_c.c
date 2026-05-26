#include <stdint.h>

enum {
    SC_GUEST_ABI_VERSION = 1,
    SC_ERR_PARSE = 4
};

__attribute__((import_module("env"), import_name("sc_host_register_tool")))
extern int32_t sc_host_register_tool(uint32_t spec_json_ptr,
                                     uint32_t spec_json_len,
                                     uint32_t invoke_name_ptr,
                                     uint32_t invoke_name_len);

__attribute__((import_module("env"), import_name("sc_host_set_tool_result")))
extern void sc_host_set_tool_result(int32_t success, uint32_t output_ptr, uint32_t output_len);

__attribute__((import_module("env"), import_name("sc_host_set_status")))
extern void sc_host_set_status(int32_t code, uint32_t key_ptr, uint32_t key_len);

static const char tool_spec_json[] =
    "{"
    "\"name\":\"sc-echo-c\","
    "\"description\":\"Echo tool implemented as a C WASM plugin\","
    "\"input_schema\":{"
    "\"type\":\"object\","
    "\"properties\":{\"message\":{\"type\":\"string\"}},"
    "\"required\":[\"message\"]"
    "},"
    "\"capabilities\":0,"
    "\"risk\":0,"
    "\"capability_category\":0,"
    "\"side_effect\":0,"
    "\"default_autonomy\":0,"
    "\"catalog_metadata_key\":\"tool.sc_echo_c\""
    "}";

static const char invoke_name[] = "sc_echo_c_invoke";
static const char output_json[] = "{\"source\":\"c\",\"ok\":true}";
static const char parse_key[] = "sc.example_wasm_c.parse_failed";

int32_t sc_plugin_guest_abi_version(void)
{
    return SC_GUEST_ABI_VERSION;
}

void sc_plugin_init(void)
{
    (void)sc_host_register_tool((uint32_t)(uintptr_t)tool_spec_json,
                                (uint32_t)(sizeof(tool_spec_json) - 1),
                                (uint32_t)(uintptr_t)invoke_name,
                                (uint32_t)(sizeof(invoke_name) - 1));
}

void sc_plugin_shutdown(void)
{
}

void sc_echo_c_invoke(uint32_t args_json_ptr, uint32_t args_json_len)
{
    if (args_json_ptr == 0 || args_json_len == 0) {
        sc_host_set_status(SC_ERR_PARSE, (uint32_t)(uintptr_t)parse_key, (uint32_t)(sizeof(parse_key) - 1));
        sc_host_set_tool_result(0, (uint32_t)(uintptr_t)output_json, (uint32_t)(sizeof(output_json) - 1));
        return;
    }
    sc_host_set_tool_result(1, (uint32_t)(uintptr_t)output_json, (uint32_t)(sizeof(output_json) - 1));
}
