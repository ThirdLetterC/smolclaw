// Copyright 2007 - 2021, Alan Antonuk and the rabbitmq-c contributors.
// SPDX-License-Identifier: mit

#include <limits.h>
#include <stdint.h>
#include <stdio.h>

#include "rabbitmq/amqp.h"
#include "rabbitmq/tcp_socket.h"

#include <assert.h>

#include <sys/time.h>

#include "utils.h"

int main(int argc, char const *const *argv) {
  char const *hostname;
  int port;
  amqp_socket_t *socket;
  amqp_connection_state_t conn;
  struct timeval tval;
  struct timeval *tv;

  if (argc < 3) {
    fprintf(stderr,
            "Usage: amqp_connect_timeout host port [timeout_sec "
            "[timeout_usec=0]]\n");
    return 1;
  }

  if (argc > 3) {
    tv = &tval;

    tv->tv_sec = parse_int_arg(argv[3], "timeout_sec", 0, INT_MAX);

    if (argc > 4) {
      tv->tv_usec = parse_int_arg(argv[4], "timeout_usec", 0, 999999);
    } else {
      tv->tv_usec = 0;
    }

  } else {
    tv = nullptr;
  }

  hostname = argv[1];
  port = parse_int_arg(argv[2], "port", 1, (int)UINT16_MAX);

  conn = amqp_new_connection();

  socket = amqp_tcp_socket_new(conn);

  if (!socket) {
    die("creating TCP socket");
  }

  die_on_error(amqp_socket_open_noblock(socket, hostname, port, tv),
               "opening TCP socket");

  die_on_amqp_error(amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN,
                               "guest", "guest"),
                    "Logging in");

  die_on_amqp_error(amqp_connection_close(conn, AMQP_REPLY_SUCCESS),
                    "Closing connection");
  die_on_error(amqp_destroy_connection(conn), "Ending connection");

  printf("Done\n");
  return 0;
}
