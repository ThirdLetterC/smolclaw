#pragma once

#include "tools/tool_internal.h"
#include "sc/runtime.h"

typedef struct sc_tool_process_request {
    sc_str executable;
    const sc_str *args;
    size_t arg_count;
    sc_str cwd;
    const sc_security_policy *policy;
    const sc_str *env_passthrough;
    size_t env_passthrough_count;
    sc_str network_subject;
    bool may_use_network;
    int64_t timeout_ms;
    size_t max_output_bytes;
    const sc_cancel_token *cancel_token;
} sc_tool_process_request;

typedef struct sc_tool_process_result {
    sc_string output;
    int exit_code;
    bool exited;
} sc_tool_process_result;

sc_status sc_tool_process_run(sc_allocator *alloc,
                              const sc_tool_process_request *request,
                              sc_string *out);
sc_status sc_tool_process_run_ex(sc_allocator *alloc,
                                 const sc_tool_process_request *request,
                                 sc_tool_process_result *out);
void sc_tool_process_result_clear(sc_tool_process_result *result);
