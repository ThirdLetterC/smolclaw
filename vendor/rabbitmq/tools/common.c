// Copyright 2007 - 2021, Alan Antonuk and the rabbitmq-c contributors.
// SPDX-License-Identifier: mit

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdckdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "common.h"

#ifdef WITH_SSL
#include "rabbitmq/ssl_socket.h"
#endif

#include "rabbitmq/tcp_socket.h"

/* For when reading auth data from a file */
static constexpr size_t max_auth_token_len = 128;
static constexpr char username_prefix[] = "username:";
static constexpr char password_prefix[] = "password:";

static char *amqp_url;
static char *amqp_server;
static int amqp_port = -1;
static char *amqp_vhost;
static char *amqp_username;
static char *amqp_password;
static int amqp_heartbeat = 0;
static char *amqp_authfile;
#ifdef WITH_SSL
static int amqp_ssl = 0;
static char *amqp_cacert = nullptr;
static char *amqp_key = nullptr;
static char *amqp_cert = nullptr;
#endif /* WITH_SSL */

static char *amqp_url_storage = nullptr;
static bool amqp_url_storage_owned = false;
static char *amqp_server_host = nullptr;
static bool amqp_server_host_owned = false;
static bool amqp_username_owned = false;
static bool amqp_password_owned = false;
static bool cleanup_registered = false;

static void secure_clear(void *ptr, size_t len) {
  volatile unsigned char *bytes = ptr;

  if (ptr == nullptr) {
    return;
  }

  while (len > 0) {
    *bytes++ = 0;
    --len;
  }
}

static void release_owned_string(char **value, bool *owned, bool scrub) {
  if (*owned && *value != nullptr) {
    if (scrub) {
      secure_clear(*value, strlen(*value));
    }
    free(*value);
  }

  *value = nullptr;
  *owned = false;
}

static void cleanup_auth_credentials() {
  release_owned_string(&amqp_username, &amqp_username_owned, true);
  release_owned_string(&amqp_password, &amqp_password_owned, true);
}

static void cleanup_connection_inputs() {
  cleanup_auth_credentials();
  release_owned_string(&amqp_url_storage, &amqp_url_storage_owned, true);
  release_owned_string(&amqp_server_host, &amqp_server_host_owned, false);
}

static void register_cleanup_once() {
  if (!cleanup_registered) {
    if (atexit(cleanup_connection_inputs) != 0) {
      die("Could not register credential cleanup");
    }
    cleanup_registered = true;
  }
}

static void enforce_authfile_permissions(int fd, const char *path) {
  struct stat st;

  if (fstat(fd, &st) != 0) {
    auto err = errno;
    close(fd);
    die_errno(err, "Could not inspect auth data file %s", path);
  }

  if (!S_ISREG(st.st_mode)) {
    close(fd);
    die("Auth data file %s is not a regular file", path);
  }

  if ((st.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
    close(fd);
    die("Auth data file %s must not be accessible by group or other users",
        path);
  }
}

static FILE *open_authfile(const char *path) {
  FILE *fp;
  int fd;
  int flags = O_RDONLY;

#ifdef O_CLOEXEC
  flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
  flags |= O_NOFOLLOW;
#endif

  fd = open(path, flags);
  if (fd < 0) {
    die_errno(errno, "Could not read auth data file %s", path);
  }

  enforce_authfile_permissions(fd, path);

  fp = fdopen(fd, "r");
  if (fp == nullptr) {
    auto err = errno;
    close(fd);
    die_errno(err, "Could not read auth data file %s", path);
  }

  return fp;
}

static char *allocate_auth_value() {
  auto value = (char *)calloc(max_auth_token_len, sizeof(char));
  if (value == nullptr) {
    die("Out of memory");
  }
  return value;
}

static void parse_auth_line(const char *field_name, const char *prefix,
                            char *destination, char *token) {
  auto prefix_len = strlen(prefix);
  auto value = token + prefix_len;
  size_t value_len;

  if (strncmp(token, prefix, prefix_len) != 0) {
    die("Malformed auth file (missing %s)", field_name);
  }

  value_len = strlen(value);
  if (value_len == 0 || value[value_len - 1] != '\n') {
    die("%s too long", field_name);
  }

  if (value_len > max_auth_token_len) {
    die("%s too long", field_name);
  }

  if (value_len > 1) {
    memcpy(destination, value, value_len - 1);
  }
  destination[value_len - 1] = '\0';
}

void die(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, "\n");
  exit(1);
}

void die_errno(int err, const char *fmt, ...) {
  va_list ap;

  if (err == 0) {
    return;
  }

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, ": %s\n", strerror(err));
  exit(1);
}

void die_amqp_error(int err, const char *fmt, ...) {
  va_list ap;

  if (err >= 0) {
    return;
  }

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, ": %s\n", amqp_error_string2(err));
  exit(1);
}

const char *amqp_server_exception_string(amqp_rpc_reply_t r) {
  int res;
  static char s[512];

  switch (r.reply.id) {
    case AMQP_CONNECTION_CLOSE_METHOD: {
      amqp_connection_close_t *m = (amqp_connection_close_t *)r.reply.decoded;
      res = snprintf(s, sizeof(s), "server connection error %d, message: %.*s",
                     m->reply_code, (int)m->reply_text.len,
                     (char *)m->reply_text.bytes);
      break;
    }

    case AMQP_CHANNEL_CLOSE_METHOD: {
      amqp_channel_close_t *m = (amqp_channel_close_t *)r.reply.decoded;
      res = snprintf(s, sizeof(s), "server channel error %d, message: %.*s",
                     m->reply_code, (int)m->reply_text.len,
                     (char *)m->reply_text.bytes);
      break;
    }

    default:
      res = snprintf(s, sizeof(s), "unknown server error, method id 0x%08X",
                     r.reply.id);
      break;
  }

  return res >= 0 ? s : nullptr;
}

const char *amqp_rpc_reply_string(amqp_rpc_reply_t r) {
  switch (r.reply_type) {
    case AMQP_RESPONSE_NORMAL:
      return "normal response";

    case AMQP_RESPONSE_NONE:
      return "missing RPC reply type";

    case AMQP_RESPONSE_LIBRARY_EXCEPTION:
      return amqp_error_string2(r.library_error);

    case AMQP_RESPONSE_SERVER_EXCEPTION:
      return amqp_server_exception_string(r);

    default:
      abort();
  }
}

void die_rpc(amqp_rpc_reply_t r, const char *fmt, ...) {
  va_list ap;

  if (r.reply_type == AMQP_RESPONSE_NORMAL) {
    return;
  }

  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fprintf(stderr, ": %s\n", amqp_rpc_reply_string(r));
  exit(1);
}

const char *connect_options_title = "Connection options";
struct poptOption connect_options[] = {
    {"url", 'u', POPT_ARG_STRING, &amqp_url, 0, "the AMQP URL to connect to",
     "amqp://..."},
    {"server", 's', POPT_ARG_STRING, &amqp_server, 0,
     "the AMQP server to connect to", "hostname"},
    {"port", 0, POPT_ARG_INT, &amqp_port, 0, "the port to connect on", "port"},
    {"vhost", 0, POPT_ARG_STRING, &amqp_vhost, 0,
     "the vhost to use when connecting", "vhost"},
    {"username", 0, POPT_ARG_STRING, &amqp_username, 0,
     "the username to login with", "username"},
    {"password", 0, POPT_ARG_STRING, &amqp_password, 0,
     "the password to login with", "password"},
    {"heartbeat", 0, POPT_ARG_INT, &amqp_heartbeat, 0,
     "heartbeat interval, set to 0 to disable", "heartbeat"},
    {"authfile", 0, POPT_ARG_STRING, &amqp_authfile, 0,
     "path to file containing username/password for authentication", "file"},
#ifdef WITH_SSL
    {"ssl", 0, POPT_ARG_NONE, &amqp_ssl, 0, "connect over SSL/TLS", nullptr},
    {"cacert", 0, POPT_ARG_STRING, &amqp_cacert, 0,
     "path to the CA certificate file", "cacert.pem"},
    {"key", 0, POPT_ARG_STRING, &amqp_key, 0,
     "path to the client private key file", "key.pem"},
    {"cert", 0, POPT_ARG_STRING, &amqp_cert, 0,
     "path to the client certificate file", "cert.pem"},
#endif /* WITH_SSL */
    {nullptr, '\0', 0, nullptr, 0, nullptr, nullptr}};

void read_authfile(const char *path) {
  FILE *fp = nullptr;
  char token[max_auth_token_len];

  register_cleanup_once();
  cleanup_auth_credentials();

  amqp_username = allocate_auth_value();
  amqp_password = allocate_auth_value();
  amqp_username_owned = true;
  amqp_password_owned = true;
  fp = open_authfile(path);

  if (fgets(token, (int)sizeof(token), fp) == nullptr) {
    fclose(fp);
    die("Malformed auth file (missing username)");
  }
  parse_auth_line("username", username_prefix, amqp_username, token);

  if (fgets(token, (int)sizeof(token), fp) == nullptr) {
    fclose(fp);
    die("Malformed auth file (missing password)");
  }
  parse_auth_line("password", password_prefix, amqp_password, token);

  (void)fgetc(fp);
  if (ferror(fp) != 0) {
    auto err = errno;
    fclose(fp);
    die_errno(err, "Could not read auth data file %s", path);
  }
  if (!feof(fp)) {
    fclose(fp);
    die("Malformed auth file (trailing data)");
  }
  fclose(fp);
}

static void init_connection_info(struct amqp_connection_info *ci) {
  register_cleanup_once();

  ci->user = nullptr;
  ci->password = nullptr;
  ci->host = nullptr;
  ci->port = -1;
  ci->vhost = nullptr;
  ci->user = nullptr;

  amqp_default_connection_info(ci);

  if (amqp_url) {
    cleanup_connection_inputs();

    amqp_url_storage = strdup(amqp_url);
    if (amqp_url_storage == nullptr) {
      die("Out of memory");
    }
    amqp_url_storage_owned = true;
    die_amqp_error(amqp_parse_url(amqp_url_storage, ci), "Parsing URL '%s'",
                   amqp_url);
  }

  if (amqp_server) {
    char *colon;
    if (amqp_url) {
      die("--server and --url options cannot be used at the same time");
    }

    /* parse the server string into a hostname and a port */
    colon = strchr(amqp_server, ':');
    if (colon) {
      char *port_end;
      size_t host_len;

      /* Deprecate specifying the port number with the
         --server option, because it is not ipv6 friendly.
         --url now allows connection options to be
         specified concisely. */
      fprintf(stderr,
              "Specifying the port number with --server is deprecated\n");

      host_len = colon - amqp_server;
      size_t host_size;
      if (ckd_add(&host_size, host_len, (size_t)1)) {
        die("server host is too long");
      }

      release_owned_string(&amqp_server_host, &amqp_server_host_owned, false);
      amqp_server_host = (char *)calloc(host_size, sizeof(char));
      if (amqp_server_host == nullptr) {
        die("Out of memory");
      }
      amqp_server_host_owned = true;
      memcpy(amqp_server_host, amqp_server, host_len);
      amqp_server_host[host_len] = 0;
      ci->host = amqp_server_host;

      if (amqp_port >= 0) {
        die("both --server and --port options specify server port");
      }

      ci->port = strtol(colon + 1, &port_end, 10);
      if (ci->port < 0 || ci->port > 65535 || port_end == colon + 1 ||
          *port_end != 0)
        die("bad server port number in '%s'", amqp_server);
    } else {
      ci->host = amqp_server;
      ci->port = 5672;
#if WITH_SSL
      if (amqp_ssl) {
        ci->port = 5671;
      }
#endif
    }
  }

#if WITH_SSL
  if (amqp_ssl && !ci->ssl) {
    if (amqp_url) {
      die("the --ssl option specifies an SSL connection"
          " but the --url option does not");
    } else {
      ci->ssl = 1;
    }
  }
#endif

  if (amqp_port >= 0) {
    if (amqp_url) {
      die("--port and --url options cannot be used at the same time");
    }

    ci->port = amqp_port;
  }

  if (amqp_username) {
    if (amqp_url) {
      die("--username and --url options cannot be used at the same time");
    } else if (amqp_authfile) {
      die("--username and --authfile options cannot be used at the same time");
    }

    ci->user = amqp_username;
  }

  if (amqp_password) {
    if (amqp_url) {
      die("--password and --url options cannot be used at the same time");
    } else if (amqp_authfile) {
      die("--password and --authfile options cannot be used at the same time");
    }

    ci->password = amqp_password;
  }

  if (amqp_authfile) {
    if (amqp_url) {
      die("--authfile and --url options cannot be used at the same time");
    }

    read_authfile(amqp_authfile);
    ci->user = amqp_username;
    ci->password = amqp_password;
  }

  if (amqp_vhost) {
    if (amqp_url) {
      die("--vhost and --url options cannot be used at the same time");
    }

    ci->vhost = amqp_vhost;
  }

  if (amqp_heartbeat < 0) {
    die("--heartbeat must be a positive value");
  }

#ifdef WITH_SSL
  if (ci->ssl) {
    if ((amqp_key == nullptr) != (amqp_cert == nullptr)) {
      die("--key and --cert must be provided together");
    }
  } else if (amqp_key != nullptr || amqp_cert != nullptr ||
             amqp_cacert != nullptr) {
    die("TLS options require an SSL/TLS connection");
  }
#endif
}

amqp_connection_state_t make_connection() {
  int status;
  amqp_socket_t *socket = nullptr;
  struct amqp_connection_info ci;
  amqp_connection_state_t conn;

  init_connection_info(&ci);
  conn = amqp_new_connection();
  if (ci.ssl) {
#ifdef WITH_SSL
    socket = amqp_ssl_socket_new(conn);
    if (!socket) {
      die("creating SSL/TLS socket");
    }
    if (amqp_cacert) {
      die_amqp_error(amqp_ssl_socket_set_cacert(socket, amqp_cacert),
                     "loading CA certificate %s", amqp_cacert);
    } else {
      die_amqp_error(amqp_ssl_socket_enable_default_verify_paths(socket),
                     "loading system CA certificates");
    }
    if (amqp_key) {
      die_amqp_error(amqp_ssl_socket_set_key(socket, amqp_cert, amqp_key),
                     "loading client certificate/key");
    }
#else
    die("librabbitmq was not built with SSL/TLS support");
#endif
  } else {
    socket = amqp_tcp_socket_new(conn);
    if (!socket) {
      die("creating TCP socket (out of memory)");
    }
  }
  status = amqp_socket_open(socket, ci.host, ci.port);
  if (status) {
    die_amqp_error(status, "opening socket to %s:%d", ci.host, ci.port);
  }
  die_rpc(amqp_login(conn, ci.vhost, 0, 131072, amqp_heartbeat,
                     AMQP_SASL_METHOD_PLAIN, ci.user, ci.password),
          "logging in to AMQP server");
  if (!amqp_channel_open(conn, 1)) {
    die_rpc(amqp_get_rpc_reply(conn), "opening channel");
  }
  cleanup_connection_inputs();
  return conn;
}

void close_connection(amqp_connection_state_t conn) {
  int res;
  die_rpc(amqp_channel_close(conn, 1, AMQP_REPLY_SUCCESS), "closing channel");
  die_rpc(amqp_connection_close(conn, AMQP_REPLY_SUCCESS),
          "closing connection");

  res = amqp_destroy_connection(conn);
  die_amqp_error(res, "closing connection");
}

amqp_bytes_t read_all(int fd) {
  size_t space = 4096;
  amqp_bytes_t bytes;

  bytes.bytes = calloc(space, sizeof(char));
  if (bytes.bytes == nullptr) {
    die("Out of memory");
  }
  bytes.len = 0;

  for (;;) {
    ssize_t res = read(fd, (char *)bytes.bytes + bytes.len, space - bytes.len);
    if (res == 0) {
      break;
    }

    if (res < 0) {
      if (errno == EINTR) {
        continue;
      }

      die_errno(errno, "reading");
    }

    bytes.len += res;
    if (bytes.len == space) {
      size_t new_space;
      if (ckd_mul(&new_space, space, (size_t)2)) {
        free(bytes.bytes);
        die("input is too large");
      }

      void *new_bytes = realloc(bytes.bytes, new_space);
      if (new_bytes == nullptr) {
        free(bytes.bytes);
        die("Out of memory");
      }

      bytes.bytes = new_bytes;
      space = new_space;
    }
  }

  return bytes;
}

void write_all(int fd, amqp_bytes_t data) {
  while (data.len > 0) {
    ssize_t res = write(fd, data.bytes, data.len);
    if (res < 0) {
      if (errno == EINTR) {
        continue;
      }
      die_errno(errno, "write");
    }
    if (res == 0) {
      die("write returned zero bytes");
    }

    data.len -= res;
    data.bytes = (char *)data.bytes + res;
  }
}

void copy_body(amqp_connection_state_t conn, int fd) {
  size_t body_remaining;
  amqp_frame_t frame;

  int res = amqp_simple_wait_frame(conn, &frame);
  die_amqp_error(res, "waiting for header frame");
  if (frame.frame_type != AMQP_FRAME_HEADER) {
    die("expected header, got frame type 0x%X", frame.frame_type);
  }

  body_remaining = frame.payload.properties.body_size;
  while (body_remaining) {
    res = amqp_simple_wait_frame(conn, &frame);
    die_amqp_error(res, "waiting for body frame");
    if (frame.frame_type != AMQP_FRAME_BODY) {
      die("expected body, got frame type 0x%X", frame.frame_type);
    }

    write_all(fd, frame.payload.body_fragment);
    body_remaining -= frame.payload.body_fragment.len;
  }
}

poptContext process_options(int argc, const char **argv,
                            struct poptOption *options, const char *help) {
  int c;
  poptContext opts = poptGetContext(nullptr, argc, argv, options, 0);
  poptSetOtherOptionHelp(opts, help);

  while ((c = poptGetNextOpt(opts)) >= 0) {
    /* no options require explicit handling */
  }

  if (c < -1) {
    fprintf(stderr, "%s: %s\n", poptBadOption(opts, POPT_BADOPTION_NOALIAS),
            poptStrerror(c));
    poptPrintUsage(opts, stderr, 0);
    exit(1);
  }

  return opts;
}

void process_all_options(int argc, const char **argv,
                         struct poptOption *options) {
  poptContext opts = process_options(argc, argv, options, "[OPTIONS]...");
  const char *opt = poptPeekArg(opts);

  if (opt) {
    fprintf(stderr, "unexpected operand: %s\n", opt);
    poptPrintUsage(opts, stderr, 0);
    exit(1);
  }

  poptFreeContext(opts);
}

amqp_bytes_t cstring_bytes(const char *str) {
  return str ? amqp_cstring_bytes(str) : amqp_empty_bytes;
}
