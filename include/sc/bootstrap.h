#pragma once

#include <stddef.h>

#include "sc/allocator.h"
#include "sc/result.h"
#include "sc/runtime.h"
#include "sc/string.h"

typedef struct sc_boot_session sc_boot_session;

typedef struct sc_boot_options {
    size_t struct_size;
    sc_str config_path;
    sc_str workspace_path;
    bool once;
    size_t max_polls;
    bool gateway_enabled;
    bool gateway_listener_enabled;
    bool hard_shutdown;
    sc_str gateway_bind;
} sc_boot_options;

sc_status sc_runtime_boot(sc_allocator *alloc, const sc_boot_options *options);
sc_status sc_boot_session_open(sc_allocator *alloc, const sc_boot_options *options, sc_boot_session **out);
sc_status sc_boot_session_process_ex(sc_boot_session *session,
                                     sc_str input,
                                     sc_turn_tool_approval_fn approval,
                                     void *approval_user_data,
                                     sc_turn_event_fn event_callback,
                                     void *event_callback_user_data,
                                     sc_allocator *alloc,
                                     sc_runtime_response *out);
sc_status sc_boot_session_process(sc_boot_session *session,
                                  sc_str input,
                                  sc_allocator *alloc,
                                  sc_runtime_response *out);
void sc_boot_session_destroy(sc_boot_session *session);
