// Copyright 2007 - 2021, Alan Antonuk and the rabbitmq-c contributors.
// SPDX-License-Identifier: mit

#pragma once

// Use OpenSSL v1.1.1 API.
#define OPENSSL_API_COMPAT 10101

#include <openssl/bio.h>

int amqp_openssl_bio_init();

void amqp_openssl_bio_destroy();

typedef const BIO_METHOD *BIO_METHOD_PTR;

BIO_METHOD_PTR amqp_openssl_bio();
