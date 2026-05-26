#pragma once

#include "sc/allocator.h"
#include "sc/buffer.h"
#include "sc/contract.h"
#include "sc/observer.h"
#include "sc/result.h"
#include "sc/string.h"

SC_BEGIN_DECLS

/*
 * Ownership/threading: commands borrow input buffers. Result buffers are
 * caller-owned. The handle owns impl and calls destroy exactly once. The
 * wrapper does not synchronize commands; peripherals document their own
 * thread-safety.
 */
typedef struct sc_peripheral sc_peripheral;
typedef struct sc_cancel_token sc_cancel_token;
typedef struct sc_estop_state sc_estop_state;

typedef struct sc_peripheral_command {
    size_t struct_size;
    sc_str operation;
    sc_buf payload;
    int64_t timeout_ms;
    const sc_cancel_token *cancel_token;
    const sc_estop_state *estop;
    sc_observer *observer;
    bool destructive;
    sc_str expected_vendor_id;
    sc_str expected_product_id;
    sc_str expected_serial_number;
} sc_peripheral_command;

typedef struct sc_peripheral_result {
    size_t struct_size;
    sc_bytes payload;
} sc_peripheral_result;

typedef struct sc_peripheral_health {
    size_t struct_size;
    bool healthy;
    sc_string message;
} sc_peripheral_health;

typedef struct sc_peripheral_context {
    size_t struct_size;
    sc_string text;
} sc_peripheral_context;

typedef struct sc_peripheral_vtab {
    size_t struct_size;
    uint32_t abi_major;
    const char *name;
    const char *display_name;
    const char *feature_flag;
    uint64_t capabilities;
    sc_stability_tier stability;
    sc_status (*health)(void *impl, sc_allocator *alloc, sc_peripheral_health *out);
    sc_status (*command)(void *impl,
                         const sc_peripheral_command *command,
                         sc_allocator *alloc,
                         sc_peripheral_result *out);
    sc_status (*describe_context)(void *impl, sc_allocator *alloc, sc_peripheral_context *out);
    void (*destroy)(void *impl);
    const char *device_class;
    const char *vendor_id;
    const char *product_id;
    const char *config_schema_ref;
    const char *manual_test_requirements;
    uint32_t safe_discovery_mode;
    bool identity_required;
    sc_status (*safe_shutdown)(void *impl);
} sc_peripheral_vtab;

static inline bool sc_peripheral_handle_is_null(const sc_peripheral *peripheral)
{
    return peripheral == nullptr;
}

bool sc_peripheral_vtab_valid(const sc_peripheral_vtab *vtab);
sc_status sc_peripheral_new(sc_allocator *alloc,
                            const sc_peripheral_vtab *vtab,
                            void *impl,
                            sc_peripheral **out);
sc_status sc_peripheral_command_send(sc_peripheral *peripheral,
                                     const sc_peripheral_command *command,
                                     sc_allocator *alloc,
                                     sc_peripheral_result *out);
sc_status sc_peripheral_health_check(sc_peripheral *peripheral, sc_allocator *alloc, sc_peripheral_health *out);
sc_status sc_peripheral_describe_context(sc_peripheral *peripheral, sc_allocator *alloc, sc_peripheral_context *out);
sc_status sc_peripheral_safe_shutdown(sc_peripheral *peripheral);
const sc_peripheral_vtab *sc_peripheral_vtab_of(const sc_peripheral *peripheral);
void sc_peripheral_destroy(sc_peripheral *peripheral);
void sc_peripheral_result_clear(sc_peripheral_result *result);
void sc_peripheral_health_clear(sc_peripheral_health *health);
void sc_peripheral_context_clear(sc_peripheral_context *context);

SC_END_DECLS
