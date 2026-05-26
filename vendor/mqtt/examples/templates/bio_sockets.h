#pragma once

#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <stddef.h>
#include <stdio.h>
#include <time.h>

/*
    A template for opening a non-blocking BIO socket.
*/
BIO *open_nb_socket(const char *addr, const char *port) {
  BIO *bio = BIO_new_connect(addr);
  BIO_set_nbio(bio, 1);
  BIO_set_conn_port(bio, port);

  /* timeout after 10 seconds */
  int start_time = (int)time(nullptr);
  while (BIO_do_connect(bio) == 0 && (int)time(nullptr) - start_time < 10)
    ;

  if (BIO_do_connect(bio) <= 0) {
    fprintf(stderr, "Failed to open socket: BIO_do_connect returned <= 0\n");
    return nullptr;
  }

  return bio;
}
