#include <signal.h>
#include <stdio.h>
#include <string.h>

#include "adapters/libuv.h"
#include "hiredis/async.h"
#include "hiredis/hiredis.h"

void debugCallback(redisAsyncContext *c, void *r,
                   [[maybe_unused]] void *privdata) {
  redisReply *reply = r;
  if (reply == nullptr) {
    /* The DEBUG SLEEP command will almost always fail, because we have set a 1
     * second timeout */
    printf("`DEBUG SLEEP` error: %s\n",
           c->errstr ? c->errstr : "unknown error");
    return;
  }
  /* Disconnect after receiving the reply of DEBUG SLEEP (which will not)*/
  redisAsyncDisconnect(c);
}

void getCallback(redisAsyncContext *c, void *r, void *privdata) {
  redisReply *reply = r;
  if (reply == nullptr) {
    printf("`GET key` error: %s\n", c->errstr ? c->errstr : "unknown error");
    return;
  }
  printf("`GET key` result: argv[%s]: %s\n", (char *)privdata, reply->str);

  /* start another request that demonstrate timeout */
  redisAsyncCommand(c, debugCallback, nullptr, "DEBUG SLEEP %f", 1.5);
}

void connectCallback(const redisAsyncContext *c, int status) {
  if (status != REDIS_OK) {
    printf("connect error: %s\n", c->errstr);
    return;
  }
  printf("Connected...\n");
}

void disconnectCallback(const redisAsyncContext *c, int status) {
  if (status != REDIS_OK) {
    printf("disconnect because of error: %s\n", c->errstr);
    return;
  }
  printf("Disconnected...\n");
}

int main(int argc, char **argv) {
  signal(SIGPIPE, SIG_IGN);

  auto loop = uv_default_loop();

  constexpr int default_port = 6'379;
  auto c = redisAsyncConnect("127.0.0.1", default_port);
  if (c == nullptr || c->err) {
    if (c != nullptr) {
      redisAsyncFree(c);
    }
    printf("Error: %s\n", c ? c->errstr : "can't allocate redis context");
    return 1;
  }

  redisLibuvAttach(c, loop);
  redisAsyncSetConnectCallback(c, connectCallback);
  redisAsyncSetDisconnectCallback(c, disconnectCallback);
  constexpr struct timeval timeout = {.tv_sec = 1, .tv_usec = 0};
  redisAsyncSetTimeout(c, timeout);

  /*
  In this demo, we first `set key`, then `get key` to demonstrate the basic
  usage of libuv adapter. Then in `getCallback`, we start a `debug sleep`
  command to create 1.5 second long request. Because we have set a 1 second
  timeout to the connection, the command will always fail with a timeout error,
  which is shown in the `debugCallback`.
  */

  const char *value = (argc > 1) ? argv[1] : "libuv-example-value";
  auto value_len = strlen(value);
  if (redisAsyncCommand(c, nullptr, nullptr, "SET key %b", value, value_len) !=
          REDIS_OK ||
      redisAsyncCommand(c, getCallback, (char *)"end-1", "GET key") !=
          REDIS_OK) {
    printf("Error: %s\n",
           c->errstr ? c->errstr : "failed to queue async command");
    redisAsyncDisconnect(c);
  }

  uv_run(loop, UV_RUN_DEFAULT);
  return 0;
}
