#![no_std]

use core::panic::PanicInfo;

const SC_GUEST_ABI_VERSION: i32 = 1;
const SC_ERR_PARSE: i32 = 4;

const TOOL_SPEC_JSON: &[u8] = br#"{"name":"sc-echo-rust","description":"Echo tool implemented as a Rust WASM plugin","input_schema":{"type":"object","properties":{"message":{"type":"string"}},"required":["message"]},"capabilities":0,"risk":0,"capability_category":0,"side_effect":0,"default_autonomy":0,"catalog_metadata_key":"tool.sc_echo_rust"}"#;
const INVOKE_NAME: &[u8] = b"sc_echo_rust_invoke";
const OUTPUT_JSON: &[u8] = br#"{"source":"rust","ok":true}"#;
const PARSE_KEY: &[u8] = b"sc.example_wasm_rust.parse_failed";

#[link(wasm_import_module = "env")]
extern "C" {
    fn sc_host_register_tool(
        spec_json_ptr: u32,
        spec_json_len: u32,
        invoke_name_ptr: u32,
        invoke_name_len: u32,
    ) -> i32;
    fn sc_host_set_tool_result(success: i32, output_ptr: u32, output_len: u32);
    fn sc_host_set_status(code: i32, key_ptr: u32, key_len: u32);
}

#[no_mangle]
pub extern "C" fn sc_plugin_guest_abi_version() -> i32 {
    SC_GUEST_ABI_VERSION
}

#[no_mangle]
pub extern "C" fn sc_plugin_init() {
    unsafe {
        let _ = sc_host_register_tool(
            TOOL_SPEC_JSON.as_ptr() as u32,
            TOOL_SPEC_JSON.len() as u32,
            INVOKE_NAME.as_ptr() as u32,
            INVOKE_NAME.len() as u32,
        );
    }
}

#[no_mangle]
pub extern "C" fn sc_plugin_shutdown() {}

#[no_mangle]
pub extern "C" fn sc_echo_rust_invoke(args_json_ptr: u32, args_json_len: u32) {
    unsafe {
        if args_json_ptr == 0 || args_json_len == 0 {
            sc_host_set_status(SC_ERR_PARSE, PARSE_KEY.as_ptr() as u32, PARSE_KEY.len() as u32);
            sc_host_set_tool_result(0, OUTPUT_JSON.as_ptr() as u32, OUTPUT_JSON.len() as u32);
            return;
        }
        sc_host_set_tool_result(1, OUTPUT_JSON.as_ptr() as u32, OUTPUT_JSON.len() as u32);
    }
}

#[panic_handler]
fn panic(_info: &PanicInfo) -> ! {
    loop {}
}
