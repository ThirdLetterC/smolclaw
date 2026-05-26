#pragma once

#include <stddef.h>
#include <stdint.h>

#include "jsonrpc/parson.h"

typedef struct jsonrpc_transport_s {
  void *user_data;
  /**
   * @brief Send raw bytes over the transport.
   * @return true when the write is accepted by the transport, false on failure.
   *         The buffer is only guaranteed to remain valid for the duration of
   *         the call.
   */
  bool (*send_raw)(struct jsonrpc_transport_s *self, const uint8_t *data,
                   size_t len);
  void (*close)(struct jsonrpc_transport_s *self);
} jsonrpc_transport_t;

typedef struct jsonrpc_conn_s jsonrpc_conn_t;

/**
 * @brief Response container populated by on_request. The server
 * zero-initializes this struct before invoking the handler.
 */
typedef struct {
  JSON_Value *result;        // owning, may be nullptr on error
  int32_t error_code;        // 0 on success, JSON-RPC error code otherwise
  const char *error_message; // optional, nullptr uses default message
} jsonrpc_response_t;

typedef struct {
  void (*on_open)(jsonrpc_conn_t *conn);
  void (*on_close)(jsonrpc_conn_t *conn);
  /**
   * @brief Handle a JSON-RPC request. Populate response with result or error.
   * @return true if handled, false to trigger "method not found".
   */
  bool (*on_request)(jsonrpc_conn_t *conn, const char *method,
                     const JSON_Value *params, jsonrpc_response_t *response);
  void (*on_notification)(jsonrpc_conn_t *conn, const char *method,
                          const JSON_Value *params);
} jsonrpc_callbacks_t;

[[nodiscard]] jsonrpc_conn_t *jsonrpc_conn_new(jsonrpc_transport_t transport,
                                               jsonrpc_callbacks_t callbacks,
                                               void *external_context);

void jsonrpc_conn_free(jsonrpc_conn_t *conn);

void jsonrpc_conn_feed(jsonrpc_conn_t *conn, const uint8_t *data, size_t len);

/**
 * @brief Send a JSON-RPC result. Takes ownership of result on success/failure.
 */
[[nodiscard]] bool jsonrpc_conn_send_result(jsonrpc_conn_t *conn,
                                            const JSON_Value *id,
                                            JSON_Value *result);

/**
 * @brief Send a JSON-RPC error response.
 */
[[nodiscard]] bool jsonrpc_conn_send_error(jsonrpc_conn_t *conn,
                                           const JSON_Value *id, int32_t code,
                                           const char *message);

[[nodiscard]] void *jsonrpc_conn_get_context(jsonrpc_conn_t *conn);
