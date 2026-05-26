#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <time.h>

#if !defined(WIN32)
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#else
#include <windows.h>
#include <ws2tcpip.h>

/* Some shortcuts to call winapi in a posix-like way */
#define close(sock) closesocket(sock)
#endif

#include "../examples/templates/posix_sockets.h"
#include "mqtt/mqtt.h"

#define assert_true(condition)                                                 \
  do {                                                                         \
    if (!(condition)) {                                                        \
      fprintf(stderr, "Assertion failed: %s (%s:%d)\n", #condition, __FILE__,  \
              __LINE__);                                                       \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

#define assert_int_equal(expected, actual)                                     \
  do {                                                                         \
    auto _expected = (expected);                                               \
    auto _actual = (actual);                                                   \
    if (_expected != _actual) {                                                \
      fprintf(stderr,                                                          \
              "Assertion failed: %s == %s (0x%llx != 0x%llx) (%s:%d)\n",       \
              #expected, #actual, (unsigned long long)_expected,               \
              (unsigned long long)_actual, __FILE__, __LINE__);                \
      exit(EXIT_FAILURE);                                                      \
    }                                                                          \
  } while (0)

struct test_case {
  const char *name;
  void (*fn)(void **);
};

static void publish_callback(void **state,
                             struct mqtt_response_publish *publish);

static void run_test(const struct test_case test) {
  void *state = nullptr;
  printf(" - %s... ", test.name);
  fflush(stdout);
  test.fn(&state);
  puts("ok");
}

static void run_group(const char *heading, const struct test_case *tests,
                      size_t count) {
  printf("[%s]\n", heading);
  for (size_t i = 0; i < count; ++i) {
    run_test(tests[i]);
  }
  puts("");
}

static void fill_payload(uint8_t *buf, size_t bufsz, const char *msg) {
  if (buf == nullptr || bufsz == 0u) {
    return;
  }
  memset(buf, 0, bufsz);
  if (msg == nullptr) {
    return;
  }
  size_t len = strlen(msg);
  if (len > bufsz) {
    len = bufsz;
  }
  memcpy(buf, msg, len);
}

void make_socket_blocking(int socket) {
#if !defined(WIN32)
  fcntl(socket, F_SETFL, fcntl(socket, F_GETFL) & ~O_NONBLOCK);
#else
  int iMode = 0;
  ioctlsocket(socket, FIONBIO, &iMode);
#endif
}

const char *addr = "test.mosquitto.org";
const char *port = "1883";

static void mqttc_sleep_us(long microseconds) {
#if defined(WIN32)
  const DWORD millis = (DWORD)((microseconds + 999L) / 1000L);
  Sleep(millis);
#else
  struct timespec delay = {.tv_sec = microseconds / 1'000'000L,
                           .tv_nsec = (microseconds % 1'000'000L) * 1'000L};
  nanosleep(&delay, nullptr);
#endif
}

static void TEST__framing__fixed_header([[maybe_unused]] void **state) {
  static uint32_t remaining_lengths[] = {
      0, 127, 128, 16383, 16384, 2097151, 2097152, 268435455, 268435456};
  static ssize_t actual_lengths[] = {
      2, 2, 3, 3, 4, 4, 5, 5, MQTT_ERROR_INVALID_REMAINING_LENGTH};
  uint8_t correct_buf[1024];
  uint8_t buf[1024];
  struct mqtt_response response;
  struct mqtt_fixed_header *fixed_header = &response.fixed_header;
  ssize_t rv;
  size_t k;

  /*
   * remaining length tests on pack and unpack
   */
  for (k = 0; k < sizeof(remaining_lengths) / sizeof(remaining_lengths[0]);
       ++k) {
    fixed_header->control_type = MQTT_CONTROL_CONNECT;
    fixed_header->control_flags = 0;
    fixed_header->remaining_length = remaining_lengths[k];

    /* the length is a necessary lie */
    rv = mqtt_pack_fixed_header(buf, sizeof(buf) + remaining_lengths[k],
                                fixed_header);
    assert_true(rv == actual_lengths[k]);

    if (k == 8)
      buf[4] = 0x86;

    /* another unavoidable lie */
    rv = mqtt_unpack_fixed_header(&response, buf,
                                  sizeof(buf) + remaining_lengths[k]);
    assert_true(rv == actual_lengths[k]);

    if (k != 8)
      assert_true(remaining_lengths[k] ==
                  response.fixed_header.remaining_length);
  }

  /* sanity check with valid fixed_header */
  correct_buf[0] = (MQTT_CONTROL_CONNECT << 4) | 0;
  correct_buf[1] = 193u;
  correct_buf[2] = 2u;

  /* check that unpack is correct */
  rv = mqtt_unpack_fixed_header(&response, correct_buf, sizeof(correct_buf));
  assert_true(rv == 3);
  assert_true(fixed_header->control_type == MQTT_CONTROL_CONNECT);
  assert_true(fixed_header->control_flags == 0);
  assert_true(fixed_header->remaining_length == 321);

  /* check that unpack is correct */
  rv = mqtt_pack_fixed_header(buf, sizeof(buf), fixed_header);
  assert_true(rv == 3);
  assert_true(memcmp(correct_buf, buf, 3) == 0);

  /* check that invalid flags are caught */
  correct_buf[0] = (MQTT_CONTROL_CONNECT << 4) | 1;
  rv = mqtt_unpack_fixed_header(&response, correct_buf, sizeof(correct_buf));
  assert_true(rv == MQTT_ERROR_CONTROL_INVALID_FLAGS);

  /* check that valid flags are ok when there is a required bit */
  correct_buf[0] = (MQTT_CONTROL_PUBREL << 4) | 2;
  rv = mqtt_unpack_fixed_header(&response, correct_buf, sizeof(correct_buf));
  assert_true(rv == 3);

  /* check that invalid flags are ok when there is a required bit */
  correct_buf[0] = (MQTT_CONTROL_PUBREL << 4) | 3;
  rv = mqtt_unpack_fixed_header(&response, correct_buf, sizeof(correct_buf));
  assert_true(rv == MQTT_ERROR_CONTROL_INVALID_FLAGS);

  /* check that valid flags are ok when there are optional flags */
  correct_buf[0] = (MQTT_CONTROL_PUBLISH << 4) | 0xF;
  rv = mqtt_unpack_fixed_header(&response, correct_buf, sizeof(correct_buf));
  assert_true(rv == 3);

  correct_buf[0] = (MQTT_CONTROL_PUBLISH << 4) | 3;
  rv = mqtt_unpack_fixed_header(&response, correct_buf, sizeof(correct_buf));
  assert_true(rv == 3);

  /* check that remaining length is packed/unpacked correctly */
  correct_buf[0] = (MQTT_CONTROL_CONNECT << 4) | 0;
  correct_buf[1] = 64;

  /* check that unpack is correct */
  rv = mqtt_unpack_fixed_header(&response, correct_buf, sizeof(correct_buf));
  assert_true(rv == 2);
  assert_true(fixed_header->control_type == MQTT_CONTROL_CONNECT);
  assert_true(fixed_header->control_flags == 0);
  assert_true(fixed_header->remaining_length == 64);

  /* check that unpack is correct */
  rv = mqtt_pack_fixed_header(buf, sizeof(buf), fixed_header);
  assert_true(rv == 2);
  assert_true(memcmp(correct_buf, buf, 2) == 0);

  /* check that remaining length is packed/unpacked correctly */
  correct_buf[0] = (MQTT_CONTROL_CONNECT << 4) | 0;
  correct_buf[1] = 127;

  /* check that unpack is correct */
  rv = mqtt_unpack_fixed_header(&response, correct_buf, sizeof(correct_buf));
  assert_true(rv == 2);
  assert_true(fixed_header->control_type == MQTT_CONTROL_CONNECT);
  assert_true(fixed_header->control_flags == 0);
  assert_true(fixed_header->remaining_length == 127);

  /* check that unpack is correct */
  rv = mqtt_pack_fixed_header(buf, sizeof(buf), fixed_header);
  assert_true(rv == 2);
  assert_true(memcmp(correct_buf, buf, 2) == 0);

  /* check that remaining length is packed/unpacked correctly */
  correct_buf[0] = (MQTT_CONTROL_CONNECT << 4) | 0;
  correct_buf[1] = 128;
  correct_buf[2] = 1;

  /* check that unpack is correct */
  rv = mqtt_unpack_fixed_header(&response, correct_buf, sizeof(correct_buf));
  assert_true(rv == 3);
  assert_true(fixed_header->control_type == MQTT_CONTROL_CONNECT);
  assert_true(fixed_header->control_flags == 0);
  assert_true(fixed_header->remaining_length == 128);

  /* check that unpack is correct */
  rv = mqtt_pack_fixed_header(buf, sizeof(buf), fixed_header);
  assert_true(rv == 3);
  assert_true(memcmp(correct_buf, buf, 3) == 0);

  /* check bad inputs */
  assert_true(mqtt_pack_fixed_header(nullptr, 5, fixed_header) ==
              MQTT_ERROR_nullptrPTR);
  assert_true(mqtt_pack_fixed_header(buf, 5, nullptr) == MQTT_ERROR_nullptrPTR);
  assert_true(mqtt_pack_fixed_header(buf, 2, fixed_header) == 0);

  assert_true(mqtt_unpack_fixed_header(nullptr, buf, 5) ==
              MQTT_ERROR_nullptrPTR);
  assert_true(mqtt_unpack_fixed_header(&response, nullptr, 5) ==
              MQTT_ERROR_nullptrPTR);
  assert_true(mqtt_unpack_fixed_header(&response, buf, 2) == 0);
}

static void TEST__framing__connect([[maybe_unused]] void **state) {
  uint8_t buf[256];
  ssize_t rv;
  const uint8_t correct_bytes[] = {(MQTT_CONTROL_CONNECT << 4) | 0,
                                   16,
                                   0,
                                   4,
                                   'M',
                                   'Q',
                                   'T',
                                   'T',
                                   MQTT_PROTOCOL_LEVEL,
                                   0,
                                   0,
                                   120u,
                                   0,
                                   4,
                                   'l',
                                   'i',
                                   'a',
                                   'm'};
  const uint8_t correct_bytes2[] = {
      (MQTT_CONTROL_CONNECT << 4) | 0,
      51,
      0,
      4,
      'M',
      'Q',
      'T',
      'T',
      MQTT_PROTOCOL_LEVEL,
      MQTT_CONNECT_WILL_FLAG | MQTT_CONNECT_USER_NAME | MQTT_CONNECT_PASSWORD,
      0,
      120u,
      0,
      4,
      'l',
      'i',
      'a',
      'm',
      0,
      9,
      'w',
      'i',
      'l',
      'l',
      't',
      'o',
      'p',
      'i',
      'c',
      0,
      2,
      'h',
      'i',
      0,
      8,
      'u',
      's',
      'e',
      'r',
      'n',
      'a',
      'm',
      'e',
      0,
      8,
      'p',
      'a',
      's',
      's',
      'w',
      'o',
      'r',
      'd'};
  const uint8_t correct_bytes_empty_client_id[] = {(MQTT_CONTROL_CONNECT << 4) |
                                                       0,
                                                   12,
                                                   0,
                                                   4,
                                                   'M',
                                                   'Q',
                                                   'T',
                                                   'T',
                                                   MQTT_PROTOCOL_LEVEL,
                                                   MQTT_CONNECT_CLEAN_SESSION,
                                                   0,
                                                   120u,
                                                   0,
                                                   0};

  struct mqtt_response response;
  struct mqtt_fixed_header *fixed_header = &response.fixed_header;

  rv = mqtt_pack_connection_request(buf, sizeof(buf), "liam", nullptr, nullptr,
                                    0, nullptr, nullptr, 0, 120u);
  assert_true(rv == sizeof(correct_bytes));

  /* check that fixed header is correct */
  rv = mqtt_unpack_fixed_header(&response, buf, rv);
  assert_true(fixed_header->control_type == MQTT_CONTROL_CONNECT);
  assert_true(fixed_header->remaining_length == 16);

  /* check that memory is correct */
  assert_true(memcmp(correct_bytes, buf, sizeof(correct_bytes)) == 0);

  /* check that will flags are okay and user name and password */
  rv = mqtt_pack_connection_request(buf, sizeof(buf), "liam", "willtopic", "hi",
                                    2, "username", "password", 0, 120u);
  assert_true(rv == sizeof(correct_bytes2));

  /* check that memory is correct */
  assert_true(memcmp(correct_bytes2, buf, sizeof(correct_bytes2)) == 0);

  /* check that the empty client_id is packed correctly */
  rv = mqtt_pack_connection_request(buf, sizeof(buf), nullptr, nullptr, nullptr,
                                    0, nullptr, nullptr,
                                    MQTT_CONNECT_CLEAN_SESSION, 120u);
  assert_true(rv == sizeof(correct_bytes_empty_client_id));

  /* check that memory is correct */
  assert_true(memcmp(correct_bytes_empty_client_id, buf,
                     sizeof(correct_bytes_empty_client_id)) == 0);

  /* Check that empty client_id is rejected without MQTT_CONNECT_CLEAN_SESSION
   */
  rv = mqtt_pack_connection_request(buf, sizeof(buf), nullptr, nullptr, nullptr,
                                    0, nullptr, nullptr, 0, 120u);
  assert_int_equal(rv, MQTT_ERROR_CLEAN_SESSION_IS_REQUIRED);
}

static void TEST__framing__publish([[maybe_unused]] void **state) {
  uint8_t buf[256];
  ssize_t rv;
  const uint8_t correct_bytes[] = {(MQTT_CONTROL_PUBLISH << 4) |
                                       MQTT_PUBLISH_RETAIN,
                                   18,
                                   0,
                                   6,
                                   't',
                                   'o',
                                   'p',
                                   'i',
                                   'c',
                                   '1',
                                   '0',
                                   '1',
                                   '2',
                                   '3',
                                   '4',
                                   '5',
                                   '6',
                                   '7',
                                   '8',
                                   '9'};
  struct mqtt_response mqtt_response;
  struct mqtt_response_publish *response;
  response = &(mqtt_response.decoded.publish);

  rv = mqtt_pack_publish_request(buf, 256, "topic1", 23, "0123456789", 10,
                                 MQTT_PUBLISH_RETAIN);
  assert_true(rv == 20);
  assert_true(memcmp(buf, correct_bytes, 20) == 0);

  rv = mqtt_unpack_fixed_header(&mqtt_response, buf, 20);
  assert_true(rv == 2);
  rv = mqtt_unpack_publish_response(&mqtt_response, buf + 2);
  assert_true(response->qos_level == 0);
  assert_true(response->retain_flag == 1);
  assert_true(response->dup_flag == 0);
  assert_true(response->topic_name_size == 6);
  assert_true(memcmp(response->topic_name, "topic1", 6) == 0);
  assert_true(response->application_message_size == 10);
  assert_true(memcmp(response->application_message, "0123456789", 10) == 0);
}

static void TEST__utility__connect_disconnect([[maybe_unused]] void **state) {
  uint8_t buf[256];
  struct mqtt_client client;
  ssize_t rv;
  struct mqtt_response mqtt_response;

  client.socketfd = open_nb_socket(addr, port);
  make_socket_blocking(client.socketfd);
  assert_true(client.socketfd != -1);

  rv = mqtt_pack_connection_request(buf, sizeof(buf), "liam-123456", nullptr,
                                    nullptr, 0, nullptr, nullptr, 0, 30);
  assert_true(rv > 0);
  assert_true(send(client.socketfd, buf, rv, 0) != -1);

  /* receive connack */
  assert_true(recv(client.socketfd, buf, sizeof(buf), 0) != -1);
  rv = mqtt_unpack_fixed_header(&mqtt_response, buf, sizeof(buf));
  assert_true(rv > 0);
  assert_true(mqtt_unpack_connack_response(&mqtt_response, buf + rv) > 0);
  assert_true(mqtt_response.decoded.connack.return_code ==
              MQTT_CONNACK_ACCEPTED);

  /* disconnect */
  rv = mqtt_pack_disconnect(buf, sizeof(buf));
  assert_true(rv > 0);
  assert_true(send(client.socketfd, buf, rv, 0) != -1);

  /*close the socket */
  close(client.socketfd);
}

static void TEST__framing__connack([[maybe_unused]] void **state) {
  uint8_t buf[] = {(MQTT_CONTROL_CONNACK << 4) | 0, 2, 0,
                   MQTT_CONNACK_ACCEPTED};
  struct mqtt_response mqtt_response;
  ssize_t rv = mqtt_unpack_fixed_header(&mqtt_response, buf, sizeof(buf));
  assert_true(rv == 2);
  assert_true(mqtt_response.fixed_header.control_type == MQTT_CONTROL_CONNACK);

  /* unpack response */
  rv = mqtt_unpack_connack_response(&mqtt_response, buf + 2);
  assert_true(rv == 2);
  assert_true(mqtt_response.decoded.connack.session_present_flag == 0);
  assert_true(mqtt_response.decoded.connack.return_code ==
              MQTT_CONNACK_ACCEPTED);
}

static void TEST__framing__pubxxx([[maybe_unused]] void **state) {
  uint8_t buf[256];
  ssize_t rv;
  struct mqtt_response response;
  uint8_t puback_correct_bytes[] = {MQTT_CONTROL_PUBACK << 4, 2, 0, 213u};
  uint8_t pubrec_correct_bytes[] = {MQTT_CONTROL_PUBREC << 4, 2, 0, 213u};
  uint8_t pubrel_correct_bytes[] = {MQTT_CONTROL_PUBREL << 4 | 2u, 2, 0, 213u};
  uint8_t pubcomp_correct_bytes[] = {MQTT_CONTROL_PUBCOMP << 4, 2, 0, 213u};

  /* puback */
  rv = mqtt_pack_pubxxx_request(buf, 256, MQTT_CONTROL_PUBACK, 213u);
  assert_true(rv == 4);
  assert_true(memcmp(puback_correct_bytes, buf, 4) == 0);

  rv = mqtt_unpack_fixed_header(&response, buf, 256);
  assert_true(rv == 2);
  assert_true(response.fixed_header.control_type == MQTT_CONTROL_PUBACK);
  rv = mqtt_unpack_pubxxx_response(&response, buf + 2);
  assert_true(rv == 2);
  assert_true(response.decoded.puback.packet_id == 213u);

  /* pubrec */
  rv = mqtt_pack_pubxxx_request(buf, 256, MQTT_CONTROL_PUBREC, 213u);
  assert_true(rv == 4);
  assert_true(memcmp(pubrec_correct_bytes, buf, 4) == 0);

  rv = mqtt_unpack_fixed_header(&response, buf, 256);
  assert_true(rv == 2);
  assert_true(response.fixed_header.control_type == MQTT_CONTROL_PUBREC);
  rv = mqtt_unpack_pubxxx_response(&response, buf + 2);
  assert_true(rv == 2);
  assert_true(response.decoded.pubrec.packet_id == 213u);

  /* pubrel */
  rv = mqtt_pack_pubxxx_request(buf, 256, MQTT_CONTROL_PUBREL, 213u);
  assert_true(rv == 4);
  assert_true(memcmp(pubrel_correct_bytes, buf, 4) == 0);

  rv = mqtt_unpack_fixed_header(&response, buf, 256);
  assert_true(rv == 2);
  assert_true(response.fixed_header.control_type == MQTT_CONTROL_PUBREL);
  rv = mqtt_unpack_pubxxx_response(&response, buf + 2);
  assert_true(rv == 2);
  assert_true(response.decoded.pubrel.packet_id == 213u);

  /* pubcomp */
  rv = mqtt_pack_pubxxx_request(buf, 256, MQTT_CONTROL_PUBCOMP, 213u);
  assert_true(rv == 4);
  assert_true(memcmp(pubcomp_correct_bytes, buf, 4) == 0);

  rv = mqtt_unpack_fixed_header(&response, buf, 256);
  assert_true(rv == 2);
  assert_true(response.fixed_header.control_type == MQTT_CONTROL_PUBCOMP);
  rv = mqtt_unpack_pubxxx_response(&response, buf + 2);
  assert_true(rv == 2);
  assert_true(response.decoded.pubcomp.packet_id == 213u);
}

static void TEST__framing__subscribe([[maybe_unused]] void **state) {
  uint8_t buf[256];
  ssize_t rv;
  const uint8_t correct[] = {
      MQTT_CONTROL_SUBSCRIBE << 4 | 2u,
      23,
      0,
      132u,
      0,
      3,
      'a',
      '/',
      'b',
      0u,
      0,
      5,
      'b',
      'b',
      'b',
      '/',
      'x',
      1u,
      0,
      4,
      'c',
      '/',
      'd',
      'd',
      0u,
  };

  rv = mqtt_pack_subscribe_request(buf, 256, 132, "a/b", 0, "bbb/x", 1, "c/dd",
                                   0, nullptr);
  assert_true(rv == 25);
  assert_true(memcmp(buf, correct, 25) == 0);
}

static void TEST__framing__suback([[maybe_unused]] void **state) {
  ssize_t rv;
  struct mqtt_response response;
  const uint8_t buf[] = {MQTT_CONTROL_SUBACK << 4,
                         5,
                         0,
                         132u,
                         MQTT_SUBACK_SUCCESS_MAX_QOS_0,
                         MQTT_SUBACK_SUCCESS_MAX_QOS_1,
                         MQTT_SUBACK_FAILURE};
  rv = mqtt_unpack_fixed_header(&response, buf, sizeof(buf));
  assert_true(rv == 2);
  assert_true(response.fixed_header.control_type == MQTT_CONTROL_SUBACK);
  rv = mqtt_unpack_suback_response(&response, buf + 2);
  assert_true(rv == 5);
  assert_true(response.decoded.suback.packet_id == 132u);
  assert_true(response.decoded.suback.num_return_codes == 3);
  assert_true(response.decoded.suback.return_codes[0] ==
              MQTT_SUBACK_SUCCESS_MAX_QOS_0);
  assert_true(response.decoded.suback.return_codes[1] ==
              MQTT_SUBACK_SUCCESS_MAX_QOS_1);
  assert_true(response.decoded.suback.return_codes[2] == MQTT_SUBACK_FAILURE);
}

static void TEST__framing__unsubscribe([[maybe_unused]] void **state) {
  uint8_t buf[256];
  ssize_t rv;
  const uint8_t correct[] = {
      MQTT_CONTROL_UNSUBSCRIBE << 4 | 2u,
      20,
      0,
      132u,
      0,
      3,
      'a',
      '/',
      'b',
      0,
      5,
      'b',
      'b',
      'b',
      '/',
      'x',
      0,
      4,
      'c',
      '/',
      'd',
      'd',
  };

  rv = mqtt_pack_unsubscribe_request(buf, 256, 132, "a/b", "bbb/x", "c/dd",
                                     nullptr);
  assert_true(rv == 22);
  assert_true(memcmp(buf, correct, sizeof(correct)) == 0);
}

static void TEST__framing__unsuback([[maybe_unused]] void **state) {
  uint8_t buf[] = {MQTT_CONTROL_UNSUBACK << 4, 2, 0, 213u};
  ssize_t rv;
  struct mqtt_response response;

  rv = mqtt_unpack_fixed_header(&response, buf, 4);
  assert_true(rv == 2);
  assert_true(response.fixed_header.control_type == MQTT_CONTROL_UNSUBACK);
  rv = mqtt_unpack_unsuback_response(&response, buf + 2);
  assert_true(rv == 2);
  assert_true(response.decoded.unsuback.packet_id == 213u);
}

static void TEST__framing__disconnect([[maybe_unused]] void **state) {
  uint8_t buf[2];
  assert_true(mqtt_pack_disconnect(buf, 2) == 2);
}

static void TEST__framing__ping([[maybe_unused]] void **state) {
  uint8_t buf[2];
  struct mqtt_response response;
  struct mqtt_fixed_header *fixed_header = &response.fixed_header;
  assert_true(mqtt_pack_ping_request(buf, 2) == 2);
  assert_true(mqtt_unpack_fixed_header(&response, buf, 2) == 2);
  assert_true(fixed_header->control_type == MQTT_CONTROL_PINGREQ);
  assert_true(fixed_header->remaining_length == 0);
}

static void TEST__framing__oversized_request_rejected(
    [[maybe_unused]] void **state) {
  uint8_t buf[64];
  char *too_long = (char *)calloc((size_t)UINT16_MAX + 2u, sizeof(char));

  assert_true(too_long != nullptr);
  memset(too_long, 'a', (size_t)UINT16_MAX + 1u);
  too_long[(size_t)UINT16_MAX + 1u] = '\0';

  assert_true(mqtt_pack_publish_request(buf, sizeof(buf), too_long, 1u, "x", 1u,
                                        MQTT_PUBLISH_QOS_0) ==
              MQTT_ERROR_MALFORMED_REQUEST);
  assert_true(mqtt_pack_connection_request(buf, sizeof(buf), "client", "topic",
                                           buf, (size_t)UINT16_MAX + 1u,
                                           nullptr, nullptr, 0u, 30u) ==
              MQTT_ERROR_MALFORMED_REQUEST);
  assert_true(mqtt_pack_subscribe_request(buf, sizeof(buf), 1u,
                                          (const char *)nullptr) ==
              MQTT_ERROR_MALFORMED_REQUEST);
  assert_true(mqtt_pack_unsubscribe_request(buf, sizeof(buf), 1u,
                                            (const char *)nullptr) ==
              MQTT_ERROR_MALFORMED_REQUEST);

  free(too_long);
}

static void TEST__utility__ping([[maybe_unused]] void **state) {
  uint8_t buf[256];
  struct mqtt_client client;
  ssize_t rv;
  struct mqtt_response mqtt_response;

  client.socketfd = open_nb_socket(addr, port);
  make_socket_blocking(client.socketfd);
  assert_true(client.socketfd != -1);

  rv = mqtt_pack_connection_request(buf, sizeof(buf), "this-is-me", nullptr,
                                    nullptr, 0, nullptr, nullptr, 0, 30);
  assert_true(rv > 0);
  assert_true(send(client.socketfd, buf, rv, 0) != -1);

  /* receive connack */
  assert_true(recv(client.socketfd, buf, sizeof(buf), 0) != -1);
  rv = mqtt_unpack_fixed_header(&mqtt_response, buf, sizeof(buf));
  assert_true(rv > 0);
  assert_true(mqtt_unpack_connack_response(&mqtt_response, buf + rv) > 0);
  assert_true(mqtt_response.decoded.connack.return_code ==
              MQTT_CONNACK_ACCEPTED);

  /* send ping request */
  rv = mqtt_pack_ping_request(buf, sizeof(buf));
  assert_true(rv > 0);
  assert_true(send(client.socketfd, buf, rv, 0) != -1);

  /* receive ping response */
  assert_true(recv(client.socketfd, buf, sizeof(buf), 0) != -1);
  rv = mqtt_unpack_fixed_header(&mqtt_response, buf, sizeof(buf));
  assert_true(rv > 0);
  assert_true(mqtt_response.fixed_header.control_type == MQTT_CONTROL_PINGRESP);

  /* disconnect */
  rv = mqtt_pack_disconnect(buf, sizeof(buf));
  assert_true(rv > 0);
  assert_true(send(client.socketfd, buf, rv, 0) != -1);

  /*close the socket */
  close(client.socketfd);
}

constexpr int QM_SZ = (int)sizeof(struct mqtt_queued_message);
static void TEST__utility__message_queue([[maybe_unused]] void **unused) {
  alignas(struct mqtt_queued_message) uint8_t mem[32 + 4 * QM_SZ];
  struct mqtt_message_queue mq;
  struct mqtt_queued_message *tail;
  mqtt_mq_init(&mq, mem, sizeof(mem));

  /* check that it fills up correctly */
  assert_true(mqtt_mq_length(&mq) == 0);
  assert_true(mq.curr_sz == 32 + 3 * QM_SZ);
  memset(mq.curr, 0, 8);
  tail = mqtt_mq_register(&mq, 8);
  tail->control_type = 2;
  tail->packet_id = 111;
  assert_true(mqtt_mq_length(&mq) == 1);
  assert_true(mq.curr_sz == 24 + 2 * QM_SZ);
  memset(mq.curr, 1, 8);
  tail = mqtt_mq_register(&mq, 8);
  tail->control_type = 3;
  tail->packet_id = 222;
  assert_true(mqtt_mq_length(&mq) == 2);
  assert_true(mq.curr_sz == 16 + 1 * QM_SZ);
  memset(mq.curr, 2, 8);
  tail = mqtt_mq_register(&mq, 8);
  tail->control_type = 4;
  tail->packet_id = 333;
  assert_true(mqtt_mq_length(&mq) == 3);
  assert_true(mq.curr_sz == 8);
  memset(mq.curr, 3, 8);
  tail = mqtt_mq_register(&mq, 8);
  tail->control_type = 5;
  tail->packet_id = 444;
  assert_true(mqtt_mq_length(&mq) == 4);
  assert_true(mq.curr_sz == 0);
  assert_true(mq.curr == (uint8_t *)mq.queue_tail);

  /* check that start's are correct */
  for (unsigned int i = 0; i < 4; ++i) {
    assert_true(mqtt_mq_get(&mq, i)->start == (uint8_t *)mq.mem_start + 8 * i);
    for (int j = 0; j < 8; ++j) {
      assert_true(mqtt_mq_get(&mq, i)->start[j] == i);
    }

    assert_true(mqtt_mq_get(&mq, i)->control_type == i + 2);
    assert_true(mqtt_mq_get(&mq, i)->packet_id == 111 * (i + 1));
  }

  /* check that it cleans correctly */
  mqtt_mq_clean(&mq); /* should do nothing */
  assert_true(mqtt_mq_length(&mq) == 4);
  assert_true(mq.curr_sz == 0);
  assert_true(mq.curr == (uint8_t *)mq.queue_tail);

  /* try clearing middle (should do nothing) */
  mqtt_mq_get(&mq, 1)->state = MQTT_QUEUED_COMPLETE;
  mqtt_mq_get(&mq, 0)->state = MQTT_QUEUED_AWAITING_ACK;
  mqtt_mq_clean(&mq);
  assert_true(mqtt_mq_length(&mq) == 4);
  assert_true(mq.curr_sz == 0);
  assert_true(mq.curr == (uint8_t *)mq.queue_tail);

  /* complete first then clean (should clear 2) */
  mqtt_mq_get(&mq, 0)->state = MQTT_QUEUED_COMPLETE;
  mqtt_mq_clean(&mq);
  assert_true(mqtt_mq_length(&mq) == 2);
  assert_true(mq.curr_sz == 16 + 1 * QM_SZ);
  assert_true(mq.curr == mem + 16);

  /* check that start's are correct */
  for (unsigned int i = 0; i < 2; ++i) {
    assert_true(mqtt_mq_get(&mq, i)->start == (uint8_t *)mq.mem_start + 8 * i);
    for (int j = 0; j < 8; ++j) {
      assert_true(mqtt_mq_get(&mq, i)->start[j] == i + 2); /* check value */
    }
    assert_true(mqtt_mq_get(&mq, i)->control_type == i + 4);
    assert_true(mqtt_mq_get(&mq, i)->packet_id == 111 * (i + 3));
  }

  /* remove the last two */
  mqtt_mq_get(&mq, 0)->state = MQTT_QUEUED_COMPLETE;
  mqtt_mq_get(&mq, 1)->state = MQTT_QUEUED_COMPLETE;
  mqtt_mq_clean(&mq);
  assert_true(mqtt_mq_length(&mq) == 0);
  assert_true(mq.curr_sz == 32 + 3 * QM_SZ);
  assert_true((void *)mq.queue_tail == mq.mem_end);
}

static void TEST__utility__message_queue_tiny_buffer(
    [[maybe_unused]] void **unused) {
  alignas(struct mqtt_queued_message) uint8_t mem[QM_SZ - 1];
  struct mqtt_message_queue mq;

  mqtt_mq_init(&mq, mem, sizeof(mem));

  assert_true(mqtt_mq_length(&mq) == 0);
  assert_true(mq.curr == mem);
  assert_true(mq.curr_sz == 0);
  assert_true((void *)mq.queue_tail == mq.mem_end);
}

static void TEST__utility__pid_lfsr([[maybe_unused]] void **unused) {
  struct mqtt_client client;
  uint8_t send[256], recv[256];
  mqtt_init(&client, -1, send, 256, recv, 256, nullptr);
  client.pid_lfsr = 163u;
  uint32_t period = 0;
  do {
    __mqtt_next_pid(&client);
    period++;
  } while (client.pid_lfsr != 163u && client.pid_lfsr != 0);
  assert_true(period == 65535u);
}

static void TEST__utility__client_state_reset([[maybe_unused]] void **unused) {
  alignas(struct mqtt_queued_message) uint8_t send[256];
  uint8_t recv[128];
  struct mqtt_client client = {0};
  int state = 123;

  client.keep_alive = 12;
  client.number_of_timeouts = 9;
  client.number_of_keep_alives = 7;
  client.typical_response_time = 1.5f;
  client.pid_lfsr = 42u;
  client.send_offset = 8u;
  client.time_of_last_send = (mqtt_pal_time_t)99;
  client.publish_response_callback_state = &state;

  assert_true(mqtt_init(&client, -1, send, sizeof(send), recv, sizeof(recv),
                        publish_callback) == MQTT_OK);
  assert_true(client.keep_alive == 0);
  assert_true(client.number_of_timeouts == 0);
  assert_true(client.number_of_keep_alives == 0);
  assert_true(client.typical_response_time == -1.0f);
  assert_true(client.pid_lfsr == 0u);
  assert_true(client.send_offset == 0u);
  assert_true(client.time_of_last_send == 0);
  assert_true(client.publish_response_callback == publish_callback);
  assert_true(client.publish_response_callback_state == nullptr);

  client.keep_alive = 22;
  client.number_of_timeouts = 2;
  client.number_of_keep_alives = 3;
  client.typical_response_time = 4.5f;
  client.pid_lfsr = 7u;
  client.send_offset = 3u;
  client.time_of_last_send = (mqtt_pal_time_t)88;
  client.publish_response_callback_state = &state;
  client.response_timeout = 77;

  mqtt_reinit(&client, -1, send, sizeof(send), recv, sizeof(recv));
  assert_true(client.keep_alive == 0);
  assert_true(client.number_of_timeouts == 0);
  assert_true(client.number_of_keep_alives == 0);
  assert_true(client.typical_response_time == -1.0f);
  assert_true(client.pid_lfsr == 0u);
  assert_true(client.send_offset == 0u);
  assert_true(client.time_of_last_send == 0);
  assert_true(client.publish_response_callback == publish_callback);
  assert_true(client.publish_response_callback_state == &state);
  assert_true(client.response_timeout == 77);
}

static void TEST__utility__null_publish_callback([[maybe_unused]] void **unused) {
  (void)unused;
  alignas(struct mqtt_queued_message) uint8_t sendmem[256];
  uint8_t recvmem[256];
  struct mqtt_client client;
  struct mqtt_response_publish publish = {
      .dup_flag = 0u,
      .qos_level = 1u,
      .retain_flag = 0u,
      .topic_name_size = 5u,
      .topic_name = "topic",
      .packet_id = 42u,
      .application_message = "ok",
      .application_message_size = 2u,
  };

  assert_true(mqtt_init(&client, -1, sendmem, sizeof(sendmem), recvmem,
                        sizeof(recvmem), nullptr) == MQTT_OK);
  client.error = MQTT_OK;

  assert_true(__mqtt_handle_publish(&client, &publish) == MQTT_OK);
  assert_true(client.error == MQTT_OK);
  assert_true(mqtt_mq_length(&client.mq) == 1);
  assert_true(mqtt_mq_get(&client.mq, 0)->control_type == MQTT_CONTROL_PUBACK);
  assert_true(mqtt_mq_get(&client.mq, 0)->packet_id == 42u);
}

void publish_callback([[maybe_unused]] void **state,
                      [[maybe_unused]] struct mqtt_response_publish *publish) {
  /*char *name = (char*) malloc(publish->topic_name_size + 1);
  memcpy(name, publish->topic_name, publish->topic_name_size);
  name[publish->topic_name_size] = '\0';
  printf("Received a PUBLISH(topic=%s, DUP=%d, QOS=%d, RETAIN=%d, pid=%d) from
  the broker. Data='%s'\n", name, publish->dup_flag, publish->qos_level,
  publish->retain_flag, publish->packet_id, (const char*)
  (publish->application_message)
  );
  free(name);*/
  **(int **)state += 1;
}

static void TEST__api__connect_ping_disconnect([[maybe_unused]] void **unused) {
  alignas(struct mqtt_queued_message) uint8_t sendmem[2048];
  uint8_t recvmem[1024];
  ssize_t rv;
  struct mqtt_client client;

  int sockfd = open_nb_socket(addr, port);

  /* initialize */
  mqtt_init(&client, sockfd, sendmem, sizeof(sendmem), recvmem, sizeof(recvmem),
            publish_callback);

  /* connect */
  assert_true(mqtt_connect(&client, "liam-123", nullptr, nullptr, 0, nullptr,
                           nullptr, 0, 30) > 0);
  assert_true(__mqtt_send(&client) > 0);
  while (mqtt_mq_length(&client.mq) > 0) {
    assert_true(__mqtt_recv(&client) > 0);
    mqtt_mq_clean(&client.mq);
    mqttc_sleep_us(10000);
  }

  /* ping */
  assert_true(mqtt_ping(&client) > 0);
  while (mqtt_mq_length(&client.mq) > 0) {
    rv = __mqtt_send(&client);
    if (rv <= 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    rv = __mqtt_recv(&client);
    if (rv <= 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    mqtt_mq_clean(&client.mq);
    mqttc_sleep_us(10000);
  }

  /* disconnect */
  assert_true(client.error == MQTT_OK);
  assert_true(mqtt_disconnect(&client) > 0);
  assert_true(__mqtt_send(&client) > 0);
}

static void
TEST__api__publish_subscribe__single([[maybe_unused]] void **unused) {
  alignas(struct mqtt_queued_message) uint8_t sendmem1[2048], sendmem2[2048];
  uint8_t recvmem1[1024], recvmem2[1024];
  struct mqtt_client sender, receiver;

  int state = 0;

  int sockfd = open_nb_socket(addr, port);
  mqtt_init(&sender, sockfd, sendmem1, sizeof(sendmem1), recvmem1,
            sizeof(recvmem1), publish_callback);

  sockfd = open_nb_socket(addr, port);
  mqtt_init(&receiver, sockfd, sendmem2, sizeof(sendmem2), recvmem2,
            sizeof(recvmem2), publish_callback);
  receiver.publish_response_callback_state = &state;

  /* connect both */
  assert_true(mqtt_connect(&sender, "liam-123", nullptr, nullptr, 0, nullptr,
                           nullptr, 0, 30) > 0);
  assert_true(mqtt_connect(&receiver, "liam-234", nullptr, nullptr, 0, nullptr,
                           nullptr, 0, 30) > 0);
  assert_true(__mqtt_send(&sender) > 0);
  assert_true(__mqtt_send(&receiver) > 0);
  while (mqtt_mq_length(&sender.mq) > 0 || mqtt_mq_length(&receiver.mq) > 0) {
    assert_true(__mqtt_recv(&sender) > 0);
    mqtt_mq_clean(&sender.mq);
    assert_true(__mqtt_recv(&receiver) > 0);
    mqtt_mq_clean(&receiver.mq);
    mqttc_sleep_us(10000);
  }

  /* subscribe receiver*/
  assert_true(mqtt_subscribe(&receiver, "liam-test-topic", 2) > 0);
  assert_true(__mqtt_send(&receiver) > 0);
  while (mqtt_mq_length(&receiver.mq) > 0) {
    assert_true(__mqtt_recv(&receiver) > 0);
    mqtt_mq_clean(&receiver.mq);
    mqttc_sleep_us(10000);
  }

  /* publish from sender */
  assert_true(mqtt_publish(&sender, "liam-test-topic", "data", 5,
                           MQTT_PUBLISH_QOS_0) > 0);
  assert_true(__mqtt_send(&sender) > 0);

  time_t start = time(nullptr);
  while (state == 0 && time(nullptr) < start + 10) {
    assert_true(__mqtt_recv(&receiver) > 0);
    mqttc_sleep_us(10000);
  }

  assert_true(state == 1);

  /* disconnect */
  assert_true(sender.error == MQTT_OK);
  assert_true(receiver.error == MQTT_OK);
  assert_true(mqtt_disconnect(&sender) > 0);
  assert_true(mqtt_disconnect(&receiver) > 0);
  assert_true(__mqtt_send(&sender) > 0);
  assert_true(__mqtt_send(&receiver) > 0);
}

constexpr size_t TEST_PACKET_SIZE = 149;
constexpr size_t TEST_DATA_SIZE = 128;
constexpr size_t TEST_SEND_ALIGNMENT = alignof(struct mqtt_queued_message);
constexpr size_t TEST_SENDMEM_SIZE =
    TEST_PACKET_SIZE * 4 + sizeof(struct mqtt_queued_message) * 4;
constexpr size_t TEST_SENDMEM_ALIGNED =
    TEST_SENDMEM_SIZE +
    (TEST_SEND_ALIGNMENT - (TEST_SENDMEM_SIZE % TEST_SEND_ALIGNMENT)) %
        TEST_SEND_ALIGNMENT;
static void
TEST__api__publish_subscribe__multiple([[maybe_unused]] void **unused) {
  alignas(struct mqtt_queued_message) uint8_t sendmem1[TEST_SENDMEM_ALIGNED],
      sendmem2[TEST_SENDMEM_ALIGNED];
  uint8_t recvmem1[TEST_PACKET_SIZE], recvmem2[TEST_PACKET_SIZE];
  uint8_t payload[TEST_DATA_SIZE];
  struct mqtt_client sender, receiver;
  ssize_t rv;

  int state = 0;

  /* initialize sender */
  int sockfd = open_nb_socket(addr, port);
  mqtt_init(&sender, sockfd, sendmem1, sizeof(sendmem1), recvmem1,
            sizeof(recvmem1), publish_callback);

  sockfd = open_nb_socket(addr, port);
  mqtt_init(&receiver, sockfd, sendmem2, sizeof(sendmem2), recvmem2,
            sizeof(recvmem2), publish_callback);
  receiver.publish_response_callback_state = &state;

  /* connect both */
  if ((rv = mqtt_connect(&sender, "liam-123", nullptr, nullptr, 0, nullptr,
                         nullptr, MQTT_CONNECT_CLEAN_SESSION, 30)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = mqtt_connect(&receiver, "liam-234", nullptr, nullptr, 0, nullptr,
                         nullptr, MQTT_CONNECT_CLEAN_SESSION, 30)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = __mqtt_send(&sender)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = __mqtt_send(&receiver)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  while (mqtt_mq_length(&sender.mq) > 0 || mqtt_mq_length(&receiver.mq) > 0) {
    if ((rv = __mqtt_recv(&sender)) <= 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(rv > 0);
    }
    mqtt_mq_clean(&sender.mq);
    if ((rv = __mqtt_recv(&receiver)) <= 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(rv > 0);
    }
    mqtt_mq_clean(&receiver.mq);
    mqttc_sleep_us(10000);
  }

  state = 0;

  /* publish with retain */
  fill_payload(payload, sizeof(payload), "this was initial retain with qos 1");
  if ((rv = mqtt_publish(&sender, "liam-test-ret1", payload, sizeof(payload),
                         MQTT_PUBLISH_QOS_1 | MQTT_PUBLISH_RETAIN)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = __mqtt_send(&sender)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }

  /* subscribe receiver*/
  if ((rv = mqtt_subscribe(&receiver, "liam-test-qos0", 0)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = mqtt_subscribe(&receiver, "liam-test-qos1", 1)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = mqtt_subscribe(&receiver, "liam-test-qos2", 2)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = mqtt_subscribe(&receiver, "liam-test-ret1", 2)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = __mqtt_send(&receiver)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }

  /* wait for retained publish and receiver and sender have 0 length mq's */
  time_t start = time(nullptr);
  while (start + 10 > time(nullptr) &&
         (state < 1 || mqtt_mq_length(&receiver.mq) > 0 ||
          mqtt_mq_length(&sender.mq) > 0)) {
    if ((rv = mqtt_sync(&receiver)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    mqtt_mq_clean(&receiver.mq);
    if ((rv = mqtt_sync(&sender)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    mqtt_mq_clean(&sender.mq);
    mqttc_sleep_us(10000);
  }

  /* make sure that we publish was called */
  assert_true(mqtt_mq_length(&receiver.mq) == 0);
  assert_true(mqtt_mq_length(&sender.mq) == 0);
  assert_true(state == 1);

  /* now publish 4 perfect sized messages */
  fill_payload(payload, sizeof(payload), "retain with qos1");
  if ((rv = mqtt_publish(&sender, "liam-test-ret1", payload, sizeof(payload),
                         MQTT_PUBLISH_QOS_1)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  fill_payload(payload, sizeof(payload), "test with qos 0");
  if ((rv = mqtt_publish(&sender, "liam-test-qos0", payload, sizeof(payload),
                         MQTT_PUBLISH_QOS_0)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  fill_payload(payload, sizeof(payload), "test with qos 1");
  if ((rv = mqtt_publish(&sender, "liam-test-qos1", payload, sizeof(payload),
                         MQTT_PUBLISH_QOS_1)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  fill_payload(payload, sizeof(payload), "test with qos 2");
  if ((rv = mqtt_publish(&sender, "liam-test-qos2", payload, sizeof(payload),
                         MQTT_PUBLISH_QOS_2)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = __mqtt_send(&sender)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  assert_true(sender.error == MQTT_OK);
  assert_true(sender.mq.curr_sz == 0);

  /* give 2 seconds for sending and receiving (also don't manually clean) */
  start = time(nullptr);
  while (time(nullptr) < start + 8) {
    if ((rv = __mqtt_recv(&receiver)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    if ((rv = __mqtt_recv(&sender)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    if ((rv = __mqtt_send(&receiver)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    if ((rv = __mqtt_send(&sender)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    mqttc_sleep_us(10000);
  }

  if (state != 5) {
    printf("error: state == %d\n", state);
    assert_true(state == 5);
  }

  /* test unsubscribe */
  if ((rv = mqtt_unsubscribe(&receiver, "liam-test-qos1")) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }

  /*sleep for 2 seconds while unsubscribe is sending */
  start = time(nullptr);
  while (time(nullptr) < start + 2) {
    if ((rv = __mqtt_recv(&receiver)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    if ((rv = __mqtt_recv(&sender)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    if ((rv = __mqtt_send(&receiver)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    if ((rv = __mqtt_send(&sender)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    mqttc_sleep_us(10000);
  }
  /* publish qos1 (should be received by receiver) */
  fill_payload(payload, sizeof(payload), "test with qos 1");
  if ((rv = mqtt_publish(&sender, "liam-test-qos1", payload, sizeof(payload),
                         MQTT_PUBLISH_QOS_1)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  /*sleep for 2 seconds to give the publish a chance  */
  start = time(nullptr);
  while (time(nullptr) < start + 2) {
    if ((rv = __mqtt_recv(&receiver)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    if ((rv = __mqtt_recv(&sender)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    if ((rv = __mqtt_send(&receiver)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    if ((rv = __mqtt_send(&sender)) < 0) {
      printf("error: %s\n", mqtt_error_str(rv));
      assert_true(0);
    }
    mqttc_sleep_us(10000);
  }

  /* check that the callback wasn't called */
  if (state != 5) {
    printf("error: state == %d\n", state);
    assert_true(state == 5);
  }

  /* disconnect */
  assert_true(sender.error == MQTT_OK);
  assert_true(receiver.error == MQTT_OK);
  if ((rv = mqtt_disconnect(&sender)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = mqtt_disconnect(&receiver)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = __mqtt_send(&sender)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
  if ((rv = __mqtt_send(&receiver)) <= 0) {
    printf("error: %s\n", mqtt_error_str(rv));
    assert_true(rv > 0);
  }
}

int main(int argc, const char *argv[]) {
  /* get address (argv[1] if present) */
  if (argc > 1) {
    addr = argv[1];
  }

  /* get port number (argv[2] if present) */
  if (argc > 2) {
    port = argv[2];
  }

  printf("Staring MQTT-C unit-tests.\n");
  printf("Using broker: \"%s:%s\"\n\n", addr, port);

#if defined(WIN32)
  WSADATA wsaData;
  int iResult = WSAStartup(MAKEWORD(2, 2), &wsaData);
  if (iResult != NO_ERROR) {
    fprintf(stderr, "Failed to init sockets: %i\n", iResult);
    return iResult;
  }
#endif

  static const struct test_case framing_tests[] = {
      {"framing__fixed_header", TEST__framing__fixed_header},
      {"framing__connect", TEST__framing__connect},
      {"framing__connack", TEST__framing__connack},
      {"framing__publish", TEST__framing__publish},
      {"framing__oversized_request_rejected",
       TEST__framing__oversized_request_rejected},
      {"framing__pubxxx", TEST__framing__pubxxx},
      {"framing__subscribe", TEST__framing__subscribe},
      {"framing__suback", TEST__framing__suback},
      {"framing__unsubscribe", TEST__framing__unsubscribe},
      {"framing__unsuback", TEST__framing__unsuback},
      {"framing__ping", TEST__framing__ping},
      {"framing__disconnect", TEST__framing__disconnect},
  };

  run_group("MQTT Packet Serialization/Deserialization Tests", framing_tests,
            sizeof(framing_tests) / sizeof(framing_tests[0]));

  static const struct test_case util_tests[] = {
      {"utility__message_queue", TEST__utility__message_queue},
      {"utility__message_queue_tiny_buffer",
       TEST__utility__message_queue_tiny_buffer},
      {"utility__pid_lfsr", TEST__utility__pid_lfsr},
      {"utility__client_state_reset", TEST__utility__client_state_reset},
      {"utility__null_publish_callback", TEST__utility__null_publish_callback},
      {"utility__connect_disconnect", TEST__utility__connect_disconnect},
      {"utility__ping", TEST__utility__ping},
  };

  run_group("MQTT-C Utilities Tests", util_tests,
            sizeof(util_tests) / sizeof(util_tests[0]));

  static const struct test_case api_tests[] = {
      {"api__connect_ping_disconnect", TEST__api__connect_ping_disconnect},
      {"api__publish_subscribe__single", TEST__api__publish_subscribe__single},
      {"api__publish_subscribe__multiple",
       TEST__api__publish_subscribe__multiple},
  };

  run_group("MQTT-C API Tests", api_tests,
            sizeof(api_tests) / sizeof(api_tests[0]));

#if defined(WIN32)
  WSACleanup();
#endif

  return 0;
}
