// Copyright 2007 - 2021, Alan Antonuk and the rabbitmq-c contributors.
// SPDX-License-Identifier: mit

#include <assert.h>
#include <sys/time.h>

#include "rabbitmq/amqp.h"
#include "rabbitmq/amqp_private.h"
#include "rabbitmq/amqp_socket.h"
#include "rabbitmq/amqp_time.h"

typedef struct {
  const struct amqp_socket_class_t *klass;
  size_t send_calls;
} test_socket_t;

static ssize_t test_socket_send(void *base, [[maybe_unused]] const void *buf,
                                size_t len, [[maybe_unused]] int flags) {
  test_socket_t *self = (test_socket_t *)base;
  self->send_calls++;
  return (ssize_t)len;
}

static ssize_t test_socket_recv([[maybe_unused]] void *base,
                                [[maybe_unused]] void *buf,
                                [[maybe_unused]] size_t len,
                                [[maybe_unused]] int flags) {
  return AMQP_STATUS_CONNECTION_CLOSED;
}

static int test_socket_open([[maybe_unused]] void *base,
                            [[maybe_unused]] const char *host,
                            [[maybe_unused]] int port,
                            [[maybe_unused]] const struct timeval *timeout) {
  return AMQP_STATUS_OK;
}

static int test_socket_close([[maybe_unused]] void *base,
                             [[maybe_unused]] amqp_socket_close_enum force) {
  return AMQP_STATUS_OK;
}

static int test_socket_get_sockfd([[maybe_unused]] void *base) { return -1; }

static void test_socket_delete([[maybe_unused]] void *base) {}

static const struct amqp_socket_class_t test_socket_class = {
    test_socket_send,  test_socket_recv,       test_socket_open,
    test_socket_close, test_socket_get_sockfd, test_socket_delete};

static void test_time_from_now_rejects_invalid_usec() {
  amqp_time_t deadline;
  struct timeval invalid_timeout = {0, 1000000};
  struct timeval valid_timeout = {0, 999999};

  assert(amqp_time_from_now(&deadline, &invalid_timeout) ==
         AMQP_STATUS_INVALID_PARAMETER);
  assert(amqp_time_from_now(&deadline, &valid_timeout) == AMQP_STATUS_OK);
}

static void test_connection_timeout_setters_validate_usec() {
  amqp_connection_state_t state = amqp_new_connection();
  struct timeval invalid_timeout = {1, 1000000};
  struct timeval valid_timeout = {1, 999999};

  assert(state != nullptr);

  assert(amqp_set_handshake_timeout(state, &invalid_timeout) ==
         AMQP_STATUS_INVALID_PARAMETER);
  assert(amqp_set_rpc_timeout(state, &invalid_timeout) ==
         AMQP_STATUS_INVALID_PARAMETER);

  assert(amqp_set_handshake_timeout(state, &valid_timeout) == AMQP_STATUS_OK);
  assert(amqp_set_rpc_timeout(state, &valid_timeout) == AMQP_STATUS_OK);

  assert(amqp_destroy_connection(state) == AMQP_STATUS_OK);
}

static void test_try_send_skips_write_after_deadline() {
  amqp_connection_state_t state = amqp_new_connection();
  test_socket_t socket = {.klass = &test_socket_class, .send_calls = 0};
  static const char payload[] = "abc";

  assert(state != nullptr);
  amqp_set_socket(state, (amqp_socket_t *)&socket);

  assert(amqp_try_send(state, payload, sizeof(payload), (amqp_time_t){0},
                       AMQP_SF_NONE) == 0);
  assert(socket.send_calls == 0);

  assert(amqp_destroy_connection(state) == AMQP_STATUS_OK);
}

static void test_try_send_writes_before_deadline() {
  amqp_connection_state_t state = amqp_new_connection();
  test_socket_t socket = {.klass = &test_socket_class, .send_calls = 0};
  static const char payload[] = "abc";

  assert(state != nullptr);
  amqp_set_socket(state, (amqp_socket_t *)&socket);

  assert(amqp_try_send(state, payload, sizeof(payload), amqp_time_infinite(),
                       AMQP_SF_NONE) == (ssize_t)sizeof(payload));
  assert(socket.send_calls == 1);

  assert(amqp_destroy_connection(state) == AMQP_STATUS_OK);
}

int main() {
  test_time_from_now_rejects_invalid_usec();
  test_connection_timeout_setters_validate_usec();
  test_try_send_skips_write_after_deadline();
  test_try_send_writes_before_deadline();
  return 0;
}
