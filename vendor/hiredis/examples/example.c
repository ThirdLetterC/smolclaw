#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hiredis/hiredis.h"

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

static bool example_argv_command(redisContext *c, size_t n) {
  constexpr size_t argv_fixed = 2;
  constexpr size_t tmp_size = 42;
  const size_t argv_count = argv_fixed + n;
  char tmp[tmp_size];
  char **cmd_argv = calloc(argv_count, sizeof(*cmd_argv));
  size_t *argvlen = calloc(argv_count, sizeof(*argvlen));
  redisReply *reply = nullptr;
  bool ok = false;

  if (cmd_argv == nullptr || argvlen == nullptr) {
    fprintf(stderr, "Error: out of memory\n");
    goto cleanup;
  }

  /* First the command. */
  cmd_argv[0] = (char *)"RPUSH";
  argvlen[0] = sizeof("RPUSH") - 1;

  /* Now our key. */
  cmd_argv[1] = (char *)"argvlist";
  argvlen[1] = sizeof("argvlist") - 1;

  /* Add the entries we wish to push to the list. */
  for (size_t i = argv_fixed; i < argv_count; i++) {
    auto written =
        snprintf(tmp, sizeof(tmp), "argv-element-%zu", i - argv_fixed);
    if (written < 0 || (size_t)written >= sizeof(tmp)) {
      fprintf(stderr, "Error: failed to build argv entry\n");
      goto cleanup;
    }

    argvlen[i] = (size_t)written;
    cmd_argv[i] = strdup(tmp);
    if (cmd_argv[i] == nullptr) {
      fprintf(stderr, "Error: out of memory\n");
      goto cleanup;
    }
  }

  reply = redisCommandArgv(c, argv_count, (const char **)cmd_argv,
                           (const size_t *)argvlen);
  if (reply == nullptr || c->err) {
    fprintf(stderr, "Error: couldn't execute redisCommandArgv: %s\n",
            c->err ? c->errstr : "unknown");
    goto cleanup;
  }

  if (reply->type == REDIS_REPLY_INTEGER) {
    printf("%s reply: %lld\n", cmd_argv[0], reply->integer);
  }

  ok = true;

cleanup:
  freeReplyObject(reply);

  if (cmd_argv != nullptr) {
    for (size_t i = argv_fixed; i < argv_count; i++) {
      free(cmd_argv[i]);
    }
  }

  free(cmd_argv);
  free(argvlen);
  return ok;
}

int main(int argc, char **argv) {
  bool isunix = false;
  redisContext *c = nullptr;
  redisReply *reply = nullptr;
  int status = EXIT_FAILURE;
  const char *hostname = (argc > 1) ? argv[1] : "127.0.0.1";

  if (argc > 2) {
    if (*argv[2] == 'u' || *argv[2] == 'U') {
      isunix = true;
      /* In this case, host is the path to the unix socket. */
      printf("Will connect to unix socket @%s\n", hostname);
    }
  }

  constexpr int default_port = 6'379;
  auto port = (argc > 2) ? atoi(argv[2]) : default_port;

  constexpr struct timeval timeout = {.tv_sec = 1,
                                      .tv_usec = 500'000}; // 1.5 seconds
  if (isunix) {
    c = redisConnectUnixWithTimeout(hostname, timeout);
  } else {
    c = redisConnectWithTimeout(hostname, port, timeout);
  }
  if (c == nullptr || c->err) {
    if (c) {
      printf("Connection error: %s\n", c->errstr);
    } else {
      printf("Connection error: can't allocate redis context\n");
    }
    goto cleanup;
  }

  /* PING server. */
  reply = runCommand(c, "PING");
  if (reply == nullptr)
    goto cleanup;
  printf("PING: %s\n", reply->str);
  freeReplyObject(reply);
  reply = nullptr;

  /* Set a key. */
  reply = runCommand(c, "SET %s %s", "foo", "hello world");
  if (reply == nullptr)
    goto cleanup;
  printf("SET: %s\n", reply->str);
  freeReplyObject(reply);
  reply = nullptr;

  /* Set a key using binary safe API. */
  reply = runCommand(c, "SET %b %b", "bar", (size_t)3, "hello", (size_t)5);
  if (reply == nullptr)
    goto cleanup;
  printf("SET (binary API): %s\n", reply->str);
  freeReplyObject(reply);
  reply = nullptr;

  /* Try a GET and two INCR. */
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

  /* Again... */
  reply = runCommand(c, "INCR counter");
  if (reply == nullptr)
    goto cleanup;
  printf("INCR counter: %lld\n", reply->integer);
  freeReplyObject(reply);
  reply = nullptr;

  /* Create a list of numbers, from 0 to 9. */
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

  /* Let's check what we have inside the list. */
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

  /* See function for an example of redisCommandArgv. */
  constexpr size_t argv_entries = 10;
  if (!example_argv_command(c, argv_entries))
    goto cleanup;

  status = EXIT_SUCCESS;

cleanup:
  freeReplyObject(reply);
  redisFree(c);
  return status;
}
