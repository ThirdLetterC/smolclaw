#pragma once

#include <stdint.h>

#include "websocket-server/websocket.h"

void server_set_callbacks(ws_callbacks_t callbacks);
[[nodiscard]] ws_callbacks_t server_get_callbacks();
void start_ws_server(int32_t port, ws_callbacks_t callbacks);
void server_request_shutdown();
