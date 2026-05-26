// Copyright 2007 - 2021, Alan Antonuk and the rabbitmq-c contributors.
// SPDX-License-Identifier: mit

#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

#include "rabbitmq/amqp.h"
#include "rabbitmq/amqp_time.h"
#include "rabbitmq/tcp_socket.h"

#ifdef NDEBUG
#undef NDEBUG
#endif

#include <assert.h>

static const int fixed_channel_id = 1;
static const char test_queue_name[] = "test_queue";

amqp_connection_state_t setup_connection_and_channel() {
  amqp_connection_state_t connection_state_ = amqp_new_connection();

  amqp_socket_t *socket = amqp_tcp_socket_new(connection_state_);
  assert(socket);

  int rc = amqp_socket_open(socket, "localhost", AMQP_PROTOCOL_PORT);
  assert(rc == AMQP_STATUS_OK);

  amqp_rpc_reply_t rpc_reply = amqp_login(
      connection_state_, "/", 1, AMQP_DEFAULT_FRAME_SIZE,
      AMQP_DEFAULT_HEARTBEAT, AMQP_SASL_METHOD_PLAIN, "guest", "guest");
  assert(rpc_reply.reply_type == AMQP_RESPONSE_NORMAL);

  amqp_channel_open_ok_t *res =
      amqp_channel_open(connection_state_, fixed_channel_id);
  assert(res != nullptr);

  return connection_state_;
}

void close_and_destroy_connection(amqp_connection_state_t connection_state_) {
  amqp_rpc_reply_t rpc_reply =
      amqp_connection_close(connection_state_, AMQP_REPLY_SUCCESS);
  assert(rpc_reply.reply_type == AMQP_RESPONSE_NORMAL);

  int rc = amqp_destroy_connection(connection_state_);
  assert(rc == AMQP_STATUS_OK);
}

void basic_publish(amqp_connection_state_t connectionState_,
                   const char *message_) {
  amqp_bytes_t message_bytes = amqp_cstring_bytes(message_);

  amqp_basic_properties_t properties;
  properties._flags = 0;

  properties._flags |= AMQP_BASIC_DELIVERY_MODE_FLAG;
  properties.delivery_mode = AMQP_DELIVERY_NONPERSISTENT;

  int retval = amqp_basic_publish(
      connectionState_, fixed_channel_id, amqp_cstring_bytes(""),
      amqp_cstring_bytes(test_queue_name),
      /* mandatory=*/1,
      /* immediate=*/0, /* RabbitMQ 3.x does not support the "immediate" flag
                          according to
                          https://www.rabbitmq.com/specification.html */
      &properties, message_bytes);

  assert(retval == 0);
}

void queue_declare(amqp_connection_state_t connection_state_,
                   const char *queue_name_) {
  amqp_queue_declare_ok_t *res = amqp_queue_declare(
      connection_state_, fixed_channel_id, amqp_cstring_bytes(queue_name_),
      /*passive*/ 0,
      /*durable*/ 0,
      /*exclusive*/ 0,
      /*auto_delete*/ 1, amqp_empty_table);
  assert(res != nullptr);
}

char *basic_get(amqp_connection_state_t connection_state_,
                const char *queue_name_, uint64_t *out_body_size_) {
  amqp_rpc_reply_t rpc_reply;
  amqp_time_t deadline;
  struct timeval timeout = {5, 0};
  int time_rc = amqp_time_from_now(&deadline, &timeout);
  assert(time_rc == AMQP_STATUS_OK);

  do {
    rpc_reply = amqp_basic_get(connection_state_, fixed_channel_id,
                               amqp_cstring_bytes(queue_name_), /*no_ack*/ 1);
  } while (rpc_reply.reply_type == AMQP_RESPONSE_NORMAL &&
           rpc_reply.reply.id == AMQP_BASIC_GET_EMPTY_METHOD &&
           amqp_time_has_past(deadline) == AMQP_STATUS_OK);

  assert(rpc_reply.reply_type == AMQP_RESPONSE_NORMAL);
  assert(rpc_reply.reply.id == AMQP_BASIC_GET_OK_METHOD);

  amqp_message_t message;
  rpc_reply =
      amqp_read_message(connection_state_, fixed_channel_id, &message, 0);
  assert(rpc_reply.reply_type == AMQP_RESPONSE_NORMAL);

  auto body_len = message.body.len;
  auto alloc_len = body_len > 0 ? body_len : (size_t)1;
  auto body = (char *)calloc(alloc_len, 1);
  if (body == nullptr) {
    return nullptr;
  }
  if (body_len > 0) {
    memcpy(body, message.body.bytes, body_len);
  }
  *out_body_size_ = body_len;
  amqp_destroy_message(&message);

  return body;
}

void publish_and_basic_get_message(const char *msg_to_publish) {
  amqp_connection_state_t connection_state = setup_connection_and_channel();

  queue_declare(connection_state, test_queue_name);
  basic_publish(connection_state, msg_to_publish);

  uint64_t body_size;
  char *msg = basic_get(connection_state, test_queue_name, &body_size);

  assert(msg != nullptr && "Test errored: memory allocation failed!");
  assert(body_size == strlen(msg_to_publish));
  assert(strncmp(msg_to_publish, msg, body_size) == 0);
  free(msg);

  close_and_destroy_connection(connection_state);
}

char *consume_message(amqp_connection_state_t connection_state_,
                      const char *queue_name_, uint64_t *out_body_size_) {
  amqp_basic_consume_ok_t *result =
      amqp_basic_consume(connection_state_, fixed_channel_id,
                         amqp_cstring_bytes(queue_name_), amqp_empty_bytes,
                         /*no_local*/ 0,
                         /*no_ack*/ 1,
                         /*exclusive*/ 0, amqp_empty_table);
  assert(result != nullptr);

  amqp_envelope_t envelope;
  struct timeval timeout = {5, 0};
  amqp_rpc_reply_t rpc_reply =
      amqp_consume_message(connection_state_, &envelope, &timeout, 0);
  assert(rpc_reply.reply_type == AMQP_RESPONSE_NORMAL);

  auto body_len = envelope.message.body.len;
  *out_body_size_ = body_len;
  auto alloc_len = body_len > 0 ? body_len : (size_t)1;
  auto body = (char *)calloc(alloc_len, 1);
  if (body == nullptr) {
    return nullptr;
  }
  if (body_len > 0) {
    memcpy(body, envelope.message.body.bytes, body_len);
  }

  amqp_destroy_envelope(&envelope);
  return body;
}

void publish_and_consume_message(const char *msg_to_publish) {
  amqp_connection_state_t connection_state = setup_connection_and_channel();

  queue_declare(connection_state, test_queue_name);
  basic_publish(connection_state, msg_to_publish);

  uint64_t body_size;
  char *msg = consume_message(connection_state, test_queue_name, &body_size);

  assert(msg != nullptr && "Test errored: memory allocation failed!");
  assert(body_size == strlen(msg_to_publish));
  assert(strncmp(msg_to_publish, msg, body_size) == 0);
  free(msg);

  close_and_destroy_connection(connection_state);
}

int main() {
  publish_and_basic_get_message("");
  publish_and_basic_get_message("TEST");

  publish_and_consume_message("");
  publish_and_consume_message("TEST");

  return 0;
}
