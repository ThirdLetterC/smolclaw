// Copyright 2007 - 2021, Alan Antonuk and the rabbitmq-c contributors.
// SPDX-License-Identifier: mit

#pragma once

#if defined(_WIN32) || defined(__CYGWIN__)
#if defined(AMQP_STATIC)
#define AMQP_EXPORT
#define AMQP_DEPRECATED
#elif defined(AMQP_EXPORTS)
#define AMQP_EXPORT __declspec(dllexport)
#define AMQP_DEPRECATED __declspec(deprecated)
#else
#define AMQP_EXPORT __declspec(dllimport)
#define AMQP_DEPRECATED __declspec(deprecated)
#endif
#else
#if defined(__GNUC__) || defined(__clang__)
#define AMQP_EXPORT __attribute__((visibility("default")))
#define AMQP_DEPRECATED __attribute__((deprecated))
#else
#define AMQP_EXPORT
#define AMQP_DEPRECATED
#endif
#endif

#define AMQP_DEPRECATED_EXPORT AMQP_EXPORT AMQP_DEPRECATED
