/*
 * Parse the config file simple.toml:
 *
 * [server]
 * host = "www.example.com"
 * port = [8080, 8181, 8282]
 *
 */

#include <inttypes.h>
#include <stdio.h>

#include "toml/toml.h"

static void error(const char *msg, const char *msg1) {
  fprintf(stderr, "ERROR: %s%s\n", msg, msg1 ? msg1 : "");
}

int main() {
  constexpr int kExitOk = 0;
  constexpr int kExitFail = 1;
  int rc = kExitFail;

  // Parse the toml file
  auto result = toml_parse_file_ex("simple.toml");

  // Check for parse error
  if (!result.ok) {
    error(result.errmsg, nullptr);
    toml_free(result);
    return rc;
  }

  // Extract values
  auto host = toml_seek(result.toptab, "server.host");
  auto port = toml_seek(result.toptab, "server.port");

  // Print server.host
  if (host.type != TOML_STRING) {
    error("missing or invalid 'server.host' property in config", nullptr);
    goto cleanup;
  }
  printf("server.host = %s\n", host.u.s);

  // Print server.port
  if (port.type != TOML_ARRAY) {
    error("missing or invalid 'server.port' property in config", nullptr);
    goto cleanup;
  }
  if (port.u.arr.size < 0) {
    error("invalid 'server.port' array size", nullptr);
    goto cleanup;
  }
  printf("server.port = [");
  for (int32_t i = 0; i < port.u.arr.size; i++) {
    auto elem = port.u.arr.elem[i];
    if (elem.type != TOML_INT64) {
      error("server.port element not an integer", nullptr);
      goto cleanup;
    }
    printf("%s%" PRId64, i ? ", " : "", elem.u.int64);
  }
  printf("]\n");

  // Done!
  rc = kExitOk;

cleanup:
  toml_free(result);
  return rc;
}
