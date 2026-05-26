
/*
 * Copyright (c) 2019, Redis Labs
 *
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 *   * Redistributions of source code must retain the above copyright notice,
 *     this list of conditions and the following disclaimer.
 *   * Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 *   * Neither the name of Redis nor the names of its contributors may be used
 *     to endorse or promote products derived from this software without
 *     specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

#pragma once

#include "hiredis/hiredis.h"

/* Forward declaration from wolfSSL's OpenSSL compatibility layer.
 * ssl.h is not included here to keep build dependencies short.
 */
struct WOLFSSL;
typedef struct WOLFSSL SSL;

/* A wrapper around SSL_CTX to allow easy SSL use without directly
 * calling wolfSSL APIs.
 */
typedef struct redisSSLContext redisSSLContext;

/**
 * Initialization errors that redisCreateSSLContext() may return.
 */

typedef enum {
  REDIS_SSL_CTX_NONE = 0,                   /* No Error */
  REDIS_SSL_CTX_CREATE_FAILED,              /* Failed to create SSL_CTX */
  REDIS_SSL_CTX_CERT_KEY_REQUIRED,          /* Client cert and key must both be specified
                                               or skipped */
  REDIS_SSL_CTX_CA_CERT_LOAD_FAILED,        /* Failed to load CA Certificate or CA Path
                                             */
  REDIS_SSL_CTX_CLIENT_CERT_LOAD_FAILED,    /* Failed to load client certificate */
  REDIS_SSL_CTX_CLIENT_DEFAULT_CERT_FAILED, /* Failed to set client default
                                               certificate directory */
  REDIS_SSL_CTX_PRIVATE_KEY_LOAD_FAILED,    /* Failed to load private key */
  REDIS_SSL_CTX_OS_CERTSTORE_OPEN_FAILED,   /* Failed to open system certificate
                                               store */
  REDIS_SSL_CTX_OS_CERT_ADD_FAILED          /* Failed to add CA certificates obtained
                                               from system to the SSL context */
} redisSSLContextError;

/* Constants that mirror SSL verify modes. By default,
 * REDIS_SSL_VERIFY_PEER is used with redisCreateSSLContext().
 * Some Redis clients disable peer verification if there are no
 * certificates specified.
 */
[[maybe_unused]] static constexpr int REDIS_SSL_VERIFY_NONE = 0b0000;
[[maybe_unused]] static constexpr int REDIS_SSL_VERIFY_PEER = 0b0001;
[[maybe_unused]] static constexpr int REDIS_SSL_VERIFY_FAIL_IF_NO_PEER_CERT = 0b0010;
[[maybe_unused]] static constexpr int REDIS_SSL_VERIFY_CLIENT_ONCE = 0b0100;
[[maybe_unused]] static constexpr int REDIS_SSL_VERIFY_POST_HANDSHAKE = 0b1000;

/* Options to create an SSL context. */
typedef struct {
  const char *cacert_filename;
  const char *capath;
  const char *cert_filename;
  const char *private_key_filename;
  const char *server_name;
  int verify_mode;
} redisSSLOptions;

/**
 * Return the error message corresponding with the specified error code.
 */

const char *redisSSLContextGetError(redisSSLContextError error);

/**
 * Helper function to initialize the SSL library.
 *
 * This function name is preserved for API compatibility. It initializes the
 * configured TLS backend when required.
 */
int redisInitOpenSSL();

/**
 * Helper function to initialize an SSL context that can be used
 * to initiate SSL connections.
 *
 * cacert_filename is an optional name of a CA certificate/bundle file to load
 * and use for validation.
 *
 * capath is an optional directory path where trusted CA certificate files are
 * stored in a compatible hashed certificate directory structure.
 *
 * cert_filename and private_key_filename are optional names of a client side
 * certificate and private key files to use for authentication. They need to
 * be both specified or omitted.
 *
 * server_name is an optional and will be used as a server name indication
 * (SNI) TLS extension.
 *
 * If error is non-null, it will be populated in case the context creation fails
 * (returning a nullptr).
 */

[[nodiscard]] redisSSLContext *redisCreateSSLContext(const char *cacert_filename,
                                                     const char *capath, const char *cert_filename,
                                                     const char *private_key_filename,
                                                     const char *server_name,
                                                     redisSSLContextError *error);

/**
 * Helper function to initialize an SSL context that can be used
 * to initiate SSL connections. This is a more extensible version of
 * redisCreateSSLContext().
 *
 * options contains a structure of SSL options to use.
 *
 * If error is non-null, it will be populated in case the context creation fails
 * (returning a nullptr).
 */
[[nodiscard]] redisSSLContext *redisCreateSSLContextWithOptions(const redisSSLOptions *options,
                                                                redisSSLContextError *error);

/**
 * Free a previously created SSL context.
 */
void redisFreeSSLContext(redisSSLContext *redis_ssl_ctx);

/**
 * Initiate SSL on an existing redisContext.
 *
 * This is similar to redisInitiateSSL() but does not require the caller
 * to directly interact with the TLS library, and instead uses a redisSSLContext
 * previously created using redisCreateSSLContext().
 */

int redisInitiateSSLWithContext(redisContext *c, redisSSLContext *redis_ssl_ctx);

/**
 * Initiate SSL/TLS negotiation on a provided SSL object.
 */

int redisInitiateSSL(redisContext *c, SSL *ssl);
