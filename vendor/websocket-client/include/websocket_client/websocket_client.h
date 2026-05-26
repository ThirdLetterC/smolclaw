#pragma once

#include <stddef.h>
#include <stdint.h>

typedef struct ws_client ws_client_t;

/**
 * @brief Allocates a websocket client instance.
 * @return Owning pointer or nullptr on allocation failure.
 */
[[nodiscard]] ws_client_t *ws_client_create();

/**
 * @brief Releases all resources owned by the client.
 */
void ws_client_destroy(ws_client_t *client);

/**
 * @brief Connects and performs an RFC6455 handshake against a ws:// endpoint.
 */
[[nodiscard]] bool ws_client_connect(ws_client_t *client, const char *host,
                                     uint16_t port, const char *path);

/**
 * @brief Connects and performs an RFC6455 handshake against a wss:// endpoint.
 */
[[nodiscard]] bool ws_client_connect_secure(ws_client_t *client,
                                            const char *host, uint16_t port,
                                            const char *path);

/**
 * @brief Sends one text frame.
 */
[[nodiscard]] bool ws_client_send_text(ws_client_t *client, const char *text,
                                       size_t length);

/**
 * @brief Sends one binary frame.
 */
[[nodiscard]] bool ws_client_send_binary(ws_client_t *client,
                                         const uint8_t *data, size_t length);

/**
 * @brief Receives one text frame into a caller-owned buffer.
 */
[[nodiscard]] bool ws_client_receive_text(ws_client_t *client, char *buffer,
                                          size_t capacity, size_t *out_length);

/**
 * @brief Receives one binary frame into a caller-owned buffer.
 */
[[nodiscard]] bool ws_client_receive_binary(ws_client_t *client,
                                            uint8_t *buffer, size_t capacity,
                                            size_t *out_length);

/**
 * @brief Sends a close frame and closes the socket.
 */
void ws_client_close(ws_client_t *client);

/**
 * @brief Returns the most recent error string.
 */
[[nodiscard]] const char *ws_client_last_error(const ws_client_t *client);
