#pragma once

#include "sc/runtime.h"
#include "sc/string.h"
#include "sc/vector.h"

typedef struct sc_runtime_channel_provider_route_entry {
    sc_string channel_name;
    sc_string provider_name;
    sc_string model;
} sc_runtime_channel_provider_route_entry;

const sc_runtime_channel_provider_route_entry *sc_runtime_channel_provider_route_for(const sc_vec *routes,
                                                                                    sc_str channel_name);
sc_status sc_runtime_channel_model_switch_for(const sc_vec *routes,
                                              sc_str channel_name,
                                              sc_model_switch_request *out);
