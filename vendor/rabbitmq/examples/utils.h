// Copyright 2007 - 2021, Alan Antonuk and the rabbitmq-c contributors.
// SPDX-License-Identifier: mit

#pragma once

#include <stddef.h>
#include <stdint.h>

#include "rabbitmq/amqp.h"

void die(const char *fmt, ...);
extern void die_on_error(int x, char const *context);
extern void die_on_amqp_error(amqp_rpc_reply_t x, char const *context);

extern void amqp_dump(void const *buffer, size_t len);

extern uint64_t now_microseconds();
extern void microsleep(int usec);
extern int parse_int_arg(const char *value, const char *name, int min_value,
                         int max_value);
