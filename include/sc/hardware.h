#pragma once

#include "sc/allocator.h"
#include "sc/json.h"
#include "sc/observer.h"
#include "sc/peripheral.h"
#include "sc/registry.h"
#include "sc/security.h"
#include "sc/string.h"
#include "sc/tool.h"
#include "sc/vector.h"

SC_BEGIN_DECLS

typedef struct sc_serial_transport sc_serial_transport;

enum {
    SC_HARDWARE_CAP_GPIO_READ = 1u << 0u,
    SC_HARDWARE_CAP_GPIO_WRITE = 1u << 1u,
    SC_HARDWARE_CAP_CONTEXT = 1u << 2u,
    SC_HARDWARE_CAP_SERIAL = 1u << 3u,
    SC_HARDWARE_CAP_SAFE_SHUTDOWN = 1u << 4u
};

typedef enum sc_hardware_device_class {
    SC_HARDWARE_DEVICE_CLASS_UNKNOWN = 0,
    SC_HARDWARE_DEVICE_CLASS_BOARD,
    SC_HARDWARE_DEVICE_CLASS_SENSOR,
    SC_HARDWARE_DEVICE_CLASS_ACTUATOR
} sc_hardware_device_class;

typedef enum sc_hardware_discovery_mode {
    SC_HARDWARE_DISCOVERY_PASSIVE = 0,
    SC_HARDWARE_DISCOVERY_CONFIGURED,
    SC_HARDWARE_DISCOVERY_UNSAFE_CLAIM
} sc_hardware_discovery_mode;

typedef enum sc_hardware_protocol_command {
    SC_HARDWARE_PROTOCOL_COMMAND_PING = 1,
    SC_HARDWARE_PROTOCOL_COMMAND_CAPABILITIES = 2,
    SC_HARDWARE_PROTOCOL_COMMAND_GPIO_READ = 3,
    SC_HARDWARE_PROTOCOL_COMMAND_GPIO_WRITE = 4
} sc_hardware_protocol_command;

typedef struct sc_hardware_pin {
    sc_string alias;
    int raw_pin;
    uint64_t capabilities;
} sc_hardware_pin;

typedef struct sc_hardware_manifest {
    sc_allocator *alloc;
    sc_string alias;
    sc_string board;
    bool allow_raw_pins;
    uint64_t capabilities;
    sc_vec pins;
    sc_hardware_device_class device_class;
    sc_string vendor_id;
    sc_string product_id;
    sc_string serial_number;
    sc_string protocol;
    sc_hardware_discovery_mode discovery_mode;
    bool safe_shutdown_supported;
    sc_string config_schema_ref;
    sc_string manual_test_requirements;
} sc_hardware_manifest;

typedef struct sc_hardware_device {
    sc_hardware_manifest manifest;
    sc_peripheral *peripheral;
} sc_hardware_device;

typedef struct sc_hardware_context {
    sc_allocator *alloc;
    bool enabled;
    sc_vec devices;
    sc_observer *observer;
    const sc_estop_state *estop;
    size_t observer_failures;
} sc_hardware_context;

typedef struct sc_serial_transport_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    sc_status (*exchange)(void *impl, sc_str request_json, sc_allocator *alloc, sc_string *response_json);
    void (*destroy)(void *impl);
} sc_serial_transport_vtab;

typedef struct sc_hardware_fake_serial_options {
    size_t struct_size;
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
} sc_hardware_fake_serial_options;

typedef struct sc_hardware_discovery_options {
    size_t struct_size;
    const sc_hardware_manifest *manifest;
    sc_hardware_fake_serial_options serial_options;
    bool allow_unsafe_claim;
    sc_str expected_vendor_id;
    sc_str expected_product_id;
    sc_str expected_serial_number;
    sc_str expected_protocol;
} sc_hardware_discovery_options;

typedef struct sc_hardware_frame {
    size_t struct_size;
    uint8_t command_id;
    sc_buf payload;
} sc_hardware_frame;

void sc_hardware_manifest_init(sc_hardware_manifest *manifest, sc_allocator *alloc);
sc_status sc_hardware_manifest_parse(sc_allocator *alloc, sc_str json, sc_hardware_manifest *out);
sc_status sc_hardware_manifest_add_pin(sc_hardware_manifest *manifest,
                                       sc_str alias,
                                       int raw_pin,
                                       uint64_t capabilities);
sc_status sc_hardware_manifest_resolve_pin(const sc_hardware_manifest *manifest,
                                           sc_str alias_or_pin,
                                           uint64_t required_capability,
                                           int *out_raw_pin);
void sc_hardware_manifest_clear(sc_hardware_manifest *manifest);

void sc_hardware_context_init(sc_hardware_context *context, sc_allocator *alloc);
void sc_hardware_context_set_observer(sc_hardware_context *context, sc_observer *observer);
void sc_hardware_context_set_estop(sc_hardware_context *context, const sc_estop_state *estop);
sc_status sc_hardware_context_add_device(sc_hardware_context *context,
                                         const sc_hardware_manifest *manifest,
                                         sc_peripheral *peripheral);
sc_status sc_hardware_context_discover_fake(sc_hardware_context *context,
                                            const sc_hardware_discovery_options *options,
                                            sc_peripheral **out_peripheral);
sc_hardware_device *sc_hardware_context_find_device(sc_hardware_context *context, sc_str alias);
const sc_hardware_device *sc_hardware_context_find_device_const(const sc_hardware_context *context, sc_str alias);
sc_status sc_hardware_context_prompt(const sc_hardware_context *context,
                                     sc_allocator *alloc,
                                     size_t max_bytes,
                                     sc_string *out);
void sc_hardware_context_clear(sc_hardware_context *context);

void sc_hardware_registry_init(sc_peripheral_registry *registry, sc_allocator *alloc);
sc_status sc_hardware_registry_register_fake(sc_peripheral_registry *registry);

bool sc_hardware_tools_enabled(const sc_hardware_context *context);
sc_status sc_hardware_validate_mutation(const sc_security_policy *policy,
                                        const sc_estop_state *estop,
                                        sc_str tool_name,
                                        bool *approval_required);

bool sc_serial_transport_vtab_valid(const sc_serial_transport_vtab *vtab);
sc_status sc_serial_transport_new(sc_allocator *alloc,
                                  const sc_serial_transport_vtab *vtab,
                                  void *impl,
                                  sc_serial_transport **out);
sc_status sc_serial_transport_exchange(sc_serial_transport *transport,
                                       sc_str request_json,
                                       sc_allocator *alloc,
                                       sc_string *response_json);
void sc_serial_transport_destroy(sc_serial_transport *transport);
sc_status sc_hardware_fake_serial_new(sc_allocator *alloc,
                                      const sc_hardware_fake_serial_options *options,
                                      sc_serial_transport **out);

sc_status sc_hardware_protocol_ping(sc_serial_transport *transport, sc_allocator *alloc, sc_string *out);
sc_status sc_hardware_protocol_capabilities(sc_serial_transport *transport, uint64_t *out_capabilities);
sc_status sc_hardware_protocol_gpio_read(sc_serial_transport *transport, int pin, bool *out_value);
sc_status sc_hardware_protocol_gpio_write(sc_serial_transport *transport, int pin, bool value);
sc_status sc_hardware_protocol_parse_frame(sc_buf frame, size_t max_payload_bytes, sc_hardware_frame *out);
uint8_t sc_hardware_protocol_frame_checksum(sc_buf frame_without_checksum);

sc_status sc_hardware_fake_board_new(sc_allocator *alloc,
                                     const sc_hardware_manifest *manifest,
                                     sc_serial_transport *transport,
                                     sc_peripheral **out);

sc_status sc_tool_hardware_gpio_read_new(sc_allocator *alloc,
                                         const sc_tool_context *tool_context,
                                         sc_hardware_context *hardware,
                                         sc_tool **out);
sc_status sc_tool_hardware_gpio_write_new(sc_allocator *alloc,
                                          const sc_tool_context *tool_context,
                                          sc_hardware_context *hardware,
                                          sc_tool **out);
sc_status sc_hardware_tools_register(sc_tool_registry *registry,
                                     const sc_tool_context *tool_context,
                                     sc_hardware_context *hardware);

SC_END_DECLS
