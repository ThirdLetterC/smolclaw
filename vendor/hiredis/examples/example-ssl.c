#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hiredis/hiredis.h"
#include "hiredis/hiredis_ssl.h"

[[nodiscard]] static redisReply *runCommand(redisContext *c, const char *format,
                                            ...) {
  va_list ap;
  va_start(ap, format);
  auto reply = (redisReply *)redisvCommand(c, format, ap);
  va_end(ap);

  if (reply == nullptr || c->err) {
    fprintf(stderr, "Command error: %s\n", c->err ? c->errstr : "unknown");
    freeReplyObject(reply);
    return nullptr;
  }

  return reply;
}

int main(int argc, char **argv) {
  redisSSLContextError ssl_error = REDIS_SSL_CTX_NONE;
  redisContext *c = nullptr;
  redisReply *reply = nullptr;
  int status = EXIT_FAILURE;

  if (argc < 5) {
    printf("Usage: %s <host> <port> <cert> <key> [ca]\n", argv[0]);
    return EXIT_FAILURE;
  }
  const char *hostname = (argc > 1) ? argv[1] : "127.0.0.1";
  auto port = atoi(argv[2]);
  const char *cert = argv[3];
  const char *key = argv[4];
  const char *ca = (argc > 5) ? argv[5] : nullptr;

  redisInitOpenSSL();
  auto ssl = redisCreateSSLContext(ca, nullptr, cert, key, nullptr, &ssl_error);
  if (!ssl || ssl_error != REDIS_SSL_CTX_NONE) {
    printf("SSL Context error: %s\n", redisSSLContextGetError(ssl_error));
    return EXIT_FAILURE;
  }

  constexpr struct timeval timeout = {.tv_sec = 1,
                                      .tv_usec = 500'000}; // 1.5 seconds
  redisOptions options = {0};
  REDIS_OPTIONS_SET_TCP(&options, hostname, port);
  options.connect_timeout = &timeout;
  c = redisConnectWithOptions(&options);

  if (c == nullptr || c->err) {
    if (c) {
      printf("Connection error: %s\n", c->errstr);
    } else {
      printf("Connection error: can't allocate redis context\n");
    }
    goto cleanup;
  }

  if (redisInitiateSSLWithContext(c, ssl) != REDIS_OK) {
    printf("Couldn't initialize SSL!\n");
    printf("Error: %s\n", c->errstr);
    goto cleanup;
  }

  /* PING server */
  reply = runCommand(c, "PING");
  if (reply == nullptr)
    goto cleanup;
  printf("PING: %s\n", reply->str);
  freeReplyObject(reply);
  reply = nullptr;

  /* Set a key */
  reply = runCommand(c, "SET %s %s", "foo", "hello world");
  if (reply == nullptr)
    goto cleanup;
  printf("SET: %s\n", reply->str);
  freeReplyObject(reply);
  reply = nullptr;

  /* Set a key using binary safe API */
  reply = runCommand(c, "SET %b %b", "bar", (size_t)3, "hello", (size_t)5);
  if (reply == nullptr)
    goto cleanup;
  printf("SET (binary API): %s\n", reply->str);
  freeReplyObject(reply);
  reply = nullptr;

  /* Try a GET and two INCR */
  reply = runCommand(c, "GET foo");
  if (reply == nullptr)
    goto cleanup;
  printf("GET foo: %s\n", reply->str);
  freeReplyObject(reply);
  reply = nullptr;

  reply = runCommand(c, "INCR counter");
  if (reply == nullptr)
    goto cleanup;
  printf("INCR counter: %lld\n", reply->integer);
  freeReplyObject(reply);
  reply = nullptr;
  /* again ... */
  reply = runCommand(c, "INCR counter");
  if (reply == nullptr)
    goto cleanup;
  printf("INCR counter: %lld\n", reply->integer);
  freeReplyObject(reply);
  reply = nullptr;

  /* Create a list of numbers, from 0 to 9 */
  reply = runCommand(c, "DEL mylist");
  if (reply == nullptr)
    goto cleanup;
  freeReplyObject(reply);
  reply = nullptr;
  constexpr size_t list_len = 10;
  constexpr size_t buf_size = 64;
  for (size_t j = 0; j < list_len; j++) {
    char buf[buf_size];

    snprintf(buf, sizeof(buf), "%zu", j);
    reply = runCommand(c, "LPUSH mylist element-%s", buf);
    if (reply == nullptr)
      goto cleanup;
    freeReplyObject(reply);
    reply = nullptr;
  }

  /* Let's check what we have inside the list */
  reply = runCommand(c, "LRANGE mylist 0 -1");
  if (reply == nullptr)
    goto cleanup;
  if (reply->type == REDIS_REPLY_ARRAY) {
    for (size_t j = 0; j < reply->elements; j++) {
      printf("%zu) %s\n", j, reply->element[j]->str);
    }
  }
  freeReplyObject(reply);
  reply = nullptr;

  /* Disconnects and frees the context */
  status = EXIT_SUCCESS;

cleanup:
  freeReplyObject(reply);
  redisFree(c);
  redisFreeSSLContext(ssl);

  return status;
}
