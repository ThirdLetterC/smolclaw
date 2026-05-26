#pragma once

#include <stdio.h>

#include "sc/allocator.h"
#include "sc/result.h"
#include "sc/string.h"

typedef struct prompt_context {
    FILE *in;
    FILE *out;
} prompt_context;

sc_status sc_app_onboard_copilot_device_flow(prompt_context *ctx, sc_allocator *alloc, sc_string *out);
