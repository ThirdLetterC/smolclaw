#include "sc/hardware.h"
#include "sc/runtime.h"
#include "sc/version.h"
#include "test_helpers.h"

#include <stdio.h>
#include <string.h>

static sc_status make_manifest(sc_hardware_manifest *manifest);
static sc_status make_hardware(sc_hardware_context *hardware, sc_peripheral **peripheral_out);
static sc_status make_tool_call(sc_allocator *alloc, const char *json, sc_json_value **args, sc_tool_call *call);
static int test_manifest_registry_context(void);
static int test_serial_protocol_and_fake_board(void);
static int test_hardware_tools_security(void);
static int test_hardware_discovery_and_descriptors(void);
static int test_hardware_protocol_frame_parser(void);
static int test_hardware_estop_and_cancellation(void);

int main(void)
{
    int failures = 0;

    failures += test_manifest_registry_context();
    failures += test_serial_protocol_and_fake_board();
    failures += test_hardware_tools_security();
    failures += test_hardware_discovery_and_descriptors();
    failures += test_hardware_protocol_frame_parser();
    failures += test_hardware_estop_and_cancellation();

    return failures == 0 ? 0 : 1;
}

static sc_status make_manifest(sc_hardware_manifest *manifest)
{
    static const char json[] =
        "{\"alias\":\"lab\",\"board\":\"fake-board\",\"allow_raw_pins\":false,"
        "\"device_class\":\"board\",\"vendor_id\":\"smolclaw.fake\",\"product_id\":\"fake-board\","
        "\"serial_number\":\"LAB-001\",\"protocol\":\"fake-json\",\"discovery_mode\":\"passive\","
        "\"safe_shutdown_supported\":true,\"config_schema_ref\":\"sc.schema.hardware.fake_board.v1\","
        "\"manual_test_requirements\":\"fake-device ci coverage\","
        "\"pins\":[{\"alias\":\"led\",\"pin\":13,\"capabilities\":[\"gpio_read\",\"gpio_write\"]},"
        "{\"alias\":\"button\",\"pin\":5,\"capabilities\":[\"gpio_read\"]}]}";
    return sc_hardware_manifest_parse(sc_allocator_heap(), sc_str_from_cstr(json), manifest);
}

static sc_status make_hardware(sc_hardware_context *hardware, sc_peripheral **peripheral_out)
{
    sc_hardware_manifest manifest = {0};
    sc_serial_transport *serial = nullptr;
    sc_peripheral *peripheral = nullptr;
    sc_status status = make_manifest(&manifest);

    sc_hardware_context_init(hardware, sc_allocator_heap());
    if (sc_status_is_ok(status)) {
        status = sc_hardware_fake_serial_new(sc_allocator_heap(),
                                             &(sc_hardware_fake_serial_options){
                                                 .struct_size = sizeof(sc_hardware_fake_serial_options),
                                                 .gpio_value = true,
                                             },
                                             &serial);
    }
    if (sc_status_is_ok(status)) {
        status = sc_hardware_fake_board_new(sc_allocator_heap(), &manifest, serial, &peripheral);
    }
    if (sc_status_is_ok(status)) {
        status = sc_hardware_context_add_device(hardware, &manifest, peripheral);
    }
    if (!sc_status_is_ok(status)) {
        sc_peripheral_destroy(peripheral);
        sc_serial_transport_destroy(serial);
        sc_hardware_context_clear(hardware);
    } else if (peripheral_out != nullptr) {
        *peripheral_out = peripheral;
    }
    sc_hardware_manifest_clear(&manifest);
    return status;
}

static sc_status make_tool_call(sc_allocator *alloc, const char *json, sc_json_value **args, sc_tool_call *call)
{
    sc_json_parse_error error = {0};
    sc_status status = sc_json_parse(alloc, sc_str_from_cstr(json), args, &error);
    if (sc_status_is_ok(status)) {
        *call = (sc_tool_call){
            .struct_size = sizeof(*call),
            .call_id = sc_str_from_cstr("call-1"),
            .args = *args,
        };
    }
    return status;
}

static int test_manifest_registry_context(void)
{
    int failures = 0;
    sc_hardware_manifest manifest = {0};
    sc_peripheral_registry registry = {0};
    sc_peripheral *created = nullptr;
    sc_hardware_context hardware = {0};
    sc_peripheral *peripheral = nullptr;
    sc_string context_text = {0};
    int raw_pin = 0;

    failures += sc_test_expect_status("manifest parse", make_manifest(&manifest), SC_OK);
    failures += sc_test_expect_true("manifest fields",
                            strcmp(manifest.alias.ptr, "lab") == 0 &&
                                strcmp(manifest.board.ptr, "fake-board") == 0 &&
                                manifest.pins.len == 2);
    failures += sc_test_expect_status("resolve alias",
                              sc_hardware_manifest_resolve_pin(&manifest, sc_str_from_cstr("led"), SC_HARDWARE_CAP_GPIO_WRITE, &raw_pin),
                              SC_OK);
    failures += sc_test_expect_true("resolved raw pin", raw_pin == 13);
    failures += sc_test_expect_status("invalid pin alias",
                              sc_hardware_manifest_resolve_pin(&manifest, sc_str_from_cstr("missing"), SC_HARDWARE_CAP_GPIO_READ, &raw_pin),
                              SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_status("raw pin disabled",
                              sc_hardware_manifest_resolve_pin(&manifest, sc_str_from_cstr("13"), SC_HARDWARE_CAP_GPIO_READ, &raw_pin),
                              SC_ERR_SECURITY_DENIED);
    sc_hardware_registry_init(&registry, sc_allocator_heap());
    failures += sc_test_expect_status("register fake", sc_hardware_registry_register_fake(&registry), SC_OK);
    failures += sc_test_expect_true("fake registered", sc_peripheral_registry_len(&registry) == 1);
    failures += sc_test_expect_status("create fake", sc_peripheral_registry_create(&registry, sc_str_from_cstr("fake-board"), sc_allocator_heap(), &created), SC_OK);

    failures += sc_test_expect_status("make hardware", make_hardware(&hardware, &peripheral), SC_OK);
    failures += sc_test_expect_true("hardware enabled", sc_hardware_tools_enabled(&hardware));
    failures += sc_test_expect_status("bounded context", sc_hardware_context_prompt(&hardware, sc_allocator_heap(), 48, &context_text), SC_OK);
    failures += sc_test_expect_true("context bounded", context_text.len <= 48 && strstr(context_text.ptr, "Hardware") != nullptr);

    sc_string_clear(&context_text);
    sc_hardware_context_clear(&hardware);
    sc_peripheral_destroy(peripheral);
    sc_peripheral_destroy(created);
    sc_peripheral_registry_clear(&registry);
    sc_hardware_manifest_clear(&manifest);
    return failures;
}

static int test_serial_protocol_and_fake_board(void)
{
    int failures = 0;
    sc_serial_transport *serial = nullptr;
    sc_serial_transport *timeout_serial = nullptr;
    sc_serial_transport *malformed_serial = nullptr;
    sc_hardware_manifest manifest = {0};
    sc_peripheral *peripheral = nullptr;
    sc_peripheral_health health = {0};
    sc_peripheral_context context = {0};
    sc_peripheral_result result = {0};
    sc_string pong = {0};
    uint64_t caps = 0;
    bool value = false;

    failures += sc_test_expect_status("fake serial", sc_hardware_fake_serial_new(sc_allocator_heap(), nullptr, &serial), SC_OK);
    failures += sc_test_expect_status("protocol ping", sc_hardware_protocol_ping(serial, sc_allocator_heap(), &pong), SC_OK);
    failures += sc_test_expect_true("pong", strcmp(pong.ptr, "pong") == 0);
    failures += sc_test_expect_status("capabilities", sc_hardware_protocol_capabilities(serial, &caps), SC_OK);
    failures += sc_test_expect_true("capability merge", (caps & SC_HARDWARE_CAP_GPIO_READ) != 0 && (caps & SC_HARDWARE_CAP_GPIO_WRITE) != 0);
    failures += sc_test_expect_status("gpio write", sc_hardware_protocol_gpio_write(serial, 13, true), SC_OK);
    failures += sc_test_expect_status("gpio read", sc_hardware_protocol_gpio_read(serial, 13, &value), SC_OK);
    failures += sc_test_expect_true("gpio value", value);
    sc_string_clear(&pong);
    sc_serial_transport_destroy(serial);

    failures += sc_test_expect_status("timeout serial",
                              sc_hardware_fake_serial_new(sc_allocator_heap(),
                                                          &(sc_hardware_fake_serial_options){
                                                              .struct_size = sizeof(sc_hardware_fake_serial_options),
                                                              .timeout_next = true,
                                                          },
                                                          &timeout_serial),
                              SC_OK);
    failures += sc_test_expect_status("timeout", sc_hardware_protocol_ping(timeout_serial, sc_allocator_heap(), &pong), SC_ERR_TIMEOUT);
    sc_serial_transport_destroy(timeout_serial);
    failures += sc_test_expect_status("malformed serial",
                              sc_hardware_fake_serial_new(sc_allocator_heap(),
                                                          &(sc_hardware_fake_serial_options){
                                                              .struct_size = sizeof(sc_hardware_fake_serial_options),
                                                              .malformed_next = true,
                                                          },
                                                          &malformed_serial),
                              SC_OK);
    failures += sc_test_expect_status("malformed response", sc_hardware_protocol_ping(malformed_serial, sc_allocator_heap(), &pong), SC_ERR_PARSE);
    sc_serial_transport_destroy(malformed_serial);

    failures += sc_test_expect_status("manifest", make_manifest(&manifest), SC_OK);
    failures += sc_test_expect_status("board serial", sc_hardware_fake_serial_new(sc_allocator_heap(), nullptr, &serial), SC_OK);
    failures += sc_test_expect_status("fake board", sc_hardware_fake_board_new(sc_allocator_heap(), &manifest, serial, &peripheral), SC_OK);
    failures += sc_test_expect_status("board health", sc_peripheral_health_check(peripheral, sc_allocator_heap(), &health), SC_OK);
    failures += sc_test_expect_true("board healthy", health.healthy);
    failures += sc_test_expect_status("board context", sc_peripheral_describe_context(peripheral, sc_allocator_heap(), &context), SC_OK);
    failures += sc_test_expect_true("board context text", strstr(context.text.ptr, "led") != nullptr);
    failures += sc_test_expect_status("board gpio read",
                              sc_peripheral_command_send(peripheral,
                                                         &(sc_peripheral_command){
                                                             .struct_size = sizeof(sc_peripheral_command),
                                                             .operation = sc_str_from_cstr("gpio_read"),
                                                             .payload = sc_buf_from_parts("{\"pin\":13}", strlen("{\"pin\":13}")),
                                                         },
                                                         sc_allocator_heap(),
                                                         &result),
                              SC_OK);
    failures += sc_test_expect_true("board read payload", strstr((const char *)result.payload.ptr, "value") != nullptr);

    sc_peripheral_result_clear(&result);
    sc_peripheral_context_clear(&context);
    sc_peripheral_health_clear(&health);
    sc_peripheral_destroy(peripheral);
    sc_hardware_manifest_clear(&manifest);
    return failures;
}

static int test_hardware_tools_security(void)
{
    int failures = 0;
    sc_hardware_context hardware = {0};
    sc_peripheral *peripheral = nullptr;
    sc_security_policy policy = {0};
    sc_receipt_chain receipts = {0};
    sc_tool_context tool_context = {0};
    sc_tool *read_tool = nullptr;
    sc_tool *write_tool = nullptr;
    sc_tool_registry tool_registry = {0};
    sc_tool_spec spec = {0};
    sc_tool_result result = {0};
    sc_json_value *args = nullptr;
    sc_tool_call call = {0};

    failures += sc_test_expect_status("make hardware", make_hardware(&hardware, &peripheral), SC_OK);
    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    policy.autonomy = SC_AUTONOMY_AUTONOMOUS;
    failures += sc_test_expect_status("allow read", sc_security_policy_add_allowed_tool(&policy, sc_str_from_cstr("hardware.gpio_read")), SC_OK);
    sc_tool_registry_init(&tool_registry, sc_allocator_heap());
    sc_receipt_chain_init(&receipts, sc_allocator_heap());
    tool_context = (sc_tool_context){
        .struct_size = sizeof(tool_context),
        .policy = &policy,
        .receipts = &receipts,
        .max_output_bytes = 128,
    };
    failures += sc_test_expect_status("read tool", sc_tool_hardware_gpio_read_new(sc_allocator_heap(), &tool_context, &hardware, &read_tool), SC_OK);
    failures += sc_test_expect_status("write tool", sc_tool_hardware_gpio_write_new(sc_allocator_heap(), &tool_context, &hardware, &write_tool), SC_OK);
    failures += sc_test_expect_status("hardware registry boundary", sc_hardware_tools_register(&tool_registry, &tool_context, &hardware), SC_OK);
    failures += sc_test_expect_status("read spec", sc_tool_spec_get(read_tool, &spec), SC_OK);
    failures += sc_test_expect_true("runtime can list fake hardware tool", sc_str_equal(spec.name, sc_str_from_cstr("hardware.gpio_read")));

    failures += sc_test_expect_status("read args", make_tool_call(sc_allocator_heap(), "{\"device\":\"lab\",\"pin\":\"led\"}", &args, &call), SC_OK);
    failures += sc_test_expect_status("read invoke", sc_tool_invoke(read_tool, &call, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("read output", result.success && strstr(result.output.ptr, "value") != nullptr);
    failures += sc_test_expect_true("read receipt", receipts.receipts.len == 1 && sc_receipt_chain_verify(&receipts));
    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("raw pin args", make_tool_call(sc_allocator_heap(), "{\"device\":\"lab\",\"pin\":\"13\"}", &args, &call), SC_OK);
    failures += sc_test_expect_status("raw pin denied", sc_tool_invoke(read_tool, &call, sc_allocator_heap(), &result), SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_true("raw pin denied receipt", receipts.receipts.len == 2 && sc_receipt_chain_verify(&receipts));
    sc_json_destroy(args);
    args = nullptr;

    failures += sc_test_expect_status("write args", make_tool_call(sc_allocator_heap(), "{\"device\":\"lab\",\"pin\":\"led\",\"value\":\"true\"}", &args, &call), SC_OK);
    failures += sc_test_expect_status("write denied", sc_tool_invoke(write_tool, &call, sc_allocator_heap(), &result), SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_true("write denied receipt", receipts.receipts.len == 3 && sc_receipt_chain_verify(&receipts));
    failures += sc_test_expect_status("allow write", sc_security_policy_add_allowed_tool(&policy, sc_str_from_cstr("hardware.gpio_write")), SC_OK);
    failures += sc_test_expect_status("allow other device", sc_security_policy_add_sandbox_allow_device(&policy, sc_str_from_cstr("other")), SC_OK);
    failures += sc_test_expect_status("hardware device denied", sc_tool_invoke(write_tool, &call, sc_allocator_heap(), &result), SC_ERR_SECURITY_DENIED);
    failures += sc_test_expect_true("hardware device denied receipt", receipts.receipts.len == 4 && sc_receipt_chain_verify(&receipts));
    failures += sc_test_expect_status("allow lab device", sc_security_policy_add_sandbox_allow_device(&policy, sc_str_from_cstr("lab")), SC_OK);
    failures += sc_test_expect_status("write invoke", sc_tool_invoke(write_tool, &call, sc_allocator_heap(), &result), SC_OK);
    failures += sc_test_expect_true("write receipt includes device", receipts.receipts.len == 5 && sc_receipt_chain_verify(&receipts));

    sc_tool_result_clear(&result);
    sc_json_destroy(args);
    sc_tool_destroy(write_tool);
    sc_tool_destroy(read_tool);
    sc_tool_registry_clear(&tool_registry);
    sc_receipt_chain_clear(&receipts);
    sc_security_policy_clear(&policy);
    sc_hardware_context_clear(&hardware);
    sc_peripheral_destroy(peripheral);
    return failures;
}

static int test_hardware_discovery_and_descriptors(void)
{
    int failures = 0;
    sc_hardware_manifest manifest = {0};
    sc_hardware_context hardware = {0};
    sc_hardware_context mismatch_hardware = {0};
    sc_hardware_context unsafe_hardware = {0};
    sc_peripheral *peripheral = nullptr;
    sc_peripheral *unused_peripheral = nullptr;
    sc_observer *observer = nullptr;
    sc_event_buffer_observer *buffer = nullptr;
    const sc_peripheral_vtab *vtab = nullptr;

    failures += sc_test_expect_status("manifest", make_manifest(&manifest), SC_OK);
    failures += sc_test_expect_status("event buffer", sc_observer_event_buffer_new(sc_allocator_heap(), 4, &buffer, &observer), SC_OK);
    sc_hardware_context_init(&hardware, sc_allocator_heap());
    sc_hardware_context_set_observer(&hardware, observer);
    failures += sc_test_expect_status("passive discovery",
                              sc_hardware_context_discover_fake(&hardware,
                                                                &(sc_hardware_discovery_options){
                                                                    .struct_size = sizeof(sc_hardware_discovery_options),
                                                                    .manifest = &manifest,
                                                                    .serial_options = {.struct_size = sizeof(sc_hardware_fake_serial_options)},
                                                                    .expected_vendor_id = sc_str_from_cstr("smolclaw.fake"),
                                                                    .expected_product_id = sc_str_from_cstr("fake-board"),
                                                                    .expected_serial_number = sc_str_from_cstr("LAB-001"),
                                                                    .expected_protocol = sc_str_from_cstr("fake-json"),
                                                                },
                                                                &peripheral),
                              SC_OK);
    failures += sc_test_expect_true("discovery events", sc_observer_event_buffer_len(buffer) >= 2);
    vtab = sc_peripheral_vtab_of(peripheral);
    failures += sc_test_expect_true("descriptor metadata",
                            vtab != nullptr &&
                                vtab->device_class != nullptr &&
                                strcmp(vtab->device_class, "board") == 0 &&
                                vtab->vendor_id != nullptr &&
                                strcmp(vtab->vendor_id, "smolclaw.fake") == 0 &&
                                vtab->config_schema_ref != nullptr &&
                                vtab->manual_test_requirements != nullptr &&
                                vtab->safe_discovery_mode == SC_HARDWARE_DISCOVERY_PASSIVE &&
                                vtab->identity_required);

    sc_hardware_context_init(&mismatch_hardware, sc_allocator_heap());
    failures += sc_test_expect_status("identity mismatch",
                              sc_hardware_context_discover_fake(&mismatch_hardware,
                                                                &(sc_hardware_discovery_options){
                                                                    .struct_size = sizeof(sc_hardware_discovery_options),
                                                                    .manifest = &manifest,
                                                                    .expected_vendor_id = sc_str_from_cstr("wrong.vendor"),
                                                                },
                                                                &unused_peripheral),
                              SC_ERR_SECURITY_DENIED);

    manifest.discovery_mode = SC_HARDWARE_DISCOVERY_UNSAFE_CLAIM;
    sc_hardware_context_init(&unsafe_hardware, sc_allocator_heap());
    failures += sc_test_expect_status("unsafe claim denied",
                              sc_hardware_context_discover_fake(&unsafe_hardware,
                                                                &(sc_hardware_discovery_options){
                                                                    .struct_size = sizeof(sc_hardware_discovery_options),
                                                                    .manifest = &manifest,
                                                                },
                                                                &unused_peripheral),
                              SC_ERR_SECURITY_DENIED);

    sc_hardware_context_clear(&unsafe_hardware);
    sc_hardware_context_clear(&mismatch_hardware);
    sc_hardware_context_clear(&hardware);
    sc_peripheral_destroy(unused_peripheral);
    sc_peripheral_destroy(peripheral);
    sc_observer_destroy(observer);
    sc_hardware_manifest_clear(&manifest);
    return failures;
}

static int test_hardware_protocol_frame_parser(void)
{
    int failures = 0;
    uint8_t frame[8] = {0x5au, SC_HARDWARE_PROTOCOL_COMMAND_GPIO_READ, 0x00u, 0x03u, 'l', 'e', 'd', 0x00u};
    uint8_t partial[] = {0x5au, SC_HARDWARE_PROTOCOL_COMMAND_PING, 0x00u};
    uint8_t bad_checksum[] = {0x5au, SC_HARDWARE_PROTOCOL_COMMAND_PING, 0x00u, 0x00u, 0xffu};
    uint8_t unknown[] = {0x5au, 0x7fu, 0x00u, 0x00u, 0xd9u};
    uint8_t oversized[] = {0x5au, SC_HARDWARE_PROTOCOL_COMMAND_GPIO_WRITE, 0x00u, 0x20u, 0x00u};
    sc_hardware_frame parsed = {0};
    sc_serial_transport *serial = nullptr;
    sc_string ignored = {0};

    frame[7] = sc_hardware_protocol_frame_checksum(sc_buf_from_parts(frame, 7));
    failures += sc_test_expect_status("frame parse", sc_hardware_protocol_parse_frame(sc_buf_from_parts(frame, sizeof(frame)), 8, &parsed), SC_OK);
    failures += sc_test_expect_true("frame fields", parsed.command_id == SC_HARDWARE_PROTOCOL_COMMAND_GPIO_READ && parsed.payload.len == 3);
    failures += sc_test_expect_status("partial frame", sc_hardware_protocol_parse_frame(sc_buf_from_parts(partial, sizeof(partial)), 8, &parsed), SC_ERR_PARSE);
    failures += sc_test_expect_status("bad checksum", sc_hardware_protocol_parse_frame(sc_buf_from_parts(bad_checksum, sizeof(bad_checksum)), 8, &parsed), SC_ERR_PARSE);
    failures += sc_test_expect_status("unknown command", sc_hardware_protocol_parse_frame(sc_buf_from_parts(unknown, sizeof(unknown)), 8, &parsed), SC_ERR_UNSUPPORTED);
    failures += sc_test_expect_status("oversized frame", sc_hardware_protocol_parse_frame(sc_buf_from_parts(oversized, sizeof(oversized)), 8, &parsed), SC_ERR_SECURITY_DENIED);

    failures += sc_test_expect_status("disappearing serial",
                              sc_hardware_fake_serial_new(sc_allocator_heap(),
                                                          &(sc_hardware_fake_serial_options){
                                                              .struct_size = sizeof(sc_hardware_fake_serial_options),
                                                              .disappear_next = true,
                                                          },
                                                          &serial),
                              SC_OK);
    failures += sc_test_expect_status("device disappeared", sc_hardware_protocol_ping(serial, sc_allocator_heap(), &ignored), SC_ERR_IO);
    sc_serial_transport_destroy(serial);
    serial = nullptr;
    failures += sc_test_expect_status("bad checksum serial",
                              sc_hardware_fake_serial_new(sc_allocator_heap(),
                                                          &(sc_hardware_fake_serial_options){
                                                              .struct_size = sizeof(sc_hardware_fake_serial_options),
                                                              .bad_checksum_next = true,
                                                          },
                                                          &serial),
                              SC_OK);
    failures += sc_test_expect_status("bad checksum command", sc_hardware_protocol_ping(serial, sc_allocator_heap(), &ignored), SC_ERR_PARSE);
    sc_serial_transport_destroy(serial);
    sc_string_clear(&ignored);
    return failures;
}

static int test_hardware_estop_and_cancellation(void)
{
    int failures = 0;
    sc_hardware_context hardware = {0};
    sc_peripheral *peripheral = nullptr;
    sc_security_policy policy = {0};
    sc_estop_state estop = {0};
    sc_cancel_token cancel = {0};
    sc_tool_context tool_context = {0};
    sc_tool *write_tool = nullptr;
    sc_tool_result result = {0};
    sc_json_value *args = nullptr;
    sc_tool_call call = {0};
    sc_peripheral_result peripheral_result = {0};

    failures += sc_test_expect_status("make hardware", make_hardware(&hardware, &peripheral), SC_OK);
    failures += sc_test_expect_status("policy defaults", sc_security_policy_init_defaults(&policy, sc_allocator_heap()), SC_OK);
    policy.autonomy = SC_AUTONOMY_AUTONOMOUS;
    failures += sc_test_expect_status("allow write", sc_security_policy_add_allowed_tool(&policy, sc_str_from_cstr("hardware.gpio_write")), SC_OK);
    sc_estop_init(&estop, sc_allocator_heap());
    failures += sc_test_expect_status("trip estop", sc_estop_trip(&estop, sc_str_from_cstr("hardware")), SC_OK);
    sc_hardware_context_set_estop(&hardware, &estop);
    tool_context = (sc_tool_context){
        .struct_size = sizeof(tool_context),
        .policy = &policy,
        .estop = &estop,
        .max_output_bytes = 128,
    };
    failures += sc_test_expect_status("write tool", sc_tool_hardware_gpio_write_new(sc_allocator_heap(), &tool_context, &hardware, &write_tool), SC_OK);
    failures += sc_test_expect_status("write args", make_tool_call(sc_allocator_heap(), "{\"device\":\"lab\",\"pin\":\"led\",\"value\":\"true\"}", &args, &call), SC_OK);
    failures += sc_test_expect_status("estop denies write", sc_tool_invoke(write_tool, &call, sc_allocator_heap(), &result), SC_ERR_SECURITY_DENIED);

    failures += sc_test_expect_status("cancel init", sc_cancel_token_init(&cancel, sc_allocator_heap()), SC_OK);
    failures += sc_test_expect_status("cancel token", sc_cancel_token_cancel(&cancel, sc_allocator_heap(), sc_str_from_cstr("hardware")), SC_OK);
    failures += sc_test_expect_status("cancelled command",
                              sc_peripheral_command_send(peripheral,
                                                         &(sc_peripheral_command){
                                                             .struct_size = sizeof(sc_peripheral_command),
                                                             .operation = sc_str_from_cstr("gpio_write"),
                                                             .payload = sc_buf_from_parts("{\"pin\":13,\"value\":true}", strlen("{\"pin\":13,\"value\":true}")),
                                                             .cancel_token = &cancel,
                                                             .destructive = true,
                                                         },
                                                         sc_allocator_heap(),
                                                         &peripheral_result),
                              SC_ERR_CANCELLED);

    sc_peripheral_result_clear(&peripheral_result);
    sc_cancel_token_clear(&cancel);
    sc_json_destroy(args);
    sc_tool_result_clear(&result);
    sc_tool_destroy(write_tool);
    sc_estop_clear(&estop);
    sc_security_policy_clear(&policy);
    sc_hardware_context_clear(&hardware);
    sc_peripheral_destroy(peripheral);
    return failures;
}
