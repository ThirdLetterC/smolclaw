#pragma once

#include <stddef.h>
#include <stdint.h>

/**
 * @brief WebSocket Protocol States and OpCodes
 */
typedef enum {
  WS_STATE_HANDSHAKE,
  WS_STATE_OPEN,
  WS_STATE_CLOSING,
  WS_STATE_CLOSED
} ws_state_t;

typedef enum {
  WS_OP_CONTINUATION = 0x0,
  WS_OP_TEXT = 0x1,
  WS_OP_BINARY = 0x2,
  WS_OP_CLOSE = 0x8,
  WS_OP_PING = 0x9,
  WS_OP_PONG = 0xA
} ws_opcode_t;

/**
 * @brief Transport Interface
 * This struct allows the protocol logic to trigger I/O without
 * knowing about libuv specifically.
 */
typedef struct ws_transport_s {
  void *user_data; // Typically points to a uv_handle_t or wrapper
  void (*send_raw)(struct ws_transport_s *self, const uint8_t *data,
                   size_t len);
  void (*close)(struct ws_transport_s *self);
} ws_transport_t;

/**
 * @brief WebSocket Connection Context
 */
typedef struct ws_conn_s ws_conn_t;

/**
 * @brief Application Callbacks
 */
typedef struct {
  void (*on_open)(ws_conn_t *conn);
  void (*on_message)(ws_conn_t *conn, const uint8_t *data, size_t len,
                     ws_opcode_t opcode);
  void (*on_close)(ws_conn_t *conn);
} ws_callbacks_t;

/* --- Core API --- */

/**
 * @brief Initialize a new WebSocket connection context.
 * @return Owning pointer; caller must call ws_conn_free when done.
 */
[[nodiscard]] ws_conn_t *ws_conn_new(ws_transport_t transport,
                                     ws_callbacks_t callbacks,
                                     void *external_context);

/**
 * @brief Free the WebSocket connection context.
 */
void ws_conn_free(ws_conn_t *conn);

/**
 * @brief Feed raw data from the transport (e.g., libuv) into the protocol
 * parser.
 */
void ws_conn_feed(ws_conn_t *conn, const uint8_t *data, size_t len);

/**
 * @brief Send a message over the WebSocket.
 */
void ws_conn_send(ws_conn_t *conn, const uint8_t *data, size_t len,
                  ws_opcode_t opcode);

/**
 * @brief Get user-defined context associated with the connection.
 * @return Non-owning pointer previously provided by the application.
 */
[[nodiscard]] void *ws_conn_get_context(ws_conn_t *conn);
