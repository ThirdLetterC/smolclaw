#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <threads.h>
#include <time.h>

#include "hiredis/hiredis.h"

typedef struct {
  const char *host;
  int port;
  const char *stream;
  size_t count;
} stream_config_t;

/* Returns an owning redisContext*. Caller must redisFree(). */
[[nodiscard]] static redisContext *connect_redis(const char *host, int port) {
  constexpr struct timeval timeout = {.tv_sec = 1, .tv_usec = 500'000};
  auto context = redisConnectWithTimeout(host, port, timeout);
  if (context == nullptr) {
    fprintf(stderr, "Connection error: can't allocate redis context\n");
    return nullptr;
  }
  if (context->err) {
    fprintf(stderr, "Connection error: %s\n", context->errstr);
    redisFree(context);
    return nullptr;
  }
  return context;
}

static void print_field_pairs(redisReply *fields) {
  if (fields == nullptr || fields->type != REDIS_REPLY_ARRAY) {
    printf("  (no fields)\n");
    return;
  }

  for (size_t i = 0; i + 1 < fields->elements; i += 2) {
    auto key = fields->element[i];
    auto value = fields->element[i + 1];
    const char *key_str = (key && key->str) ? key->str : "(null)";
    const char *value_str = (value && value->str) ? value->str : "(null)";
    printf("  %s=%s\n", key_str, value_str);
  }
}

static int publisher_thread(void *arg) {
  auto cfg = (stream_config_t *)arg;
  auto context = connect_redis(cfg->host, cfg->port);
  if (context == nullptr) {
    return 1;
  }

  constexpr struct timespec pause = {.tv_sec = 0, .tv_nsec = 200'000'000};
  constexpr size_t payload_size = 64;

  for (size_t i = 0; i < cfg->count; i++) {
    char payload[payload_size];
    snprintf(payload, sizeof(payload), "message-%zu", i);

    redisReply *reply =
        redisCommand(context, "XADD %s * producer %s payload %s", cfg->stream,
                     "publisher", payload);
    if (reply == nullptr || context->err) {
      const char *err = context->err ? context->errstr : "unknown error";
      fprintf(stderr, "XADD error: %s\n", err);
      freeReplyObject(reply);
      redisFree(context);
      return 1;
    }

    if (reply->type == REDIS_REPLY_STRING) {
      printf("published id=%s\n", reply->str);
    }

    freeReplyObject(reply);
    thrd_sleep(&pause, nullptr);
  }

  redisFree(context);
  return 0;
}

static int subscriber_thread(void *arg) {
  auto cfg = (stream_config_t *)arg;
  auto context = connect_redis(cfg->host, cfg->port);
  if (context == nullptr) {
    return 1;
  }

  constexpr size_t id_buffer_size = 64;
  char last_id[id_buffer_size];
  snprintf(last_id, sizeof(last_id), "0-0");

  size_t received = 0;
  while (received < cfg->count) {
    redisReply *reply =
        redisCommand(context, "XREAD BLOCK 5000 COUNT 1 STREAMS %s %s",
                     cfg->stream, last_id);
    if (reply == nullptr || context->err) {
      const char *err = context->err ? context->errstr : "unknown error";
      fprintf(stderr, "XREAD error: %s\n", err);
      freeReplyObject(reply);
      redisFree(context);
      return 1;
    }

    if (reply->type == REDIS_REPLY_NIL) {
      freeReplyObject(reply);
      continue;
    }

    if (reply->type != REDIS_REPLY_ARRAY || reply->elements == 0) {
      fprintf(stderr, "XREAD unexpected reply type=%d\n", reply->type);
      freeReplyObject(reply);
      redisFree(context);
      return 1;
    }

    auto stream_reply = reply->element[0];
    if (stream_reply == nullptr || stream_reply->type != REDIS_REPLY_ARRAY ||
        stream_reply->elements < 2) {
      fprintf(stderr, "XREAD malformed stream reply\n");
      freeReplyObject(reply);
      redisFree(context);
      return 1;
    }

    auto entries = stream_reply->element[1];
    if (entries == nullptr || entries->type != REDIS_REPLY_ARRAY ||
        entries->elements == 0) {
      freeReplyObject(reply);
      continue;
    }

    auto entry = entries->element[0];
    if (entry == nullptr || entry->type != REDIS_REPLY_ARRAY ||
        entry->elements < 2) {
      fprintf(stderr, "XREAD malformed entry\n");
      freeReplyObject(reply);
      redisFree(context);
      return 1;
    }

    auto id = entry->element[0];
    auto fields = entry->element[1];
    const char *id_str = (id && id->str) ? id->str : "(null)";
    printf("received id=%s\n", id_str);
    print_field_pairs(fields);

    if (id && id->str) {
      snprintf(last_id, sizeof(last_id), "%s", id->str);
    }

    received++;
    freeReplyObject(reply);
  }

  redisFree(context);
  return 0;
}

int main(int argc, char **argv) {
  const char *host = (argc > 1) ? argv[1] : "127.0.0.1";
  constexpr int default_port = 6'379;
  auto port = (argc > 2) ? atoi(argv[2]) : default_port;
  const char *stream = (argc > 3) ? argv[3] : "example:stream";
  constexpr size_t default_count = 5;
  auto count = (argc > 4) ? (size_t)atoi(argv[4]) : default_count;

  stream_config_t config = {
      .host = host,
      .port = port,
      .stream = stream,
      .count = count,
  };

  auto setup = connect_redis(host, port);
  if (setup == nullptr) {
    return EXIT_FAILURE;
  }
  redisReply *cleanup_reply = redisCommand(setup, "DEL %s", stream);
  freeReplyObject(cleanup_reply);
  redisFree(setup);

  thrd_t sub_thread;
  thrd_t pub_thread;

  if (thrd_create(&sub_thread, subscriber_thread, &config) != thrd_success) {
    fprintf(stderr, "Failed to create subscriber thread\n");
    return EXIT_FAILURE;
  }

  constexpr struct timespec start_pause = {.tv_sec = 0, .tv_nsec = 150'000'000};
  thrd_sleep(&start_pause, nullptr);

  if (thrd_create(&pub_thread, publisher_thread, &config) != thrd_success) {
    fprintf(stderr, "Failed to create publisher thread\n");
    return EXIT_FAILURE;
  }

  int pub_result = 0;
  int sub_result = 0;
  thrd_join(pub_thread, &pub_result);
  thrd_join(sub_thread, &sub_result);

  if (pub_result != 0 || sub_result != 0) {
    fprintf(stderr, "One or more threads failed\n");
    return EXIT_FAILURE;
  }

  return 0;
}
