
/**
 * @file
 */
#include <process.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <windows.h>

#include "mqtt/mqtt.h"
#include "templates/openssl_sockets.h"

/**
 * @brief The function that would be called whenever a PUBLISH is received.
 *
 * @note This function is not used in this example.
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
static void client_refresher(void *client);

/**
 * @brief Safelty closes the \p sockfd and cancels the \p client_daemon before
 * \c exit.
 */
static void exit_example(int status, BIO *sockfd, SSL_CTX *ssl_ctx);

/**
 * A simple program to that publishes the current time whenever ENTER is
 * pressed.
 */
int main(int argc, const char *argv[]) {
  const char *addr;
  const char *port;
  const char *topic;
  const char *ca_file;

  /* Load OpenSSL */
  SSL_load_error_strings();
  ERR_load_BIO_strings();
  OpenSSL_add_all_algorithms();
  SSL_library_init();

  SSL_CTX *ssl_ctx = nullptr;
  BIO *sockfd;

  if (argc > 1) {
    ca_file = argv[1];
  } else {
    printf("error: path to the CA certificate to use\n");
    exit(1);
  }

  /* get address (argv[2] if present) */
  if (argc > 2) {
    addr = argv[2];
  } else {
    addr = "test.mosquitto.org";
  }

  /* get port number (argv[3] if present) */
  if (argc > 3) {
    port = argv[3];
  } else {
    port = "8883";
  }

  /* get the topic name to publish */
  if (argc > 4) {
    topic = argv[4];
  } else {
    topic = "datetime";
  }

  /* open the non-blocking TCP socket (connecting to the broker) */
  open_nb_socket(&sockfd, &ssl_ctx, addr, port, ca_file, nullptr, nullptr,
                 nullptr);

  if (sockfd == nullptr) {
    exit_example(EXIT_FAILURE, sockfd, ssl_ctx);
  }

  /* setup a client */
  struct mqtt_client client;
  uint8_t sendbuf[2048]; /* sendbuf should be large enough to hold multiple
                            whole mqtt messages */
  uint8_t recvbuf[1024]; /* recvbuf should be large enough any whole mqtt
                            message expected to be received */
  mqtt_init(&client, sockfd, sendbuf, sizeof(sendbuf), recvbuf, sizeof(recvbuf),
            publish_callback);
  mqtt_connect(&client, "publishing_client", nullptr, nullptr, 0, nullptr,
               nullptr, 0, 400);

  /* check that we don't have any errors */
  if (client.error != MQTT_OK) {
    fprintf(stderr, "error: %s\n", mqtt_error_str(client.error));
    exit_example(EXIT_FAILURE, sockfd, ssl_ctx);
  }

  /* start a thread to refresh the client (handle egress and ingree client
   * traffic) */
  if (_beginthread(client_refresher, 0, &client) == -1) {
    fprintf(stderr, "Failed to start client daemon.\n");
    exit_example(EXIT_FAILURE, sockfd, ssl_ctx);
  }

  /* start publishing the time */
  printf("%s is ready to begin publishing the time.\n", argv[0]);
  printf("Press ENTER to publish the current time.\n");
  printf("Press CTRL-D (or any other key) to exit.\n\n");
  while (fgetc(stdin) == '\n') {
    /* get the current time */
    time_t timer;
    time(&timer);
    struct tm *tm_info = localtime(&timer);
    char timebuf[26];
    strftime(timebuf, 26, "%Y-%m-%d %H:%M:%S", tm_info);

    /* print a message */
    char application_message[256];
    snprintf(application_message, sizeof(application_message), "The time is %s",
             timebuf);
    printf("%s published : \"%s\"", argv[0], application_message);

    /* publish the time */
    mqtt_publish(&client, topic, application_message,
                 strlen(application_message) + 1, MQTT_PUBLISH_QOS_2);

    /* check for errors */
    if (client.error != MQTT_OK) {
      fprintf(stderr, "error: %s\n", mqtt_error_str(client.error));
      exit_example(EXIT_FAILURE, sockfd, ssl_ctx);
    }
  }

  /* disconnect */
  printf("\n%s disconnecting from %s\n", argv[0], addr);
  Sleep(1000);

  /* exit */
  exit_example(EXIT_SUCCESS, sockfd, ssl_ctx);
}

static void exit_example(int status, BIO *sockfd, SSL_CTX *ssl_ctx) {
  if (sockfd != nullptr)
    BIO_free_all(sockfd);
  if (ssl_ctx != nullptr)
    SSL_CTX_free(ssl_ctx);
  exit(status);
}

static void
publish_callback([[maybe_unused]] void **unused,
                 [[maybe_unused]] struct mqtt_response_publish *published) {}

static void client_refresher(void *client) {
  while (true) {
    mqtt_sync((struct mqtt_client *)client);
    Sleep(100);
  }
}
