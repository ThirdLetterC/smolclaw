
/**
 * @file
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "mqtt/mqtt.h"
#include "templates/mbedtls_sockets.h"

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
static void *client_refresher(void *client);

/**
 * @brief Safelty closes the \p sockfd and cancels the \p client_daemon before
 * \c exit.
 */
static void exit_example(int status, struct mbedtls_context *ctx,
                         pthread_t *client_daemon);

/**
 * A simple program to that publishes the current time whenever ENTER is
 * pressed.
 */
int main(int argc, const char *argv[]) {
  const char *addr;
  const char *port;
  const char *topic;
  const char *ca_file;

  struct mbedtls_context ctx;
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
  open_nb_socket(&ctx, addr, port, ca_file);
  mqtt_pal_socket_handle sockfd = &ctx.ssl_ctx;

  if (sockfd == nullptr) {
    exit_example(EXIT_FAILURE, &ctx, nullptr);
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
    exit_example(EXIT_FAILURE, &ctx, nullptr);
  }

  /* start a thread to refresh the client (handle egress and ingree client
   * traffic) */
  pthread_t client_daemon;
  if (pthread_create(&client_daemon, nullptr, client_refresher, &client)) {
    fprintf(stderr, "Failed to start client daemon.\n");
    exit_example(EXIT_FAILURE, &ctx, nullptr);
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
      exit_example(EXIT_FAILURE, &ctx, &client_daemon);
    }
  }

  /* disconnect */
  printf("\n%s disconnecting from %s\n", argv[0], addr);
  sleep(1);

  /* exit */
  exit_example(EXIT_SUCCESS, &ctx, &client_daemon);
}

static void exit_example(int status, struct mbedtls_context *ctx,
                         pthread_t *client_daemon) {
  if (client_daemon != nullptr)
    pthread_cancel(*client_daemon);
  if (ctx != nullptr) {
    mbedtls_ssl_free(&ctx->ssl_ctx);
    mbedtls_ssl_config_free(&ctx->ssl_conf);
    mbedtls_x509_crt_free(&ctx->ca_crt);
    mbedtls_ctr_drbg_free(&ctx->ctr_drbg);
    mbedtls_entropy_free(&ctx->entropy);
    mbedtls_net_free(&ctx->net_ctx);
  }
  exit(status);
}

static void
publish_callback([[maybe_unused]] void **unused,
                 [[maybe_unused]] struct mqtt_response_publish *published) {}

static void *client_refresher(void *client) {
  static const struct timespec refresh_delay = {.tv_sec = 0,
                                                .tv_nsec = 100'000'000L};
  while (true) {
    mqtt_sync((struct mqtt_client *)client);
    nanosleep(&refresh_delay, nullptr);
  }
  return nullptr;
}
