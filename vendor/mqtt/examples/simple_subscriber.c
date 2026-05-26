
/**
 * @file
 * A simple program that subscribes to a topic.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mqtt/mqtt.h"
#include "templates/posix_sockets.h"

/**
 * @brief The function will be called whenever a PUBLISH message is received.
 */
static void
publish_callback([[maybe_unused]] void **unused,
                 [[maybe_unused]] struct mqtt_response_publish *published);

/**
 * @brief The client's refresher. This function triggers back-end routines to
 *        handle ingress/egress traffic to the broker.
 *
 * @note All this function needs to do is call \ref __mqtt_recv and
 *       \ref __mqtt_send every so often. I've picked 100 ms meaning that
 *       client ingress/egress traffic will be handled every 100 ms.
 */
static void *client_refresher(void *client);

/**
 * @brief Safelty closes the \p sockfd and cancels the \p client_daemon before
 * \c exit.
 */
static void exit_example(int status, int sockfd, pthread_t *client_daemon);

int main(int argc, const char *argv[]) {
  const char *addr;
  const char *port;
  const char *topic;

  /* get address (argv[1] if present) */
  if (argc > 1) {
    addr = argv[1];
  } else {
    addr = "test.mosquitto.org";
  }

  /* get port number (argv[2] if present) */
  if (argc > 2) {
    port = argv[2];
  } else {
    port = "1883";
  }

  /* get the topic name to publish */
  if (argc > 3) {
    topic = argv[3];
  } else {
    topic = "datetime";
  }

  /* open the non-blocking TCP socket (connecting to the broker) */
  int sockfd = open_nb_socket(addr, port);

  if (sockfd == -1) {
    perror("Failed to open socket: ");
    exit_example(EXIT_FAILURE, sockfd, nullptr);
  }

  /* setup a client */
  struct mqtt_client client;
  uint8_t sendbuf[2048]; /* sendbuf should be large enough to hold multiple
                            whole mqtt messages */
  uint8_t recvbuf[1024]; /* recvbuf should be large enough any whole mqtt
                            message expected to be received */
  mqtt_init(&client, sockfd, sendbuf, sizeof(sendbuf), recvbuf, sizeof(recvbuf),
            publish_callback);
  /* Create an anonymous session */
  const char *client_id = nullptr;
  /* Ensure we have a clean session */
  uint8_t connect_flags = MQTT_CONNECT_CLEAN_SESSION;
  /* Send connection request to the broker. */
  mqtt_connect(&client, client_id, nullptr, nullptr, 0, nullptr, nullptr,
               connect_flags, 400);

  /* check that we don't have any errors */
  if (client.error != MQTT_OK) {
    fprintf(stderr, "error: %s\n", mqtt_error_str(client.error));
    exit_example(EXIT_FAILURE, sockfd, nullptr);
  }

  /* start a thread to refresh the client (handle egress and ingree client
   * traffic) */
  pthread_t client_daemon;
  if (pthread_create(&client_daemon, nullptr, client_refresher, &client)) {
    fprintf(stderr, "Failed to start client daemon.\n");
    exit_example(EXIT_FAILURE, sockfd, nullptr);
  }

  /* subscribe */
  mqtt_subscribe(&client, topic, 0);

  /* start publishing the time */
  printf("%s listening for '%s' messages.\n", argv[0], topic);
  printf("Press CTRL-D to exit.\n\n");

  /* block */
  while (fgetc(stdin) != EOF)
    ;

  /* disconnect */
  printf("\n%s disconnecting from %s\n", argv[0], addr);
  sleep(1);

  /* exit */
  exit_example(EXIT_SUCCESS, sockfd, &client_daemon);
}

static void exit_example(int status, int sockfd, pthread_t *client_daemon) {
  if (sockfd != -1)
    close(sockfd);
  if (client_daemon != nullptr)
    pthread_cancel(*client_daemon);
  exit(status);
}

static void
publish_callback([[maybe_unused]] void **unused,
                 [[maybe_unused]] struct mqtt_response_publish *published) {
  /* note that published->topic_name is NOT null-terminated (here we'll change
   * it to a c-string) */
  char *topic_name =
      (char *)calloc(published->topic_name_size + 1u, sizeof(char));
  if (topic_name == nullptr) {
    fprintf(stderr, "Failed to allocate topic buffer\n");
    return;
  }
  memcpy(topic_name, published->topic_name, published->topic_name_size);
  topic_name[published->topic_name_size] = '\0';

  printf("Received publish('%s'): %s\n", topic_name,
         (const char *)published->application_message);

  free(topic_name);
}

static void *client_refresher(void *client) {
  static const struct timespec refresh_delay = {.tv_sec = 0,
                                                .tv_nsec = 100'000'000L};
  while (true) {
    mqtt_sync((struct mqtt_client *)client);
    nanosleep(&refresh_delay, nullptr);
  }
  return nullptr;
}
