#pragma once

#include <stdint.h>

#include "sc/allocator.h"
#include "sc/result.h"
#include "sc/string.h"

typedef struct sc_mcp_client_options {
    size_t struct_size;
    sc_str transport;
    sc_str command;
    sc_str args;
    sc_str url;
    sc_str headers;
    size_t max_output_bytes;
    int64_t timeout_ms;
} sc_mcp_client_options;

sc_status sc_mcp_client_call(sc_allocator *alloc,
                             const sc_mcp_client_options *options,
                             sc_str tool_name,
                             sc_str arguments_json,
                             sc_string *out);
