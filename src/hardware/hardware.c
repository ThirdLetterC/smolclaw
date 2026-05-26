#include "sc/hardware.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "sc/api.h"
#include "sc/runtime.h"
#include "tools/tool_internal.h"

typedef struct sc_serial_transport {
    sc_allocator *alloc;
    const sc_serial_transport_vtab *vtab;
    void *impl;
} sc_serial_transport;

typedef struct fake_serial {
    sc_allocator *alloc;
    bool timeout_next;
    bool malformed_next;
    bool gpio_value;
    uint64_t capabilities;
    bool disappear_next;
    bool partial_read_next;
    bool bad_checksum_next;
    bool oversized_frame_next;
    bool unknown_command_next;
    bool safe_shutdown_supported;
    bool safe_shutdown_called;
} fake_serial;

typedef struct fake_board {
    sc_allocator *alloc;
    sc_hardware_manifest manifest;
    sc_serial_transport *transport;
} fake_board;

typedef struct hardware_tool {
    sc_tool_impl_context context;
    sc_hardware_context *hardware;
    bool write;
} hardware_tool;

static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out);
static sc_status copy_optional_string(sc_allocator *alloc, const sc_json_value *object, sc_str key, sc_string *out);
static sc_str empty_if_null(sc_str value);
static void hardware_pin_clear(sc_hardware_pin *pin);
static sc_status manifest_clone(sc_allocator *alloc, const sc_hardware_manifest *source, sc_hardware_manifest *out);
static bool str_present(sc_str value);
static bool str_matches_if_present(sc_str expected, sc_str actual);
static uint64_t capability_from_name(sc_str name);
static sc_hardware_device_class device_class_from_name(sc_str name);
static sc_hardware_discovery_mode discovery_mode_from_name(sc_str name);
static sc_status parse_capabilities(const sc_json_value *value, uint64_t *out);
static sc_status json_get_required_str(const sc_json_value *object, sc_str key, sc_str *out);
static sc_status json_get_optional_bool(const sc_json_value *object, sc_str key, bool default_value, bool *out);
static sc_status json_get_optional_str(const sc_json_value *object, sc_str key, sc_str *out);
static sc_status json_get_required_int(const sc_json_value *object, sc_str key, int *out);
static bool str_to_int(sc_str text, int *out);
static bool raw_pin_allowed(const sc_hardware_manifest *manifest, sc_str input, int *out);
static sc_status append_context_device(const sc_hardware_device *device, sc_string_builder *builder, size_t max_bytes);
static sc_status serial_common_response(sc_allocator *alloc, sc_str body, sc_string *out);
static sc_status hardware_emit(sc_hardware_context *context, sc_str name, sc_str operation, sc_str outcome, sc_str error_key);
static sc_status hardware_emit_command(const sc_peripheral_command *command,
                                       sc_str name,
                                       sc_str operation,
                                       sc_str outcome,
                                       sc_str error_key);
static sc_status hardware_validate_identity(const sc_hardware_manifest *manifest,
                                            sc_str expected_vendor_id,
                                            sc_str expected_product_id,
                                            sc_str expected_serial_number,
                                            sc_str expected_protocol);
static sc_status fake_serial_exchange(void *impl, sc_str request_json, sc_allocator *alloc, sc_string *response_json);
static void fake_serial_destroy(void *impl);
static sc_status response_require_ok(sc_str response_json, sc_allocator *alloc, sc_json_value **out_root);
static sc_status fake_board_health(void *impl, sc_allocator *alloc, sc_peripheral_health *out);
static sc_status fake_board_command(void *impl,
                                    const sc_peripheral_command *command,
                                    sc_allocator *alloc,
                                    sc_peripheral_result *out);
static sc_status fake_board_context(void *impl, sc_allocator *alloc, sc_peripheral_context *out);
static sc_status fake_board_safe_shutdown(void *impl);
static void fake_board_destroy(void *impl);
static sc_status fake_board_factory(sc_allocator *alloc, sc_peripheral **out);
static sc_status hardware_tool_spec(void *impl, sc_tool_spec *out);
static sc_status hardware_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out);
static void hardware_tool_destroy(void *impl);
static sc_status hardware_tool_new(sc_allocator *alloc,
                                   const sc_tool_context *tool_context,
                                   sc_hardware_context *hardware,
                                   bool write,
                                   sc_tool **out);

static const sc_serial_transport_vtab fake_serial_vtab = {
    .struct_size = sizeof(sc_serial_transport_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "fake-serial",
    .exchange = fake_serial_exchange,
    .destroy = fake_serial_destroy,
};

static const sc_peripheral_vtab fake_board_vtab = {
    .struct_size = sizeof(sc_peripheral_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "fake-board",
    .display_name = "Fake board",
    .feature_flag = "SC_ENABLE_HARDWARE",
    .capabilities = SC_HARDWARE_CAP_GPIO_READ | SC_HARDWARE_CAP_GPIO_WRITE | SC_HARDWARE_CAP_CONTEXT | SC_HARDWARE_CAP_SERIAL,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .health = fake_board_health,
    .command = fake_board_command,
    .describe_context = fake_board_context,
    .destroy = fake_board_destroy,
    .device_class = "board",
    .vendor_id = "smolclaw.fake",
    .product_id = "fake-board",
    .config_schema_ref = "sc.schema.hardware.fake_board.v1",
    .manual_test_requirements = "fake-only; real hardware requires external manual test",
    .safe_discovery_mode = SC_HARDWARE_DISCOVERY_PASSIVE,
    .identity_required = true,
    .safe_shutdown = fake_board_safe_shutdown,
};

static const sc_tool_vtab gpio_read_tool_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "hardware.gpio_read",
    .display_name = "Hardware GPIO read",
    .feature_flag = "SC_ENABLE_HARDWARE",
    .capabilities = SC_CONTRACT_CAP_SECURE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = hardware_tool_spec,
    .invoke = hardware_tool_invoke,
    .destroy = hardware_tool_destroy,
};

static const sc_tool_vtab gpio_write_tool_vtab = {
    .struct_size = sizeof(sc_tool_vtab),
    .abi_major = SC_ABI_VERSION_MAJOR,
    .name = "hardware.gpio_write",
    .display_name = "Hardware GPIO write",
    .feature_flag = "SC_ENABLE_HARDWARE",
    .capabilities = SC_CONTRACT_CAP_SECURE,
    .stability = SC_STABILITY_EXPERIMENTAL,
    .spec = hardware_tool_spec,
    .invoke = hardware_tool_invoke,
    .destroy = hardware_tool_destroy,
};

void sc_peripheral_result_clear(sc_peripheral_result *result)
{
    if (result == nullptr) {
        return;
    }
    sc_bytes_clear(&result->payload);
    *result = (sc_peripheral_result){0};
}

void sc_peripheral_health_clear(sc_peripheral_health *health)
{
    if (health == nullptr) {
        return;
    }
    sc_string_clear(&health->message);
    *health = (sc_peripheral_health){0};
}

void sc_peripheral_context_clear(sc_peripheral_context *context)
{
    if (context == nullptr) {
        return;
    }
    sc_string_clear(&context->text);
    *context = (sc_peripheral_context){0};
}

void sc_hardware_manifest_init(sc_hardware_manifest *manifest, sc_allocator *alloc)
{
    if (manifest == nullptr) {
        return;
    }
    *manifest = (sc_hardware_manifest){.alloc = alloc == nullptr ? sc_allocator_heap() : alloc};
    sc_vec_init(&manifest->pins, manifest->alloc, sizeof(sc_hardware_pin));
}

sc_status sc_hardware_manifest_parse(sc_allocator *alloc, sc_str json, sc_hardware_manifest *out)
{
    sc_json_parse_error error = {0};
    sc_json_value *root = nullptr;
    sc_json_value *pins = nullptr;
    const sc_json_value *capabilities = nullptr;
    sc_hardware_manifest manifest = {0};
    sc_str alias = {0};
    sc_str board = {0};
    sc_str optional = {0};
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.hardware.manifest.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    status = sc_json_parse(alloc, json, &root, &error);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    if (sc_json_type_of(root) != SC_JSON_OBJECT) {
        sc_json_destroy(root);
        return sc_status_parse("sc.hardware.manifest.not_object");
    }
    sc_hardware_manifest_init(&manifest, alloc);
    status = json_get_required_str(root, sc_str_from_cstr("alias"), &alias);
    if (sc_status_is_ok(status)) {
        status = json_get_required_str(root, sc_str_from_cstr("board"), &board);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, alias, &manifest.alias);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, board, &manifest.board);
    }
    if (sc_status_is_ok(status)) {
        status = copy_optional_string(alloc, root, sc_str_from_cstr("vendor_id"), &manifest.vendor_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_optional_string(alloc, root, sc_str_from_cstr("product_id"), &manifest.product_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_optional_string(alloc, root, sc_str_from_cstr("serial_number"), &manifest.serial_number);
    }
    if (sc_status_is_ok(status)) {
        status = copy_optional_string(alloc, root, sc_str_from_cstr("protocol"), &manifest.protocol);
    }
    if (sc_status_is_ok(status)) {
        status = copy_optional_string(alloc, root, sc_str_from_cstr("config_schema_ref"), &manifest.config_schema_ref);
    }
    if (sc_status_is_ok(status)) {
        status = copy_optional_string(alloc, root, sc_str_from_cstr("manual_test_requirements"), &manifest.manual_test_requirements);
    }
    if (sc_status_is_ok(status)) {
        manifest.device_class = SC_HARDWARE_DEVICE_CLASS_BOARD;
        status = json_get_optional_str(root, sc_str_from_cstr("device_class"), &optional);
        if (sc_status_is_ok(status) && optional.ptr != nullptr) {
            manifest.device_class = device_class_from_name(optional);
            if (manifest.device_class == SC_HARDWARE_DEVICE_CLASS_UNKNOWN) {
                status = sc_status_parse("sc.hardware.manifest.device_class_invalid");
            }
        }
    }
    if (sc_status_is_ok(status)) {
        manifest.discovery_mode = SC_HARDWARE_DISCOVERY_PASSIVE;
        status = json_get_optional_str(root, sc_str_from_cstr("discovery_mode"), &optional);
        if (sc_status_is_ok(status) && optional.ptr != nullptr) {
            manifest.discovery_mode = discovery_mode_from_name(optional);
        }
    }
    if (sc_status_is_ok(status)) {
        status = json_get_optional_bool(root, sc_str_from_cstr("allow_raw_pins"), false, &manifest.allow_raw_pins);
    }
    if (sc_status_is_ok(status)) {
        status = json_get_optional_bool(root, sc_str_from_cstr("safe_shutdown_supported"), false, &manifest.safe_shutdown_supported);
    }
    capabilities = sc_json_object_get(root, sc_str_from_cstr("capabilities"));
    if (sc_status_is_ok(status) && capabilities != nullptr) {
        status = parse_capabilities(capabilities, &manifest.capabilities);
    }
    pins = sc_json_object_get(root, sc_str_from_cstr("pins"));
    if (sc_status_is_ok(status) && (pins == nullptr || sc_json_type_of(pins) != SC_JSON_ARRAY)) {
        status = sc_status_parse("sc.hardware.manifest.pins_invalid");
    }
    for (size_t i = 0; sc_status_is_ok(status) && pins != nullptr && i < sc_json_array_len(pins); i += 1) {
        sc_json_value *pin = sc_json_array_get(pins, i);
        const sc_json_value *pin_caps = nullptr;
        sc_str pin_alias = {0};
        int raw_pin = 0;
        uint64_t caps = 0;
        if (pin == nullptr || sc_json_type_of(pin) != SC_JSON_OBJECT) {
            status = sc_status_parse("sc.hardware.manifest.pin_invalid");
            break;
        }
        status = json_get_required_str(pin, sc_str_from_cstr("alias"), &pin_alias);
        if (sc_status_is_ok(status)) {
            status = json_get_required_int(pin, sc_str_from_cstr("pin"), &raw_pin);
        }
        pin_caps = sc_json_object_get(pin, sc_str_from_cstr("capabilities"));
        if (sc_status_is_ok(status)) {
            status = parse_capabilities(pin_caps, &caps);
        }
        if (sc_status_is_ok(status)) {
            status = sc_hardware_manifest_add_pin(&manifest, pin_alias, raw_pin, caps);
        }
    }
    sc_json_destroy(root);
    if (!sc_status_is_ok(status)) {
        sc_hardware_manifest_clear(&manifest);
        return status;
    }
    *out = manifest;
    return sc_status_ok();
}

sc_status sc_hardware_manifest_add_pin(sc_hardware_manifest *manifest,
                                       sc_str alias,
                                       int raw_pin,
                                       uint64_t capabilities)
{
    sc_hardware_pin pin = {0};
    sc_status status;

    if (manifest == nullptr || alias.len == 0 || alias.ptr == nullptr || raw_pin < 0) {
        return sc_status_invalid_argument("sc.hardware.manifest.pin_invalid");
    }
    status = copy_string(manifest->alloc, alias, &pin.alias);
    if (sc_status_is_ok(status)) {
        pin.raw_pin = raw_pin;
        pin.capabilities = capabilities;
        status = sc_vec_push(&manifest->pins, &pin);
    }
    if (!sc_status_is_ok(status)) {
        hardware_pin_clear(&pin);
    } else {
        manifest->capabilities |= capabilities;
    }
    return status;
}

sc_status sc_hardware_manifest_resolve_pin(const sc_hardware_manifest *manifest,
                                           sc_str alias_or_pin,
                                           uint64_t required_capability,
                                           int *out_raw_pin)
{
    int raw_pin = 0;

    if (manifest == nullptr || out_raw_pin == nullptr || alias_or_pin.len == 0 || alias_or_pin.ptr == nullptr) {
        return sc_status_invalid_argument("sc.hardware.pin.invalid_argument");
    }
    for (size_t i = 0; i < manifest->pins.len; i += 1) {
        const sc_hardware_pin *pin = sc_vec_at_const(&manifest->pins, i);
        if (pin != nullptr && sc_str_equal(sc_string_as_str(&pin->alias), alias_or_pin)) {
            if ((pin->capabilities & required_capability) == 0) {
                return sc_status_security_denied("sc.hardware.pin.capability_denied");
            }
            *out_raw_pin = pin->raw_pin;
            return sc_status_ok();
        }
    }
    if (raw_pin_allowed(manifest, alias_or_pin, &raw_pin)) {
        *out_raw_pin = raw_pin;
        return sc_status_ok();
    }
    return sc_status_security_denied("sc.hardware.pin.alias_invalid");
}

void sc_hardware_manifest_clear(sc_hardware_manifest *manifest)
{
    if (manifest == nullptr) {
        return;
    }
    sc_string_clear(&manifest->alias);
    sc_string_clear(&manifest->board);
    sc_string_clear(&manifest->vendor_id);
    sc_string_clear(&manifest->product_id);
    sc_string_clear(&manifest->serial_number);
    sc_string_clear(&manifest->protocol);
    sc_string_clear(&manifest->config_schema_ref);
    sc_string_clear(&manifest->manual_test_requirements);
    for (size_t i = 0; i < manifest->pins.len; i += 1) {
        sc_hardware_pin *pin = sc_vec_at(&manifest->pins, i);
        hardware_pin_clear(pin);
    }
    sc_vec_clear(&manifest->pins);
    *manifest = (sc_hardware_manifest){0};
}

void sc_hardware_context_init(sc_hardware_context *context, sc_allocator *alloc)
{
    if (context == nullptr) {
        return;
    }
    *context = (sc_hardware_context){.alloc = alloc == nullptr ? sc_allocator_heap() : alloc};
    sc_vec_init(&context->devices, context->alloc, sizeof(sc_hardware_device));
}

void sc_hardware_context_set_observer(sc_hardware_context *context, sc_observer *observer)
{
    if (context != nullptr) {
        context->observer = observer;
    }
}

void sc_hardware_context_set_estop(sc_hardware_context *context, const sc_estop_state *estop)
{
    if (context != nullptr) {
        context->estop = estop;
    }
}

sc_status sc_hardware_context_add_device(sc_hardware_context *context,
                                         const sc_hardware_manifest *manifest,
                                         sc_peripheral *peripheral)
{
    sc_hardware_device device = {0};
    sc_status status;

    if (context == nullptr || manifest == nullptr || peripheral == nullptr) {
        return sc_status_invalid_argument("sc.hardware.context.invalid_argument");
    }
    status = manifest_clone(context->alloc, manifest, &device.manifest);
    if (sc_status_is_ok(status)) {
        device.peripheral = peripheral;
        status = sc_vec_push(&context->devices, &device);
    }
    if (!sc_status_is_ok(status)) {
        sc_hardware_manifest_clear(&device.manifest);
        return status;
    }
    context->enabled = true;
    return sc_status_ok();
}

sc_status sc_hardware_context_discover_fake(sc_hardware_context *context,
                                            const sc_hardware_discovery_options *options,
                                            sc_peripheral **out_peripheral)
{
    sc_serial_transport *serial = nullptr;
    sc_peripheral *peripheral = nullptr;
    sc_status status;

    if (context == nullptr || options == nullptr || options->manifest == nullptr || out_peripheral == nullptr) {
        return sc_status_invalid_argument("sc.hardware.discovery.invalid_argument");
    }
    *out_peripheral = nullptr;
    status = hardware_emit(context,
                           sc_str_from_cstr("hardware.discovery.start"),
                           sc_str_from_cstr("discovery"),
                           sc_str_from_cstr("start"),
                           sc_str_from_cstr(""));
    if (sc_status_is_ok(status) && options->manifest->discovery_mode == SC_HARDWARE_DISCOVERY_UNSAFE_CLAIM && !options->allow_unsafe_claim) {
        status = sc_status_security_denied("sc.hardware.discovery.unsafe_claim_denied");
    }
    if (sc_status_is_ok(status)) {
        status = hardware_validate_identity(options->manifest,
                                            options->expected_vendor_id,
                                            options->expected_product_id,
                                            options->expected_serial_number,
                                            options->expected_protocol);
    }
    if (sc_status_is_ok(status)) {
        status = sc_hardware_fake_serial_new(context->alloc, &options->serial_options, &serial);
    }
    if (sc_status_is_ok(status)) {
        status = sc_hardware_fake_board_new(context->alloc, options->manifest, serial, &peripheral);
        if (sc_status_is_ok(status)) {
            serial = nullptr;
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_hardware_context_add_device(context, options->manifest, peripheral);
    }
    if (sc_status_is_ok(status)) {
        *out_peripheral = peripheral;
        (void)hardware_emit(context,
                            sc_str_from_cstr("hardware.discovery.found"),
                            sc_str_from_cstr("discovery"),
                            sc_str_from_cstr("success"),
                            sc_str_from_cstr(""));
        return sc_status_ok();
    }
    (void)hardware_emit(context,
                        sc_str_from_cstr("hardware.discovery.denied"),
                        sc_str_from_cstr("discovery"),
                        sc_str_from_cstr("failure"),
                        sc_str_from_cstr(status.error_key == nullptr ? "" : status.error_key));
    sc_peripheral_destroy(peripheral);
    sc_serial_transport_destroy(serial);
    return status;
}

sc_hardware_device *sc_hardware_context_find_device(sc_hardware_context *context, sc_str alias)
{
    if (context == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < context->devices.len; i += 1) {
        sc_hardware_device *device = sc_vec_at(&context->devices, i);
        if (device != nullptr && sc_str_equal(sc_string_as_str(&device->manifest.alias), alias)) {
            return device;
        }
    }
    return nullptr;
}

const sc_hardware_device *sc_hardware_context_find_device_const(const sc_hardware_context *context, sc_str alias)
{
    if (context == nullptr) {
        return nullptr;
    }
    for (size_t i = 0; i < context->devices.len; i += 1) {
        const sc_hardware_device *device = sc_vec_at_const(&context->devices, i);
        if (device != nullptr && sc_str_equal(sc_string_as_str(&device->manifest.alias), alias)) {
            return device;
        }
    }
    return nullptr;
}

sc_status sc_hardware_context_prompt(const sc_hardware_context *context,
                                     sc_allocator *alloc,
                                     size_t max_bytes,
                                     sc_string *out)
{
    sc_string_builder builder = {0};
    sc_status status;

    if (context == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.hardware.context_prompt.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    if (max_bytes == 0) {
        max_bytes = 4096;
    }
    sc_string_builder_init(&builder, alloc);
    status = sc_string_builder_append_cstr(&builder, "Hardware devices:\n");
    for (size_t i = 0; sc_status_is_ok(status) && i < context->devices.len; i += 1) {
        const sc_hardware_device *device = sc_vec_at_const(&context->devices, i);
        status = append_context_device(device, &builder, max_bytes);
    }
    if (builder.bytes.len > max_bytes) {
        builder.bytes.len = max_bytes;
        if (builder.bytes.ptr != nullptr) {
            builder.bytes.ptr[builder.bytes.len] = 0;
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_finish(&builder, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

void sc_hardware_context_clear(sc_hardware_context *context)
{
    if (context == nullptr) {
        return;
    }
    for (size_t i = 0; i < context->devices.len; i += 1) {
        sc_hardware_device *device = sc_vec_at(&context->devices, i);
        if (device != nullptr) {
            if (device->peripheral != nullptr) {
                (void)sc_peripheral_safe_shutdown(device->peripheral);
            }
            sc_hardware_manifest_clear(&device->manifest);
        }
    }
    sc_vec_clear(&context->devices);
    *context = (sc_hardware_context){0};
}

void sc_hardware_registry_init(sc_peripheral_registry *registry, sc_allocator *alloc)
{
    sc_peripheral_registry_init(registry, alloc);
}

sc_status sc_hardware_registry_register_fake(sc_peripheral_registry *registry)
{
    return sc_peripheral_registry_register(registry, &fake_board_vtab, fake_board_factory);
}

bool sc_hardware_tools_enabled(const sc_hardware_context *context)
{
    return context != nullptr && context->enabled && context->devices.len > 0;
}

sc_status sc_hardware_validate_mutation(const sc_security_policy *policy,
                                        const sc_estop_state *estop,
                                        sc_str tool_name,
                                        bool *approval_required)
{
    sc_security_tool_request request = {
        .struct_size = sizeof(request),
        .tool_name = tool_name,
        .risk = SC_TOOL_RISK_SIDE_EFFECT,
    };
    return sc_security_validate_request(policy, estop, &request, approval_required);
}

bool sc_serial_transport_vtab_valid(const sc_serial_transport_vtab *vtab)
{
    return vtab != nullptr &&
           vtab->struct_size >= sizeof(*vtab) &&
           vtab->abi_major == SC_ABI_VERSION_MAJOR &&
           vtab->name != nullptr &&
           sc_contract_name_is_valid(sc_str_from_cstr(vtab->name)) &&
           vtab->exchange != nullptr &&
           vtab->destroy != nullptr;
}

sc_status sc_serial_transport_new(sc_allocator *alloc,
                                  const sc_serial_transport_vtab *vtab,
                                  void *impl,
                                  sc_serial_transport **out)
{
    sc_serial_transport *transport = nullptr;
    if (out == nullptr || !sc_serial_transport_vtab_valid(vtab)) {
        return sc_status_invalid_argument("sc.serial.invalid_vtab");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    transport = sc_alloc(alloc, sizeof(*transport), _Alignof(sc_serial_transport));
    if (transport == nullptr) {
        return sc_status_no_memory();
    }
    *transport = (sc_serial_transport){.alloc = alloc, .vtab = vtab, .impl = impl};
    *out = transport;
    return sc_status_ok();
}

sc_status sc_serial_transport_exchange(sc_serial_transport *transport,
                                       sc_str request_json,
                                       sc_allocator *alloc,
                                       sc_string *response_json)
{
    if (transport == nullptr || response_json == nullptr || transport->vtab == nullptr || transport->vtab->exchange == nullptr) {
        return sc_status_invalid_argument("sc.serial.invalid_argument");
    }
    return transport->vtab->exchange(transport->impl, request_json, alloc, response_json);
}

void sc_serial_transport_destroy(sc_serial_transport *transport)
{
    if (transport == nullptr) {
        return;
    }
    if (transport->vtab != nullptr && transport->vtab->destroy != nullptr) {
        transport->vtab->destroy(transport->impl);
    }
    sc_free(transport->alloc, transport, sizeof(*transport), _Alignof(sc_serial_transport));
}

sc_status sc_hardware_fake_serial_new(sc_allocator *alloc,
                                      const sc_hardware_fake_serial_options *options,
                                      sc_serial_transport **out)
{
    fake_serial *impl = nullptr;
    sc_status status;

    if (out == nullptr) {
        return sc_status_invalid_argument("sc.fake_serial.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(fake_serial));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (fake_serial){
        .alloc = alloc,
        .timeout_next = options != nullptr && options->timeout_next,
        .malformed_next = options != nullptr && options->malformed_next,
        .gpio_value = options != nullptr && options->gpio_value,
        .capabilities = options == nullptr || options->capabilities == 0 ? (SC_HARDWARE_CAP_GPIO_READ | SC_HARDWARE_CAP_GPIO_WRITE) : options->capabilities,
        .disappear_next = options != nullptr && options->disappear_next,
        .partial_read_next = options != nullptr && options->partial_read_next,
        .bad_checksum_next = options != nullptr && options->bad_checksum_next,
        .oversized_frame_next = options != nullptr && options->oversized_frame_next,
        .unknown_command_next = options != nullptr && options->unknown_command_next,
        .safe_shutdown_supported = options == nullptr || options->safe_shutdown_supported,
    };
    status = sc_serial_transport_new(alloc, &fake_serial_vtab, impl, out);
    if (!sc_status_is_ok(status)) {
        fake_serial_destroy(impl);
    }
    return status;
}

sc_status sc_hardware_protocol_ping(sc_serial_transport *transport, sc_allocator *alloc, sc_string *out)
{
    sc_string response = {0};
    sc_json_value *root = nullptr;
    sc_str pong_text = {0};
    sc_status status;

    status = sc_serial_transport_exchange(transport, sc_str_from_cstr("{\"op\":\"ping\"}"), alloc, &response);
    if (sc_status_is_ok(status)) {
        status = response_require_ok(sc_string_as_str(&response), alloc, &root);
    }
    if (sc_status_is_ok(status)) {
        sc_json_value *pong = sc_json_object_get(root, sc_str_from_cstr("pong"));
        if (pong == nullptr || !sc_json_as_str(pong, &pong_text)) {
            status = sc_status_parse("sc.hardware.protocol.ping_invalid");
        }
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(alloc, pong_text, out);
    }
    sc_json_destroy(root);
    sc_string_clear(&response);
    return status;
}

sc_status sc_hardware_protocol_capabilities(sc_serial_transport *transport, uint64_t *out_capabilities)
{
    sc_string response = {0};
    sc_json_value *root = nullptr;
    const sc_json_value *caps = nullptr;
    sc_status status;

    if (out_capabilities == nullptr) {
        return sc_status_invalid_argument("sc.hardware.protocol.invalid_argument");
    }
    status = sc_serial_transport_exchange(transport, sc_str_from_cstr("{\"op\":\"capabilities\"}"), sc_allocator_heap(), &response);
    if (sc_status_is_ok(status)) {
        status = response_require_ok(sc_string_as_str(&response), sc_allocator_heap(), &root);
    }
    caps = sc_status_is_ok(status) ? sc_json_object_get(root, sc_str_from_cstr("capabilities")) : nullptr;
    if (sc_status_is_ok(status)) {
        status = parse_capabilities(caps, out_capabilities);
    }
    sc_json_destroy(root);
    sc_string_clear(&response);
    return status;
}

sc_status sc_hardware_protocol_gpio_read(sc_serial_transport *transport, int pin, bool *out_value)
{
    char request[64] = {0};
    sc_string response = {0};
    sc_json_value *root = nullptr;
    sc_json_value *value = nullptr;
    int written = 0;
    sc_status status;

    if (out_value == nullptr || pin < 0) {
        return sc_status_invalid_argument("sc.hardware.protocol.gpio_read_invalid_argument");
    }
    written = snprintf(request, sizeof(request), "{\"op\":\"gpio_read\",\"pin\":%d}", pin);
    if (written < 0 || (size_t)written >= sizeof(request)) {
        return sc_status_invalid_argument("sc.hardware.protocol.gpio_read_invalid_argument");
    }
    status = sc_serial_transport_exchange(transport, sc_str_from_cstr(request), sc_allocator_heap(), &response);
    if (sc_status_is_ok(status)) {
        status = response_require_ok(sc_string_as_str(&response), sc_allocator_heap(), &root);
    }
    value = sc_status_is_ok(status) ? sc_json_object_get(root, sc_str_from_cstr("value")) : nullptr;
    if (sc_status_is_ok(status) && (value == nullptr || !sc_json_as_bool(value, out_value))) {
        status = sc_status_parse("sc.hardware.protocol.gpio_read_invalid");
    }
    sc_json_destroy(root);
    sc_string_clear(&response);
    return status;
}

sc_status sc_hardware_protocol_gpio_write(sc_serial_transport *transport, int pin, bool value)
{
    char request[80] = {0};
    sc_string response = {0};
    sc_json_value *root = nullptr;
    int written = 0;
    sc_status status;

    if (pin < 0) {
        return sc_status_invalid_argument("sc.hardware.protocol.gpio_write_invalid_argument");
    }
    written = snprintf(request, sizeof(request), "{\"op\":\"gpio_write\",\"pin\":%d,\"value\":%s}", pin, value ? "true" : "false");
    if (written < 0 || (size_t)written >= sizeof(request)) {
        return sc_status_invalid_argument("sc.hardware.protocol.gpio_write_invalid_argument");
    }
    status = sc_serial_transport_exchange(transport, sc_str_from_cstr(request), sc_allocator_heap(), &response);
    if (sc_status_is_ok(status)) {
        status = response_require_ok(sc_string_as_str(&response), sc_allocator_heap(), &root);
    }
    sc_json_destroy(root);
    sc_string_clear(&response);
    return status;
}

uint8_t sc_hardware_protocol_frame_checksum(sc_buf frame_without_checksum)
{
    uint8_t checksum = 0;
    for (size_t i = 0; i < frame_without_checksum.len; i += 1) {
        checksum = (uint8_t)(checksum + frame_without_checksum.ptr[i]);
    }
    return checksum;
}

sc_status sc_hardware_protocol_parse_frame(sc_buf frame, size_t max_payload_bytes, sc_hardware_frame *out)
{
    uint16_t payload_len = 0;
    uint8_t command_id = 0;
    uint8_t checksum = 0;

    if (out == nullptr || frame.ptr == nullptr || frame.len == 0) {
        return sc_status_invalid_argument("sc.hardware.protocol.frame.invalid_argument");
    }
    if (frame.len < 5) {
        return sc_status_parse("sc.hardware.protocol.frame.partial");
    }
    if (frame.ptr[0] != 0x5au) {
        return sc_status_parse("sc.hardware.protocol.frame.magic_invalid");
    }
    command_id = frame.ptr[1];
    if (command_id < SC_HARDWARE_PROTOCOL_COMMAND_PING || command_id > SC_HARDWARE_PROTOCOL_COMMAND_GPIO_WRITE) {
        return sc_status_unsupported("sc.hardware.protocol.frame.command_unknown");
    }
    payload_len = (uint16_t)(((uint16_t)frame.ptr[2] << 8u) | (uint16_t)frame.ptr[3]);
    if ((size_t)payload_len > max_payload_bytes) {
        return sc_status_security_denied("sc.hardware.protocol.frame.oversized");
    }
    if (frame.len != (size_t)payload_len + 5u) {
        return frame.len < (size_t)payload_len + 5u ? sc_status_parse("sc.hardware.protocol.frame.partial")
                                                    : sc_status_parse("sc.hardware.protocol.frame.length_invalid");
    }
    checksum = sc_hardware_protocol_frame_checksum(sc_buf_from_parts(frame.ptr, frame.len - 1u));
    if (checksum != frame.ptr[frame.len - 1u]) {
        return sc_status_parse("sc.hardware.protocol.frame.checksum_invalid");
    }
    *out = (sc_hardware_frame){
        .struct_size = sizeof(*out),
        .command_id = command_id,
        .payload = sc_buf_from_parts(frame.ptr + 4u, payload_len),
    };
    return sc_status_ok();
}

sc_status sc_hardware_fake_board_new(sc_allocator *alloc,
                                     const sc_hardware_manifest *manifest,
                                     sc_serial_transport *transport,
                                     sc_peripheral **out)
{
    fake_board *board = nullptr;
    sc_status status;

    if (out == nullptr || manifest == nullptr || transport == nullptr) {
        return sc_status_invalid_argument("sc.fake_board.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    board = sc_alloc(alloc, sizeof(*board), _Alignof(fake_board));
    if (board == nullptr) {
        return sc_status_no_memory();
    }
    *board = (fake_board){.alloc = alloc, .transport = transport};
    status = manifest_clone(alloc, manifest, &board->manifest);
    if (sc_status_is_ok(status)) {
        status = sc_peripheral_new(alloc, &fake_board_vtab, board, out);
    }
    if (!sc_status_is_ok(status)) {
        fake_board_destroy(board);
    }
    return status;
}

sc_status sc_tool_hardware_gpio_read_new(sc_allocator *alloc,
                                         const sc_tool_context *tool_context,
                                         sc_hardware_context *hardware,
                                         sc_tool **out)
{
    return hardware_tool_new(alloc, tool_context, hardware, false, out);
}

sc_status sc_tool_hardware_gpio_write_new(sc_allocator *alloc,
                                          const sc_tool_context *tool_context,
                                          sc_hardware_context *hardware,
                                          sc_tool **out)
{
    return hardware_tool_new(alloc, tool_context, hardware, true, out);
}

// cppcheck-suppress constParameterPointer
sc_status sc_hardware_tools_register(sc_tool_registry *registry,
                                     const sc_tool_context *tool_context,
                                     // cppcheck-suppress constParameterPointer
                                     sc_hardware_context *hardware)
{
    (void)tool_context;
    if (registry == nullptr || hardware == nullptr) {
        return sc_status_invalid_argument("sc.hardware_tools.register_invalid_argument");
    }
    if (!sc_hardware_tools_enabled(hardware)) {
        return sc_status_security_denied("sc.hardware_tools.disabled");
    }
    /*
     * The generic registry currently stores stateless factories only. Hardware
     * tools require a live context, so callers create handles with the explicit
     * constructors and can still pass those handles through model-spec registry
     * helpers. This API marks the validated registration boundary for future
     * stateful registries without leaking direct runtime access.
     */
    return sc_status_ok();
}

static sc_status copy_string(sc_allocator *alloc, sc_str input, sc_string *out)
{
    return sc_string_from_str(alloc, empty_if_null(input), out);
}

static sc_status copy_optional_string(sc_allocator *alloc, const sc_json_value *object, sc_str key, sc_string *out)
{
    sc_str value = {0};
    sc_status status = json_get_optional_str(object, key, &value);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    return copy_string(alloc, value, out);
}

static sc_str empty_if_null(sc_str value)
{
    return value.ptr == nullptr ? sc_str_from_cstr("") : value;
}

static void hardware_pin_clear(sc_hardware_pin *pin)
{
    if (pin == nullptr) {
        return;
    }
    sc_string_clear(&pin->alias);
    *pin = (sc_hardware_pin){0};
}

static sc_status manifest_clone(sc_allocator *alloc, const sc_hardware_manifest *source, sc_hardware_manifest *out)
{
    sc_status status;
    if (source == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.hardware.manifest.clone_invalid_argument");
    }
    sc_hardware_manifest_init(out, alloc);
    out->allow_raw_pins = source->allow_raw_pins;
    out->capabilities = source->capabilities;
    out->device_class = source->device_class;
    out->discovery_mode = source->discovery_mode;
    out->safe_shutdown_supported = source->safe_shutdown_supported;
    status = copy_string(out->alloc, sc_string_as_str(&source->alias), &out->alias);
    if (sc_status_is_ok(status)) {
        status = copy_string(out->alloc, sc_string_as_str(&source->board), &out->board);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(out->alloc, sc_string_as_str(&source->vendor_id), &out->vendor_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(out->alloc, sc_string_as_str(&source->product_id), &out->product_id);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(out->alloc, sc_string_as_str(&source->serial_number), &out->serial_number);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(out->alloc, sc_string_as_str(&source->protocol), &out->protocol);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(out->alloc, sc_string_as_str(&source->config_schema_ref), &out->config_schema_ref);
    }
    if (sc_status_is_ok(status)) {
        status = copy_string(out->alloc, sc_string_as_str(&source->manual_test_requirements), &out->manual_test_requirements);
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < source->pins.len; i += 1) {
        const sc_hardware_pin *pin = sc_vec_at_const(&source->pins, i);
        if (pin != nullptr) {
            status = sc_hardware_manifest_add_pin(out, sc_string_as_str(&pin->alias), pin->raw_pin, pin->capabilities);
        }
    }
    if (!sc_status_is_ok(status)) {
        sc_hardware_manifest_clear(out);
    }
    return status;
}

static bool str_present(sc_str value)
{
    return value.ptr != nullptr && value.len > 0;
}

static bool str_matches_if_present(sc_str expected, sc_str actual)
{
    return !str_present(expected) || sc_str_equal(expected, actual);
}

static uint64_t capability_from_name(sc_str name)
{
    if (sc_str_equal(name, sc_str_from_cstr("gpio_read"))) {
        return SC_HARDWARE_CAP_GPIO_READ;
    }
    if (sc_str_equal(name, sc_str_from_cstr("gpio_write"))) {
        return SC_HARDWARE_CAP_GPIO_WRITE;
    }
    if (sc_str_equal(name, sc_str_from_cstr("context"))) {
        return SC_HARDWARE_CAP_CONTEXT;
    }
    if (sc_str_equal(name, sc_str_from_cstr("serial"))) {
        return SC_HARDWARE_CAP_SERIAL;
    }
    return 0;
}

static sc_hardware_device_class device_class_from_name(sc_str name)
{
    if (sc_str_equal(name, sc_str_from_cstr("board"))) {
        return SC_HARDWARE_DEVICE_CLASS_BOARD;
    }
    if (sc_str_equal(name, sc_str_from_cstr("sensor"))) {
        return SC_HARDWARE_DEVICE_CLASS_SENSOR;
    }
    if (sc_str_equal(name, sc_str_from_cstr("actuator"))) {
        return SC_HARDWARE_DEVICE_CLASS_ACTUATOR;
    }
    return SC_HARDWARE_DEVICE_CLASS_UNKNOWN;
}

static sc_hardware_discovery_mode discovery_mode_from_name(sc_str name)
{
    if (sc_str_equal(name, sc_str_from_cstr("configured"))) {
        return SC_HARDWARE_DISCOVERY_CONFIGURED;
    }
    if (sc_str_equal(name, sc_str_from_cstr("unsafe_claim"))) {
        return SC_HARDWARE_DISCOVERY_UNSAFE_CLAIM;
    }
    return SC_HARDWARE_DISCOVERY_PASSIVE;
}

static sc_status parse_capabilities(const sc_json_value *value, uint64_t *out)
{
    uint64_t caps = 0;
    if (out == nullptr || value == nullptr || sc_json_type_of(value) != SC_JSON_ARRAY) {
        return sc_status_parse("sc.hardware.capabilities.invalid");
    }
    for (size_t i = 0; i < sc_json_array_len(value); i += 1) {
        sc_json_value *item = sc_json_array_get(value, i);
        sc_str text = {0};
        uint64_t cap = 0;
        if (item == nullptr || !sc_json_as_str(item, &text)) {
            return sc_status_parse("sc.hardware.capability.invalid");
        }
        cap = capability_from_name(text);
        if (cap == 0) {
            return sc_status_parse("sc.hardware.capability.unknown");
        }
        caps |= cap;
    }
    *out = caps;
    return sc_status_ok();
}

static sc_status json_get_required_str(const sc_json_value *object, sc_str key, sc_str *out)
{
    sc_json_value *value = sc_json_object_get(object, key);
    if (value == nullptr || !sc_json_as_str(value, out) || out->len == 0) {
        return sc_status_parse("sc.hardware.json.string_required");
    }
    return sc_status_ok();
}

static sc_status json_get_optional_bool(const sc_json_value *object, sc_str key, bool default_value, bool *out)
{
    sc_json_value *value = sc_json_object_get(object, key);
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.hardware.json.bool_invalid_argument");
    }
    if (value == nullptr) {
        *out = default_value;
        return sc_status_ok();
    }
    if (!sc_json_as_bool(value, out)) {
        return sc_status_parse("sc.hardware.json.bool_invalid");
    }
    return sc_status_ok();
}

static sc_status json_get_optional_str(const sc_json_value *object, sc_str key, sc_str *out)
{
    sc_json_value *value = sc_json_object_get(object, key);
    if (out == nullptr) {
        return sc_status_invalid_argument("sc.hardware.json.string_invalid_argument");
    }
    *out = (sc_str){0};
    if (value == nullptr) {
        return sc_status_ok();
    }
    if (!sc_json_as_str(value, out)) {
        return sc_status_parse("sc.hardware.json.string_invalid");
    }
    return sc_status_ok();
}

static sc_status json_get_required_int(const sc_json_value *object, sc_str key, int *out)
{
    sc_json_value *value = sc_json_object_get(object, key);
    double number = 0.0;
    if (out == nullptr || value == nullptr || !sc_json_as_number(value, &number) || number < 0.0 || number > 100000.0) {
        return sc_status_parse("sc.hardware.json.int_required");
    }
    *out = (int)number;
    return sc_status_ok();
}

static bool str_to_int(sc_str text, int *out)
{
    int value = 0;
    if (text.len == 0 || text.ptr == nullptr || out == nullptr) {
        return false;
    }
    for (size_t i = 0; i < text.len; i += 1) {
        unsigned char ch = (unsigned char)text.ptr[i];
        if (!isdigit(ch)) {
            return false;
        }
        if (value > 100000) {
            return false;
        }
        value = (value * 10) + (int)(ch - '0');
    }
    *out = value;
    return true;
}

static bool raw_pin_allowed(const sc_hardware_manifest *manifest, sc_str input, int *out)
{
    int pin = 0;
    if (manifest == nullptr || !manifest->allow_raw_pins || !str_to_int(input, &pin)) {
        return false;
    }
    for (size_t i = 0; i < manifest->pins.len; i += 1) {
        const sc_hardware_pin *known = sc_vec_at_const(&manifest->pins, i);
        if (known != nullptr && known->raw_pin == pin) {
            *out = pin;
            return true;
        }
    }
    return false;
}

static sc_status append_context_device(const sc_hardware_device *device, sc_string_builder *builder, size_t max_bytes)
{
    sc_status status;
    char pin_text[64] = {0};
    if (device == nullptr || builder == nullptr || builder->bytes.len >= max_bytes) {
        return sc_status_ok();
    }
    status = sc_string_builder_append_cstr(builder, "- ");
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(builder, sc_string_as_str(&device->manifest.alias));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, " (");
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append(builder, sc_string_as_str(&device->manifest.board));
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "): ");
    }
    for (size_t i = 0; sc_status_is_ok(status) && i < device->manifest.pins.len; i += 1) {
        const sc_hardware_pin *pin = sc_vec_at_const(&device->manifest.pins, i);
        if (pin == nullptr) {
            continue;
        }
        int written = snprintf(pin_text, sizeof(pin_text), "%s%d", i == 0 ? "" : ", ", pin->raw_pin);
        if (written < 0 || (size_t)written >= sizeof(pin_text)) {
            return sc_status_io("sc.hardware.context.format_failed");
        }
        status = sc_string_builder_append(builder, sc_string_as_str(&pin->alias));
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(builder, "=");
        }
        if (sc_status_is_ok(status)) {
            status = sc_string_builder_append_cstr(builder, pin_text);
        }
    }
    if (sc_status_is_ok(status)) {
        status = sc_string_builder_append_cstr(builder, "\n");
    }
    return status;
}

static sc_status serial_common_response(sc_allocator *alloc, sc_str body, sc_string *out)
{
    return copy_string(alloc, body, out);
}

static sc_status hardware_emit(sc_hardware_context *context, sc_str name, sc_str operation, sc_str outcome, sc_str error_key)
{
    size_t failures = 0;
    sc_observer_event event = {
        .struct_size = sizeof(event),
        .target = sc_str_from_cstr("hardware"),
        .name = name,
        .kind = sc_str_equal(outcome, sc_str_from_cstr("failure")) ? SC_OBSERVER_EVENT_AUDIT : SC_OBSERVER_EVENT_RUNTIME,
        .subsystem = sc_str_from_cstr("hardware"),
        .operation = operation,
        .outcome = outcome,
        .error_key = error_key,
        .redacted_target = sc_str_from_cstr("hardware.device"),
    };
    if (context == nullptr || context->observer == nullptr) {
        return sc_status_ok();
    }
    (void)sc_observer_emit_safe(context->observer, &event, &failures);
    context->observer_failures += failures;
    return sc_status_ok();
}

static sc_status hardware_emit_command(const sc_peripheral_command *command,
                                       sc_str name,
                                       sc_str operation,
                                       sc_str outcome,
                                       sc_str error_key)
{
    size_t failures = 0;
    sc_observer_event event = {
        .struct_size = sizeof(event),
        .target = sc_str_from_cstr("hardware"),
        .name = name,
        .kind = command != nullptr && command->destructive ? SC_OBSERVER_EVENT_AUDIT : SC_OBSERVER_EVENT_RUNTIME,
        .subsystem = sc_str_from_cstr("hardware"),
        .operation = operation,
        .outcome = outcome,
        .error_key = error_key,
        .redacted_target = sc_str_from_cstr("hardware.command"),
    };
    if (command == nullptr || command->observer == nullptr) {
        return sc_status_ok();
    }
    (void)sc_observer_emit_safe(command->observer, &event, &failures);
    return sc_status_ok();
}

static sc_status hardware_validate_identity(const sc_hardware_manifest *manifest,
                                            sc_str expected_vendor_id,
                                            sc_str expected_product_id,
                                            sc_str expected_serial_number,
                                            sc_str expected_protocol)
{
    if (manifest == nullptr) {
        return sc_status_invalid_argument("sc.hardware.identity.invalid_argument");
    }
    if (!str_matches_if_present(expected_vendor_id, sc_string_as_str(&manifest->vendor_id)) ||
        !str_matches_if_present(expected_product_id, sc_string_as_str(&manifest->product_id)) ||
        !str_matches_if_present(expected_serial_number, sc_string_as_str(&manifest->serial_number)) ||
        !str_matches_if_present(expected_protocol, sc_string_as_str(&manifest->protocol))) {
        return sc_status_security_denied("sc.hardware.identity.mismatch");
    }
    return sc_status_ok();
}

static sc_status fake_serial_exchange(void *impl, sc_str request_json, sc_allocator *alloc, sc_string *response_json)
{
    fake_serial *serial = impl;
    sc_json_value *root = nullptr;
    sc_json_parse_error error = {0};
    sc_json_value *op_value = nullptr;
    sc_str op = {0};
    sc_status status;
    char response[128] = {0};

    if (serial == nullptr || response_json == nullptr) {
        return sc_status_invalid_argument("sc.fake_serial.invalid_argument");
    }
    if (serial->timeout_next) {
        serial->timeout_next = false;
        return sc_status_timeout("sc.fake_serial.timeout");
    }
    if (serial->disappear_next) {
        serial->disappear_next = false;
        return sc_status_io("sc.fake_serial.disappeared");
    }
    if (serial->partial_read_next) {
        uint8_t frame[] = {0x5au, SC_HARDWARE_PROTOCOL_COMMAND_PING, 0x00u};
        sc_hardware_frame parsed = {0};
        serial->partial_read_next = false;
        return sc_hardware_protocol_parse_frame(sc_buf_from_parts(frame, sizeof(frame)), 16, &parsed);
    }
    if (serial->bad_checksum_next) {
        uint8_t frame[] = {0x5au, SC_HARDWARE_PROTOCOL_COMMAND_PING, 0x00u, 0x00u, 0xffu};
        sc_hardware_frame parsed = {0};
        serial->bad_checksum_next = false;
        return sc_hardware_protocol_parse_frame(sc_buf_from_parts(frame, sizeof(frame)), 16, &parsed);
    }
    if (serial->oversized_frame_next) {
        uint8_t frame[] = {0x5au, SC_HARDWARE_PROTOCOL_COMMAND_GPIO_READ, 0x00u, 0x20u, 0x00u};
        sc_hardware_frame parsed = {0};
        serial->oversized_frame_next = false;
        return sc_hardware_protocol_parse_frame(sc_buf_from_parts(frame, sizeof(frame)), 16, &parsed);
    }
    if (serial->unknown_command_next) {
        uint8_t frame[] = {0x5au, 0x7fu, 0x00u, 0x00u, 0xd9u};
        sc_hardware_frame parsed = {0};
        serial->unknown_command_next = false;
        return sc_hardware_protocol_parse_frame(sc_buf_from_parts(frame, sizeof(frame)), 16, &parsed);
    }
    if (serial->malformed_next) {
        serial->malformed_next = false;
        return serial_common_response(alloc, sc_str_from_cstr("{\"status\":\"ok\","), response_json);
    }
    status = sc_json_parse(alloc, request_json, &root, &error);
    if (!sc_status_is_ok(status)) {
        return status;
    }
    op_value = sc_json_object_get(root, sc_str_from_cstr("op"));
    if (op_value == nullptr || !sc_json_as_str(op_value, &op)) {
        sc_json_destroy(root);
        return sc_status_parse("sc.fake_serial.op_missing");
    }
    if (sc_str_equal(op, sc_str_from_cstr("ping"))) {
        status = serial_common_response(alloc, sc_str_from_cstr("{\"status\":\"ok\",\"pong\":\"pong\"}"), response_json);
    } else if (sc_str_equal(op, sc_str_from_cstr("capabilities"))) {
        status = serial_common_response(alloc, sc_str_from_cstr("{\"status\":\"ok\",\"capabilities\":[\"gpio_read\",\"gpio_write\"]}"), response_json);
    } else if (sc_str_equal(op, sc_str_from_cstr("gpio_read"))) {
        int written = snprintf(response, sizeof(response), "{\"status\":\"ok\",\"value\":%s}", serial->gpio_value ? "true" : "false");
        status = written < 0 || (size_t)written >= sizeof(response) ? sc_status_io("sc.fake_serial.format_failed") : serial_common_response(alloc, sc_str_from_cstr(response), response_json);
    } else if (sc_str_equal(op, sc_str_from_cstr("gpio_write"))) {
        sc_json_value *value = sc_json_object_get(root, sc_str_from_cstr("value"));
        if (value == nullptr || !sc_json_as_bool(value, &serial->gpio_value)) {
            status = sc_status_parse("sc.fake_serial.value_invalid");
        } else {
            status = serial_common_response(alloc, sc_str_from_cstr("{\"status\":\"ok\"}"), response_json);
        }
    } else {
        status = serial_common_response(alloc, sc_str_from_cstr("{\"status\":\"error\",\"error\":\"unsupported\"}"), response_json);
    }
    sc_json_destroy(root);
    return status;
}

static void fake_serial_destroy(void *impl)
{
    fake_serial *serial = impl;
    if (serial == nullptr) {
        return;
    }
    sc_free(serial->alloc, serial, sizeof(*serial), _Alignof(fake_serial));
}

static sc_status response_require_ok(sc_str response_json, sc_allocator *alloc, sc_json_value **out_root)
{
    sc_json_parse_error error = {0};
    sc_json_value *root = nullptr;
    sc_json_value *status_value = nullptr;
    sc_str status_text = {0};
    sc_status status;

    if (out_root == nullptr) {
        return sc_status_invalid_argument("sc.hardware.protocol.invalid_argument");
    }
    if (response_json.len > 4096) {
        return sc_status_security_denied("sc.hardware.protocol.response_oversized");
    }
    status = sc_json_parse(alloc, response_json, &root, &error);
    if (!sc_status_is_ok(status)) {
        return sc_status_parse("sc.hardware.protocol.malformed_response");
    }
    status_value = sc_json_object_get(root, sc_str_from_cstr("status"));
    if (status_value == nullptr || !sc_json_as_str(status_value, &status_text)) {
        sc_json_destroy(root);
        return sc_status_parse("sc.hardware.protocol.status_missing");
    }
    if (!sc_str_equal(status_text, sc_str_from_cstr("ok"))) {
        sc_json_destroy(root);
        return sc_status_io("sc.hardware.protocol.error_response");
    }
    *out_root = root;
    return sc_status_ok();
}

static sc_status fake_board_health(void *impl, sc_allocator *alloc, sc_peripheral_health *out)
{
    const fake_board *board = impl;
    sc_string pong = {0};
    sc_status status;
    if (board == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.fake_board.health_invalid_argument");
    }
    status = sc_hardware_protocol_ping(board->transport, alloc, &pong);
    if (sc_status_is_ok(status)) {
        *out = (sc_peripheral_health){.struct_size = sizeof(*out), .healthy = true};
        status = copy_string(alloc, sc_string_as_str(&pong), &out->message);
    }
    sc_string_clear(&pong);
    return status;
}

static sc_status fake_board_command(void *impl,
                                    const sc_peripheral_command *command,
                                    sc_allocator *alloc,
                                    sc_peripheral_result *out)
{
    fake_board *board = impl;
    sc_json_parse_error error = {0};
    sc_json_value *payload = nullptr;
    int pin = 0;
    bool value = false;
    sc_status status;

    if (board == nullptr || command == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.fake_board.command_invalid_argument");
    }
    if (command->cancel_token != nullptr && command->cancel_token->cancel_requested) {
        (void)hardware_emit_command(command,
                                    sc_str_from_cstr("hardware.command.cancelled"),
                                    command->operation,
                                    sc_str_from_cstr("cancelled"),
                                    sc_str_from_cstr("sc.hardware.command.cancelled"));
        return sc_status_cancelled("sc.hardware.command.cancelled");
    }
    if (command->destructive && command->estop != nullptr) {
        status = sc_estop_check(command->estop);
        if (!sc_status_is_ok(status)) {
            (void)fake_board_safe_shutdown(board);
            (void)hardware_emit_command(command,
                                        sc_str_from_cstr("hardware.estop.denied"),
                                        command->operation,
                                        sc_str_from_cstr("failure"),
                                        sc_str_from_cstr(status.error_key == nullptr ? "sc.hardware.estop.active" : status.error_key));
            return status;
        }
    }
    status = hardware_validate_identity(&board->manifest,
                                        command->expected_vendor_id,
                                        command->expected_product_id,
                                        command->expected_serial_number,
                                        sc_str_from_cstr(""));
    if (!sc_status_is_ok(status)) {
        (void)hardware_emit_command(command,
                                    sc_str_from_cstr("hardware.identity.denied"),
                                    command->operation,
                                    sc_str_from_cstr("failure"),
                                    sc_str_from_cstr(status.error_key == nullptr ? "sc.hardware.identity.mismatch" : status.error_key));
        return status;
    }
    if (command->payload.len == 0 || command->payload.ptr == nullptr) {
        return sc_status_parse("sc.fake_board.payload_missing");
    }
    status = sc_json_parse(alloc, sc_str_from_parts((const char *)command->payload.ptr, command->payload.len), &payload, &error);
    if (sc_status_is_ok(status)) {
        status = json_get_required_int(payload, sc_str_from_cstr("pin"), &pin);
    }
    if (sc_status_is_ok(status) && sc_str_equal(command->operation, sc_str_from_cstr("gpio_read"))) {
        status = sc_hardware_protocol_gpio_read(board->transport, pin, &value);
        if (sc_status_is_ok(status)) {
            char response[32] = {0};
            int written = snprintf(response, sizeof(response), "{\"value\":%s}", value ? "true" : "false");
            status = written < 0 || (size_t)written >= sizeof(response) ? sc_status_io("sc.fake_board.format_failed") : sc_bytes_from_buf(alloc, sc_buf_from_parts(response, (size_t)written), &out->payload);
        }
    } else if (sc_status_is_ok(status) && sc_str_equal(command->operation, sc_str_from_cstr("gpio_write"))) {
        sc_json_value *value_json = sc_json_object_get(payload, sc_str_from_cstr("value"));
        if (value_json == nullptr || !sc_json_as_bool(value_json, &value)) {
            status = sc_status_parse("sc.fake_board.value_invalid");
        } else {
            status = sc_hardware_protocol_gpio_write(board->transport, pin, value);
            if (sc_status_is_ok(status)) {
                status = sc_bytes_from_buf(alloc, sc_buf_from_parts("{\"ok\":true}", strlen("{\"ok\":true}")), &out->payload);
            }
        }
    } else if (sc_status_is_ok(status)) {
        status = sc_status_unsupported("sc.fake_board.command_unsupported");
    }
    sc_json_destroy(payload);
    if (sc_status_is_ok(status)) {
        out->struct_size = sizeof(*out);
        (void)hardware_emit_command(command,
                                    sc_str_from_cstr("hardware.command.complete"),
                                    command->operation,
                                    sc_str_from_cstr("success"),
                                    sc_str_from_cstr(""));
    } else {
        (void)hardware_emit_command(command,
                                    sc_str_from_cstr("hardware.command.failed"),
                                    command->operation,
                                    sc_str_from_cstr("failure"),
                                    sc_str_from_cstr(status.error_key == nullptr ? "sc.hardware.command.failed" : status.error_key));
    }
    return status;
}

static sc_status fake_board_context(void *impl, sc_allocator *alloc, sc_peripheral_context *out)
{
    const fake_board *board = impl;
    sc_status status;
    sc_string_builder builder = {0};
    sc_hardware_device device = {0};
    if (board == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.fake_board.context_invalid_argument");
    }
    sc_string_builder_init(&builder, alloc);
    device.manifest = board->manifest;
    status = sc_string_builder_append_cstr(&builder, "Hardware devices:\n");
    if (sc_status_is_ok(status)) {
        status = append_context_device(&device, &builder, 1024);
    }
    if (sc_status_is_ok(status)) {
        *out = (sc_peripheral_context){.struct_size = sizeof(*out)};
        status = sc_string_builder_finish(&builder, &out->text);
    }
    if (!sc_status_is_ok(status)) {
        sc_string_builder_clear(&builder);
    }
    return status;
}

static sc_status fake_board_safe_shutdown(void *impl)
{
    fake_board *board = impl;
    fake_serial *serial = nullptr;
    if (board == nullptr || board->transport == nullptr) {
        return sc_status_invalid_argument("sc.fake_board.safe_shutdown_invalid_argument");
    }
    serial = board->transport->impl;
    if (serial == nullptr || !serial->safe_shutdown_supported) {
        return sc_status_unsupported("sc.fake_board.safe_shutdown_unsupported");
    }
    serial->gpio_value = false;
    serial->safe_shutdown_called = true;
    return sc_status_ok();
}

static void fake_board_destroy(void *impl)
{
    fake_board *board = impl;
    if (board == nullptr) {
        return;
    }
    sc_hardware_manifest_clear(&board->manifest);
    sc_serial_transport_destroy(board->transport);
    sc_free(board->alloc, board, sizeof(*board), _Alignof(fake_board));
}

static sc_status fake_board_factory(sc_allocator *alloc, sc_peripheral **out)
{
    static const char manifest_json[] =
        "{\"alias\":\"fake\",\"board\":\"fake-board\",\"pins\":[{\"alias\":\"led\",\"pin\":13,\"capabilities\":[\"gpio_read\",\"gpio_write\"]}]}";
    sc_hardware_manifest manifest = {0};
    sc_serial_transport *serial = nullptr;
    sc_status status = sc_hardware_manifest_parse(alloc, sc_str_from_cstr(manifest_json), &manifest);
    if (sc_status_is_ok(status)) {
        status = sc_hardware_fake_serial_new(alloc, nullptr, &serial);
    }
    if (sc_status_is_ok(status)) {
        status = sc_hardware_fake_board_new(alloc, &manifest, serial, out);
    }
    if (!sc_status_is_ok(status)) {
        sc_serial_transport_destroy(serial);
    }
    sc_hardware_manifest_clear(&manifest);
    return status;
}

static sc_status hardware_tool_spec(void *impl, sc_tool_spec *out)
{
    hardware_tool *tool = impl;
    if (tool == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.hardware_tool.spec_invalid_argument");
    }
    *out = (sc_tool_spec){
        .struct_size = sizeof(*out),
        .name = tool->write ? sc_str_from_cstr("hardware.gpio_write") : sc_str_from_cstr("hardware.gpio_read"),
        .description = tool->write ? sc_str_from_cstr("tool.hardware.gpio_write.description") : sc_str_from_cstr("tool.hardware.gpio_read.description"),
        .input_schema = tool->context.schema,
        .capabilities = SC_CONTRACT_CAP_SECURE,
        .risk = tool->write ? SC_TOOL_RISK_SIDE_EFFECT : SC_TOOL_RISK_READONLY,
        .capability_category = SC_TOOL_CAPABILITY_HARDWARE,
        .side_effect = tool->write ? SC_TOOL_SIDE_EFFECT_WRITE : SC_TOOL_SIDE_EFFECT_READ,
        .default_autonomy = SC_AUTONOMY_SUPERVISED,
        .catalog_metadata_key = tool->write ? sc_str_from_cstr("tool.hardware.gpio_write.metadata") : sc_str_from_cstr("tool.hardware.gpio_read.metadata"),
    };
    return sc_status_ok();
}

static sc_status hardware_tool_invoke(void *impl, const sc_tool_call *call, sc_allocator *alloc, sc_tool_result *out)
{
    hardware_tool *tool = impl;
    sc_str device_alias = {0};
    sc_str pin_alias = {0};
    sc_str value_text = {0};
    sc_hardware_device *device = nullptr;
    sc_peripheral_result result = {0};
    char payload[96] = {0};
    int raw_pin = 0;
    bool value = false;
    int written = 0;
    sc_str tool_name = tool != nullptr && tool->write ? sc_str_from_cstr("hardware.gpio_write") : sc_str_from_cstr("hardware.gpio_read");
    sc_status status;

    if (tool == nullptr || call == nullptr || out == nullptr) {
        return sc_status_invalid_argument("sc.hardware_tool.invoke_invalid_argument");
    }
    status = sc_hardware_tools_enabled(tool->hardware) ? sc_status_ok() : sc_status_security_denied("sc.hardware_tool.disabled");
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("device"), &device_alias);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("pin"), &pin_alias);
    }
    if (sc_status_is_ok(status) && tool->write) {
        status = sc_tool_get_string_arg(call, sc_str_from_cstr("value"), &value_text);
        if (sc_status_is_ok(status)) {
            if (sc_str_equal(value_text, sc_str_from_cstr("true"))) {
                value = true;
            } else if (sc_str_equal(value_text, sc_str_from_cstr("false"))) {
                value = false;
            } else {
                status = sc_status_parse("sc.hardware_tool.value_invalid");
            }
        }
    }
    device = sc_status_is_ok(status) ? sc_hardware_context_find_device(tool->hardware, device_alias) : nullptr;
    if (sc_status_is_ok(status) && device == nullptr) {
        status = sc_status_security_denied("sc.hardware_tool.device_unknown");
    }
    if (sc_status_is_ok(status)) {
        status = sc_hardware_manifest_resolve_pin(&device->manifest,
                                                  pin_alias,
                                                  tool->write ? SC_HARDWARE_CAP_GPIO_WRITE : SC_HARDWARE_CAP_GPIO_READ,
                                                  &raw_pin);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_security_check_ex(&tool->context,
                                           tool_name,
                                           tool->write ? SC_TOOL_RISK_SIDE_EFFECT : SC_TOOL_RISK_READONLY,
                                           sc_str_from_cstr(""),
                                           false,
                                           sc_str_from_cstr(""),
                                           sc_str_from_cstr(""),
                                           device_alias,
                                           sc_str_from_cstr(""));
        if (!sc_status_is_ok(status) && tool->write && device != nullptr) {
            (void)sc_peripheral_safe_shutdown(device->peripheral);
            (void)hardware_emit(tool->hardware,
                                sc_str_from_cstr("hardware.estop.denied"),
                                sc_str_from_cstr("gpio_write"),
                                sc_str_from_cstr("failure"),
                                sc_str_from_cstr(status.error_key == nullptr ? "sc.hardware.estop.active" : status.error_key));
        }
    }
    if (sc_status_is_ok(status)) {
        written = tool->write ? snprintf(payload, sizeof(payload), "{\"pin\":%d,\"value\":%s}", raw_pin, value ? "true" : "false") : snprintf(payload, sizeof(payload), "{\"pin\":%d}", raw_pin);
        if (written < 0 || (size_t)written >= sizeof(payload)) {
            status = sc_status_io("sc.hardware_tool.payload_format_failed");
        }
    }
    if (sc_status_is_ok(status)) {
        sc_peripheral_command command = {
            .struct_size = sizeof(command),
            .operation = tool->write ? sc_str_from_cstr("gpio_write") : sc_str_from_cstr("gpio_read"),
            .payload = sc_buf_from_parts(payload, (size_t)written),
            .timeout_ms = tool->context.context.timeout_ms,
            .cancel_token = call->cancel_token != nullptr ? call->cancel_token : tool->context.context.cancel_token,
            .estop = tool->context.context.estop != nullptr ? tool->context.context.estop : tool->hardware->estop,
            .observer = tool->hardware->observer,
            .destructive = tool->write,
            .expected_vendor_id = sc_string_as_str(&device->manifest.vendor_id),
            .expected_product_id = sc_string_as_str(&device->manifest.product_id),
            .expected_serial_number = sc_string_as_str(&device->manifest.serial_number),
        };
        status = sc_peripheral_command_send(device->peripheral, &command, alloc, &result);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_set_output(alloc,
                                    &tool->context,
                                    out,
                                    sc_str_from_parts((const char *)result.payload.ptr, result.payload.len),
                                    true);
    }
    {
        char summary[128] = {0};
        int summary_len = snprintf(summary,
                                   sizeof(summary),
                                   "device=%.*s pin=%.*s raw=%d",
                                   (int)device_alias.len,
                                   device_alias.ptr == nullptr ? "" : device_alias.ptr,
                                   (int)pin_alias.len,
                                   pin_alias.ptr == nullptr ? "" : pin_alias.ptr,
                                   raw_pin);
        if (summary_len < 0 || (size_t)summary_len >= sizeof(summary)) {
            if (sc_status_is_ok(status)) {
                status = sc_status_io("sc.hardware_tool.summary_format_failed");
            }
        } else {
            sc_str output_summary = sc_status_is_ok(status) ? sc_str_from_parts((const char *)result.payload.ptr, result.payload.len) :
                                                              sc_str_from_cstr("error");
            (void)sc_tool_record_receipt_status(&tool->context,
                                                tool_name,
                                                sc_str_from_parts(summary, (size_t)summary_len),
                                                output_summary,
                                                sc_status_is_ok(status),
                                                status);
        }
    }
    sc_peripheral_result_clear(&result);
    return status;
}

static void hardware_tool_destroy(void *impl)
{
    hardware_tool *tool = impl;
    if (tool == nullptr) {
        return;
    }
    sc_tool_impl_context_clear(&tool->context);
    sc_free(tool->context.alloc, tool, sizeof(*tool), _Alignof(hardware_tool));
}

static sc_status hardware_tool_new(sc_allocator *alloc,
                                   const sc_tool_context *tool_context,
                                   sc_hardware_context *hardware,
                                   bool write,
                                   sc_tool **out)
{
    hardware_tool *impl = nullptr;
    sc_status status;

    if (out == nullptr || tool_context == nullptr || hardware == nullptr) {
        return sc_status_invalid_argument("sc.hardware_tool.invalid_argument");
    }
    alloc = alloc == nullptr ? sc_allocator_heap() : alloc;
    impl = sc_alloc(alloc, sizeof(*impl), _Alignof(hardware_tool));
    if (impl == nullptr) {
        return sc_status_no_memory();
    }
    *impl = (hardware_tool){.hardware = hardware, .write = write};
    status = sc_tool_context_copy(alloc, tool_context, &impl->context);
    if (sc_status_is_ok(status)) {
        status = write ? sc_tool_schema_three_strings(alloc,
                                                      sc_str_from_cstr("device"),
                                                      true,
                                                      sc_str_from_cstr("pin"),
                                                      true,
                                                      sc_str_from_cstr("value"),
                                                      true,
                                                      &impl->context.schema)
                       : sc_tool_schema_two_strings(alloc,
                                                    sc_str_from_cstr("device"),
                                                    true,
                                                    sc_str_from_cstr("pin"),
                                                    true,
                                                    &impl->context.schema);
    }
    if (sc_status_is_ok(status)) {
        status = sc_tool_new(alloc, write ? &gpio_write_tool_vtab : &gpio_read_tool_vtab, impl, out);
    }
    if (!sc_status_is_ok(status)) {
        hardware_tool_destroy(impl);
    }
    return status;
}
