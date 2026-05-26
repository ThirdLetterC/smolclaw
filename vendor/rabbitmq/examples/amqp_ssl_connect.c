// Copyright 2007 - 2021, Alan Antonuk and the rabbitmq-c contributors.
// SPDX-License-Identifier: mit

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "rabbitmq/amqp.h"
#include "rabbitmq/ssl_socket.h"

#include <assert.h>

#include <sys/time.h>

#include "utils.h"

[[noreturn]] static void usage() {
  fprintf(stderr,
          "Usage: amqp_ssl_connect host port timeout_sec "
          "[cacert.pem|system] [engine engine_ID] [noverifypeer] "
          "[noverifyhostname] [key.pem cert.pem]\n");
  exit(1);
}

static int parse_decimal_int(const char *value, const char *name) {
  char *end = nullptr;
  long parsed = strtol(value, &end, 10);

  if (value == end || *end != '\0' || parsed < INT32_MIN ||
      parsed > INT32_MAX) {
    die("invalid %s: %s", name, value);
  }

  return (int)parsed;
}

int main(int argc, char const *const *argv) {
  char const *hostname;
  int port;
  int timeout;
  amqp_socket_t *socket;
  amqp_connection_state_t conn;
  struct timeval tval;
  struct timeval *tv;

  if (argc < 4) {
    usage();
  }

  hostname = argv[1];
  port = parse_decimal_int(argv[2], "port");
  if (port <= 0 || port > UINT16_MAX) {
    die("invalid port: %s", argv[2]);
  }

  timeout = parse_decimal_int(argv[3], "timeout");
  if (timeout > 0) {
    tv = &tval;

    tv->tv_sec = timeout;
    tv->tv_usec = 0;
  } else {
    tv = nullptr;
  }

  conn = amqp_new_connection();

  socket = amqp_ssl_socket_new(conn);
  if (!socket) {
    die("creating SSL/TLS socket");
  }

  int nextarg = 4;
  if (argc > nextarg && strcmp(argv[nextarg], "engine") != 0 &&
      strcmp(argv[nextarg], "noverifypeer") != 0 &&
      strcmp(argv[nextarg], "noverifyhostname") != 0) {
    if (strcmp(argv[nextarg], "system") == 0) {
      die_on_error(amqp_ssl_socket_enable_default_verify_paths(socket),
                   "loading system CA certificates");
    } else {
      die_on_error(amqp_ssl_socket_set_cacert(socket, argv[nextarg]),
                   "setting CA certificate");
    }
    nextarg++;
  } else {
    die_on_error(amqp_ssl_socket_enable_default_verify_paths(socket),
                 "loading system CA certificates");
  }

  while (argc > nextarg) {
    if (!strcmp("engine", argv[nextarg])) {
      if (argc <= nextarg + 1) {
        usage();
      }
      die_on_error(amqp_set_ssl_engine(argv[nextarg + 1]),
                   "setting SSL engine");
      nextarg += 2;
      continue;
    }

    if (!strcmp("noverifypeer", argv[nextarg])) {
      amqp_ssl_socket_set_verify_peer(socket, 0);
      nextarg++;
      continue;
    }

    if (!strcmp("noverifyhostname", argv[nextarg])) {
      amqp_ssl_socket_set_verify_hostname(socket, 0);
      nextarg++;
      continue;
    }

    break;
  }

  if (argc - nextarg == 2) {
    die_on_error(
        amqp_ssl_socket_set_key(socket, argv[nextarg + 1], argv[nextarg]),
        "setting client key");
  } else if (argc != nextarg) {
    usage();
  }

  die_on_error(amqp_socket_open_noblock(socket, hostname, port, tv),
               "opening SSL/TLS connection");

  die_on_amqp_error(amqp_login(conn, "/", 0, 131072, 0, AMQP_SASL_METHOD_PLAIN,
                               "guest", "guest"),
                    "Logging in");

  die_on_amqp_error(amqp_connection_close(conn, AMQP_REPLY_SUCCESS),
                    "Closing connection");
  die_on_error(amqp_destroy_connection(conn), "Ending connection");

  printf("Done\n");
  return 0;
}
