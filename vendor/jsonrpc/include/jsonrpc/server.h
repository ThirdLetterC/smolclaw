#pragma once

#include <stdint.h>

#include "jsonrpc/jsonrpc.h"

void server_set_callbacks(jsonrpc_callbacks_t callbacks);
[[nodiscard]] jsonrpc_callbacks_t server_get_callbacks();
void start_jsonrpc_server(int32_t port, jsonrpc_callbacks_t callbacks);
void server_request_shutdown();
