#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "adapters/poll.h"
#include "hiredis/async.h"

/* Put in the global scope, so that loop can be explicitly stopped */
static bool exit_loop = false;

void getCallback(redisAsyncContext *c, void *r, void *privdata) {
  redisReply *reply = r;
  if (reply == nullptr)
    return;
  printf("argv[%s]: %s\n", (char *)privdata, reply->str);

  /* Disconnect after receiving the reply to GET */
  redisAsyncDisconnect(c);
}

void connectCallback(const redisAsyncContext *c, int status) {
  if (status != REDIS_OK) {
    printf("Error: %s\n", c->errstr);
    exit_loop = true;
    return;
  }

  printf("Connected...\n");
}

void disconnectCallback(const redisAsyncContext *c, int status) {
  exit_loop = true;
  if (status != REDIS_OK) {
    printf("Error: %s\n", c->errstr);
    return;
  }

  printf("Disconnected...\n");
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);

  constexpr int default_port = 6'379;
  auto c = redisAsyncConnect("127.0.0.1", default_port);
  if (c == nullptr || c->err) {
    if (c != nullptr) {
      redisAsyncFree(c);
    }
    printf("Error: %s\n", c ? c->errstr : "can't allocate redis context");
    return 1;
  }

  redisPollAttach(c);
  redisAsyncSetConnectCallback(c, connectCallback);
  redisAsyncSetDisconnectCallback(c, disconnectCallback);
  const char *value = (argc > 1) ? argv[1] : "poll-example-value";
  auto value_len = strlen(value);
  if (redisAsyncCommand(c, nullptr, nullptr, "SET key %b", value, value_len) !=
          REDIS_OK ||
      redisAsyncCommand(c, getCallback, (char *)"end-1", "GET key") !=
          REDIS_OK) {
    printf("Error: %s\n",
           c->errstr ? c->errstr : "failed to queue async command");
    redisAsyncDisconnect(c);
  }
  constexpr double tick_seconds = 0.1;
  while (!exit_loop) {
    redisPollTick(c, tick_seconds);
  }
  return 0;
}
