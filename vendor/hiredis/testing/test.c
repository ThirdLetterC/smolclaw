#include <stdint.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hiredis/alloc.h"
#include "hiredis/hiredis.h"

#define CHECK(cond)                                                            \
  do {                                                                         \
    if (!(cond)) {                                                             \
      fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #cond, __FILE__,      \
              __LINE__);                                                       \
      ok = false;                                                              \
      goto cleanup;                                                            \
    }                                                                          \
  } while (false)

static bool test_format_command_simple() {
  bool ok = true;
  char *cmd = nullptr;

  auto len = redisFormatCommand(&cmd, "SET %s %s", "foo", "bar");
  static constexpr const char expected[] =
      "*3\r\n$3\r\nSET\r\n$3\r\nfoo\r\n$3\r\nbar\r\n";

  CHECK(len > 0);
  CHECK((size_t)len == sizeof(expected) - 1);
  CHECK(memcmp(cmd, expected, sizeof(expected)) == 0);

cleanup:
  redisFreeCommand(cmd);
  return ok;
}

static bool test_format_command_binary_payload() {
  bool ok = true;
  char *cmd = nullptr;

  static constexpr char payload[] = {'h', 'i', '\0', '!'};
  auto len = redisFormatCommand(&cmd, "SET %s %b", "blob", payload,
                                sizeof(payload));
  static constexpr const char expected[] =
      "*3\r\n$3\r\nSET\r\n$4\r\nblob\r\n$4\r\nhi\0!\r\n";

  CHECK(len > 0);
  CHECK((size_t)len == sizeof(expected) - 1);
  CHECK(memcmp(cmd, expected, sizeof(expected)) == 0);

cleanup:
  redisFreeCommand(cmd);
  return ok;
}

static bool test_format_command_argv_with_explicit_lengths() {
  bool ok = true;
  char *cmd = nullptr;

  static constexpr char key[] = {'k', '\0', 'y'};
  static constexpr char value[] = {'v', '\0', 'l'};
  static constexpr char op[] = "SET";
  static const char *argv[] = {op, key, value};
  static constexpr size_t argvlen[] = {sizeof(op) - 1, sizeof(key),
                                       sizeof(value)};

  auto len = redisFormatCommandArgv(&cmd, 3, argv, argvlen);
  static constexpr const char expected[] =
      "*3\r\n$3\r\nSET\r\n$3\r\nk\0y\r\n$3\r\nv\0l\r\n";

  CHECK(len > 0);
  CHECK((size_t)len == sizeof(expected) - 1);
  CHECK(memcmp(cmd, expected, sizeof(expected)) == 0);

cleanup:
  redisFreeCommand(cmd);
  return ok;
}

static bool test_format_command_argv_with_strlen_lengths() {
  bool ok = true;
  char *cmd = nullptr;

  static const char *argv[] = {"INCR", "counter"};
  auto len = redisFormatCommandArgv(&cmd, 2, argv, nullptr);
  static constexpr const char expected[] = "*2\r\n$4\r\nINCR\r\n$7\r\ncounter\r\n";

  CHECK(len > 0);
  CHECK((size_t)len == sizeof(expected) - 1);
  CHECK(memcmp(cmd, expected, sizeof(expected)) == 0);

cleanup:
  redisFreeCommand(cmd);
  return ok;
}

static bool test_format_command_invalid_inputs() {
  bool ok = true;
  char *cmd = nullptr;
  static const char *argv[] = {"PING"};
  static const char *bad_argv[] = {"SET", nullptr, "x"};

  CHECK(redisFormatCommand(nullptr, "PING") == -1);
  CHECK(redisFormatCommand(&cmd, "PING %q", "x") == -1);
  CHECK(cmd == nullptr);

  CHECK(redisFormatCommandArgv(nullptr, 1, argv, nullptr) == -1);
  CHECK(redisFormatCommandArgv(&cmd, -1, argv, nullptr) == -1);
  CHECK(redisFormatCommandArgv(&cmd, 1, nullptr, nullptr) == -1);
  CHECK(redisFormatCommandArgv(&cmd, 3, bad_argv, nullptr) == -1);
  CHECK(cmd == nullptr);

cleanup:
  redisFreeCommand(cmd);
  return ok;
}

static bool test_format_command_numeric_and_percent_specifiers() {
  bool ok = true;
  char *cmd = nullptr;

  auto len =
      redisFormatCommand(&cmd, "ECHO %hhd %hd %ld %lld %f %%", (int)1, (int)2,
                         (long)3, (long long)4, 1.5);
  static constexpr const char expected[] =
      "*7\r\n$4\r\nECHO\r\n$1\r\n1\r\n$1\r\n2\r\n$1\r\n3\r\n$1\r\n4\r\n$8\r\n1.500000\r\n$1\r\n%\r\n";

  CHECK(len > 0);
  CHECK((size_t)len == sizeof(expected) - 1);
  CHECK(memcmp(cmd, expected, sizeof(expected)) == 0);

cleanup:
  redisFreeCommand(cmd);
  return ok;
}

static bool test_format_sds_command_argv() {
  bool ok = true;
  sds cmd = nullptr;

  static const char *argv[] = {"SET", "alpha", "beta"};
  auto len = redisFormatSdsCommandArgv(&cmd, 3, argv, nullptr);
  static constexpr const char expected[] =
      "*3\r\n$3\r\nSET\r\n$5\r\nalpha\r\n$4\r\nbeta\r\n";

  CHECK(len > 0);
  CHECK((size_t)len == sizeof(expected) - 1);
  CHECK(sdslen(cmd) == sizeof(expected) - 1);
  CHECK(memcmp(cmd, expected, sizeof(expected)) == 0);

cleanup:
  redisFreeSdsCommand(cmd);
  return ok;
}

static bool test_format_sds_command_argv_invalid_inputs() {
  bool ok = true;
  sds cmd = nullptr;
  static const char *argv[] = {"PING"};
  static const char *bad_argv[] = {"SET", nullptr, "x"};

  CHECK(redisFormatSdsCommandArgv(nullptr, 1, argv, nullptr) == -1);
  CHECK(redisFormatSdsCommandArgv(&cmd, -1, argv, nullptr) == -1);
  CHECK(redisFormatSdsCommandArgv(&cmd, 1, nullptr, nullptr) == -1);
  CHECK(redisFormatSdsCommandArgv(&cmd, 3, bad_argv, nullptr) == -1);
  CHECK(cmd == nullptr);

cleanup:
  redisFreeSdsCommand(cmd);
  return ok;
}

static bool test_reader_parses_fragmented_status_reply() {
  bool ok = true;
  redisReader *reader = redisReaderCreate();
  void *reply = nullptr;

  CHECK(reader != nullptr);
  CHECK(redisReaderFeed(reader, "+PO", 3) == REDIS_OK);
  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply == nullptr);

  CHECK(redisReaderFeed(reader, "NG\r\n", 4) == REDIS_OK);
  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);

  auto typed = (redisReply *)reply;
  CHECK(typed->type == REDIS_REPLY_STATUS);
  CHECK(typed->len == 4);
  CHECK(memcmp(typed->str, "PONG", 4) == 0);

cleanup:
  freeReplyObject(reply);
  redisReaderFree(reader);
  return ok;
}

static bool test_reader_parses_bulk_nil_and_verbatim() {
  bool ok = true;
  redisReader *reader = redisReaderCreate();
  void *reply = nullptr;

  CHECK(reader != nullptr);
  CHECK(redisReaderFeed(reader, "$5\r\nhello\r\n$-1\r\n=9\r\ntxt:hello\r\n",
                        sizeof("$5\r\nhello\r\n$-1\r\n=9\r\ntxt:hello\r\n") -
                            1) == REDIS_OK);

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto first = (redisReply *)reply;
  CHECK(first->type == REDIS_REPLY_STRING);
  CHECK(first->len == 5);
  CHECK(memcmp(first->str, "hello", 5) == 0);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto second = (redisReply *)reply;
  CHECK(second->type == REDIS_REPLY_NIL);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto third = (redisReply *)reply;
  CHECK(third->type == REDIS_REPLY_VERB);
  CHECK(third->len == 5);
  CHECK(memcmp(third->vtype, "txt", 4) == 0);
  CHECK(memcmp(third->str, "hello", 5) == 0);

cleanup:
  freeReplyObject(reply);
  redisReaderFree(reader);
  return ok;
}

static bool test_reader_parses_array_of_integers() {
  bool ok = true;
  redisReader *reader = redisReaderCreate();
  void *reply = nullptr;

  CHECK(reader != nullptr);
  CHECK(redisReaderFeed(reader, "*2\r\n:41\r\n:42\r\n",
                        sizeof("*2\r\n:41\r\n:42\r\n") - 1) == REDIS_OK);
  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);

  auto typed = (redisReply *)reply;
  CHECK(typed->type == REDIS_REPLY_ARRAY);
  CHECK(typed->elements == 2);
  CHECK(typed->element[0] != nullptr);
  CHECK(typed->element[1] != nullptr);
  CHECK(typed->element[0]->type == REDIS_REPLY_INTEGER);
  CHECK(typed->element[1]->type == REDIS_REPLY_INTEGER);
  CHECK(typed->element[0]->integer == 41);
  CHECK(typed->element[1]->integer == 42);

cleanup:
  freeReplyObject(reply);
  redisReaderFree(reader);
  return ok;
}

static bool test_reader_parses_resp3_scalars() {
  bool ok = true;
  redisReader *reader = redisReaderCreate();
  void *reply = nullptr;

  CHECK(reader != nullptr);
  CHECK(redisReaderFeed(reader,
                        ",1.5\r\n,inf\r\n,-inf\r\n,nan\r\n_\r\n#t\r\n#f\r\n(12345678901234567890\r\n",
                        sizeof(",1.5\r\n,inf\r\n,-inf\r\n,nan\r\n_\r\n#t\r\n#f\r\n(12345678901234567890\r\n") -
                            1) == REDIS_OK);

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto finite = (redisReply *)reply;
  CHECK(finite->type == REDIS_REPLY_DOUBLE);
  CHECK(finite->len == 3);
  CHECK(finite->dval == 1.5);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto pos_inf = (redisReply *)reply;
  CHECK(pos_inf->type == REDIS_REPLY_DOUBLE);
  CHECK(isinf(pos_inf->dval) != 0);
  CHECK(pos_inf->dval > 0.0);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto neg_inf = (redisReply *)reply;
  CHECK(neg_inf->type == REDIS_REPLY_DOUBLE);
  CHECK(isinf(neg_inf->dval) != 0);
  CHECK(neg_inf->dval < 0.0);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto nan_value = (redisReply *)reply;
  CHECK(nan_value->type == REDIS_REPLY_DOUBLE);
  CHECK(isnan(nan_value->dval) != 0);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto nil_value = (redisReply *)reply;
  CHECK(nil_value->type == REDIS_REPLY_NIL);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto true_value = (redisReply *)reply;
  CHECK(true_value->type == REDIS_REPLY_BOOL);
  CHECK(true_value->integer == 1);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto false_value = (redisReply *)reply;
  CHECK(false_value->type == REDIS_REPLY_BOOL);
  CHECK(false_value->integer == 0);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto bignum = (redisReply *)reply;
  CHECK(bignum->type == REDIS_REPLY_BIGNUM);
  CHECK(bignum->len == 20);
  CHECK(memcmp(bignum->str, "12345678901234567890", 20) == 0);

cleanup:
  freeReplyObject(reply);
  redisReaderFree(reader);
  return ok;
}

static bool test_reader_parses_resp3_aggregates() {
  bool ok = true;
  redisReader *reader = redisReaderCreate();
  void *reply = nullptr;

  CHECK(reader != nullptr);
  CHECK(redisReaderFeed(
            reader,
            "%2\r\n+first\r\n:1\r\n+second\r\n:2\r\n~2\r\n+alpha\r\n+beta\r\n>2\r\n+pubsub\r\n+message\r\n|1\r\n+ttl\r\n:42\r\n",
            sizeof("%2\r\n+first\r\n:1\r\n+second\r\n:2\r\n~2\r\n+alpha\r\n+beta\r\n>2\r\n+pubsub\r\n+message\r\n|1\r\n+ttl\r\n:42\r\n") -
                1) == REDIS_OK);

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto map = (redisReply *)reply;
  CHECK(map->type == REDIS_REPLY_MAP);
  CHECK(map->elements == 4);
  CHECK(map->element[0]->type == REDIS_REPLY_STATUS);
  CHECK(map->element[1]->type == REDIS_REPLY_INTEGER);
  CHECK(map->element[2]->type == REDIS_REPLY_STATUS);
  CHECK(map->element[3]->type == REDIS_REPLY_INTEGER);
  CHECK(memcmp(map->element[0]->str, "first", 5) == 0);
  CHECK(map->element[1]->integer == 1);
  CHECK(memcmp(map->element[2]->str, "second", 6) == 0);
  CHECK(map->element[3]->integer == 2);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto set = (redisReply *)reply;
  CHECK(set->type == REDIS_REPLY_SET);
  CHECK(set->elements == 2);
  CHECK(set->element[0]->type == REDIS_REPLY_STATUS);
  CHECK(set->element[1]->type == REDIS_REPLY_STATUS);
  CHECK(memcmp(set->element[0]->str, "alpha", 5) == 0);
  CHECK(memcmp(set->element[1]->str, "beta", 4) == 0);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto push = (redisReply *)reply;
  CHECK(push->type == REDIS_REPLY_PUSH);
  CHECK(push->elements == 2);
  CHECK(push->element[0]->type == REDIS_REPLY_STATUS);
  CHECK(push->element[1]->type == REDIS_REPLY_STATUS);
  CHECK(memcmp(push->element[0]->str, "pubsub", 6) == 0);
  CHECK(memcmp(push->element[1]->str, "message", 7) == 0);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto attr = (redisReply *)reply;
  CHECK(attr->type == REDIS_REPLY_ATTR);
  CHECK(attr->elements == 2);
  CHECK(attr->element[0]->type == REDIS_REPLY_STATUS);
  CHECK(attr->element[1]->type == REDIS_REPLY_INTEGER);
  CHECK(memcmp(attr->element[0]->str, "ttl", 3) == 0);
  CHECK(attr->element[1]->integer == 42);

cleanup:
  freeReplyObject(reply);
  redisReaderFree(reader);
  return ok;
}

static bool test_reader_multiple_replies_and_null_target() {
  bool ok = true;
  redisReader *reader = redisReaderCreate();
  void *reply = nullptr;

  CHECK(reader != nullptr);
  CHECK(redisReaderFeed(reader, ":1\r\n:2\r\n+OK\r\n",
                        sizeof(":1\r\n:2\r\n+OK\r\n") - 1) == REDIS_OK);

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto first = (redisReply *)reply;
  CHECK(first->type == REDIS_REPLY_INTEGER);
  CHECK(first->integer == 1);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply != nullptr);
  auto second = (redisReply *)reply;
  CHECK(second->type == REDIS_REPLY_INTEGER);
  CHECK(second->integer == 2);
  freeReplyObject(reply);
  reply = nullptr;

  CHECK(redisReaderGetReply(reader, nullptr) == REDIS_OK);
  CHECK(redisReaderGetReply(reader, &reply) == REDIS_OK);
  CHECK(reply == nullptr);

cleanup:
  freeReplyObject(reply);
  redisReaderFree(reader);
  return ok;
}

static bool test_reader_additional_protocol_errors() {
  bool ok = true;
  redisReader *reader = nullptr;
  void *reply = nullptr;

  reader = redisReaderCreate();
  CHECK(reader != nullptr);
  CHECK(redisReaderFeed(reader, "#x\r\n", sizeof("#x\r\n") - 1) == REDIS_OK);
  CHECK(redisReaderGetReply(reader, &reply) == REDIS_ERR);
  CHECK(reader->err == REDIS_ERR_PROTOCOL);
  CHECK(strstr(reader->errstr, "Bad bool value") != nullptr);
  redisReaderFree(reader);
  reader = nullptr;

  reader = redisReaderCreate();
  CHECK(reader != nullptr);
  CHECK(redisReaderFeed(reader, "=3\r\nabc\r\n", sizeof("=3\r\nabc\r\n") - 1) ==
            REDIS_OK);
  CHECK(redisReaderGetReply(reader, &reply) == REDIS_ERR);
  CHECK(reader->err == REDIS_ERR_PROTOCOL);
  CHECK(strstr(reader->errstr, "Verbatim string") != nullptr);
  redisReaderFree(reader);
  reader = nullptr;

  reader = redisReaderCreate();
  CHECK(reader != nullptr);
  CHECK(redisReaderFeed(reader, "$2\r\nabX\n", sizeof("$2\r\nabX\n") - 1) ==
            REDIS_OK);
  CHECK(redisReaderGetReply(reader, &reply) == REDIS_ERR);
  CHECK(reader->err == REDIS_ERR_PROTOCOL);
  CHECK(strstr(reader->errstr, "Bad bulk string format") != nullptr);

cleanup:
  freeReplyObject(reply);
  redisReaderFree(reader);
  return ok;
}

static bool test_reader_protocol_error() {
  bool ok = true;
  redisReader *reader = redisReaderCreate();
  void *reply = nullptr;

  CHECK(reader != nullptr);
  CHECK(redisReaderFeed(reader, "!\r\n", sizeof("!\r\n") - 1) == REDIS_OK);
  CHECK(redisReaderGetReply(reader, &reply) == REDIS_ERR);
  CHECK(reply == nullptr);
  CHECK(reader->err == REDIS_ERR_PROTOCOL);
  CHECK(strstr(reader->errstr, "Protocol error") != nullptr);

cleanup:
  freeReplyObject(reply);
  redisReaderFree(reader);
  return ok;
}

static size_t g_custom_calloc_calls = 0;

static void *test_calloc(size_t nmemb, size_t size) {
  g_custom_calloc_calls++;
  return calloc(nmemb, size);
}

static bool test_allocator_override_and_overflow_guard() {
  bool ok = true;

  hiredisAllocFuncs initial = hiredisAllocFns;
  hiredisAllocFuncs custom = {
      .mallocFn = nullptr,
      .callocFn = test_calloc,
      .reallocFn = nullptr,
      .strdupFn = nullptr,
      .freeFn = nullptr,
  };

  g_custom_calloc_calls = 0;
  auto previous = hiredisSetAllocators(&custom);
  CHECK(previous.callocFn == initial.callocFn);
  CHECK(hiredisAllocFns.callocFn == test_calloc);

  auto ptr = hi_calloc(4, sizeof(int));
  CHECK(ptr != nullptr);
  CHECK(g_custom_calloc_calls == 1);
  hi_free(ptr);

  constexpr size_t too_many = (SIZE_MAX / 2) + 1;
  auto before = g_custom_calloc_calls;
  CHECK(hi_calloc(too_many, 2) == nullptr);
  CHECK(g_custom_calloc_calls == before);

  hiredisSetAllocators(&previous);
  CHECK(hiredisAllocFns.callocFn == initial.callocFn);

cleanup:
  hiredisSetAllocators(&initial);
  return ok;
}

typedef bool (*test_fn)();

typedef struct test_case {
  const char *name;
  test_fn fn;
} test_case;

int main() {
  static const test_case tests[] = {
      {"format_command_simple", test_format_command_simple},
      {"format_command_binary_payload", test_format_command_binary_payload},
      {"format_command_argv_with_explicit_lengths",
       test_format_command_argv_with_explicit_lengths},
      {"format_command_argv_with_strlen_lengths",
       test_format_command_argv_with_strlen_lengths},
      {"format_command_invalid_inputs", test_format_command_invalid_inputs},
      {"format_command_numeric_and_percent_specifiers",
       test_format_command_numeric_and_percent_specifiers},
      {"format_sds_command_argv", test_format_sds_command_argv},
      {"format_sds_command_argv_invalid_inputs",
       test_format_sds_command_argv_invalid_inputs},
      {"reader_parses_fragmented_status_reply",
       test_reader_parses_fragmented_status_reply},
      {"reader_parses_bulk_nil_and_verbatim",
       test_reader_parses_bulk_nil_and_verbatim},
      {"reader_parses_array_of_integers", test_reader_parses_array_of_integers},
      {"reader_parses_resp3_scalars", test_reader_parses_resp3_scalars},
      {"reader_parses_resp3_aggregates", test_reader_parses_resp3_aggregates},
      {"reader_multiple_replies_and_null_target",
       test_reader_multiple_replies_and_null_target},
      {"reader_additional_protocol_errors",
       test_reader_additional_protocol_errors},
      {"reader_protocol_error", test_reader_protocol_error},
      {"allocator_override_and_overflow_guard",
       test_allocator_override_and_overflow_guard},
  };

  size_t failures = 0;
  for (size_t i = 0; i < (sizeof(tests) / sizeof(tests[0])); i++) {
    auto passed = tests[i].fn();
    printf("[%s] %s\n", passed ? "PASS" : "FAIL", tests[i].name);
    if (!passed) {
      failures++;
    }
  }

  if (failures != 0) {
    fprintf(stderr, "%zu test(s) failed.\n", failures);
    return EXIT_FAILURE;
  }

  printf("All %zu tests passed.\n", sizeof(tests) / sizeof(tests[0]));
  return EXIT_SUCCESS;
}
