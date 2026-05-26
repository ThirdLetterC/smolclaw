/*
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2011, Pieter Noordhuis <pcnoordhuis at gmail dot com>
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

#include <stddef.h> /* for size_t */

[[maybe_unused]] static constexpr int REDIS_ERR = -1;
[[maybe_unused]] static constexpr int REDIS_OK = 0;

/* When an error occurs, the err flag in a context is set to hold the type of
 * error that occurred. REDIS_ERR_IO means there was an I/O error and you
 * should use the "errno" variable to find out what is wrong.
 * For other values, the "errstr" field will hold a description. */
[[maybe_unused]] static constexpr int REDIS_ERR_IO = 1;       /* Error in read or write */
[[maybe_unused]] static constexpr int REDIS_ERR_EOF = 3;      /* End of file */
[[maybe_unused]] static constexpr int REDIS_ERR_PROTOCOL = 4; /* Protocol error */
[[maybe_unused]] static constexpr int REDIS_ERR_OOM = 5;      /* Out of memory */
[[maybe_unused]] static constexpr int REDIS_ERR_TIMEOUT = 6;  /* Timed out */
[[maybe_unused]] static constexpr int REDIS_ERR_OTHER = 2;    /* Everything else... */

[[maybe_unused]] static constexpr int REDIS_REPLY_STRING = 1;
[[maybe_unused]] static constexpr int REDIS_REPLY_ARRAY = 2;
[[maybe_unused]] static constexpr int REDIS_REPLY_INTEGER = 3;
[[maybe_unused]] static constexpr int REDIS_REPLY_NIL = 4;
[[maybe_unused]] static constexpr int REDIS_REPLY_STATUS = 5;
[[maybe_unused]] static constexpr int REDIS_REPLY_ERROR = 6;
[[maybe_unused]] static constexpr int REDIS_REPLY_DOUBLE = 7;
[[maybe_unused]] static constexpr int REDIS_REPLY_BOOL = 8;
[[maybe_unused]] static constexpr int REDIS_REPLY_MAP = 9;
[[maybe_unused]] static constexpr int REDIS_REPLY_SET = 10;
[[maybe_unused]] static constexpr int REDIS_REPLY_ATTR = 11;
[[maybe_unused]] static constexpr int REDIS_REPLY_PUSH = 12;
[[maybe_unused]] static constexpr int REDIS_REPLY_BIGNUM = 13;
[[maybe_unused]] static constexpr int REDIS_REPLY_VERB = 14;

/* Default max unused reader buffer. */
[[maybe_unused]] static constexpr size_t REDIS_READER_MAX_BUF = 16'384;

/* Default multi-bulk element limit */
[[maybe_unused]] static constexpr long long REDIS_READER_MAX_ARRAY_ELEMENTS = (1LL << 32) - 1;

typedef struct redisReadTask {
  int type;
  long long elements;           /* number of elements in multibulk container */
  int idx;                      /* index in parent (array) object */
  void *obj;                    /* holds user-generated value for a read task */
  struct redisReadTask *parent; /* parent task */
  void *privdata;               /* user-settable arbitrary field */
} redisReadTask;

typedef struct redisReplyObjectFunctions {
  void *(*createString)(const redisReadTask *, char *, size_t);
  void *(*createArray)(const redisReadTask *, size_t);
  void *(*createInteger)(const redisReadTask *, long long);
  void *(*createDouble)(const redisReadTask *, double, char *, size_t);
  void *(*createNil)(const redisReadTask *);
  void *(*createBool)(const redisReadTask *, int);
  void (*freeObject)(void *);
} redisReplyObjectFunctions;

typedef struct redisReader {
  int err;          /* Error flags, 0 when there is no error */
  char errstr[128]; /* String representation of error when applicable */

  char *buf;             /* Read buffer */
  size_t pos;            /* Buffer cursor */
  size_t len;            /* Buffer length */
  size_t maxbuf;         /* Max length of unused buffer */
  long long maxelements; /* Max multi-bulk elements */

  redisReadTask **task;
  int tasks;

  int ridx;    /* Index of current read task */
  void *reply; /* Temporary reply pointer */

  redisReplyObjectFunctions *fn;
  void *privdata;
} redisReader;

/* Public API for the protocol parser. */
[[nodiscard]] redisReader *redisReaderCreateWithFunctions(redisReplyObjectFunctions *fn);
void redisReaderFree(redisReader *r);
int redisReaderFeed(redisReader *r, const char *buf, size_t len);
int redisReaderGetReply(redisReader *r, void **reply);

#define redisReaderSetPrivdata(_r, _p) (int)(((redisReader *)(_r))->privdata = (_p))
#define redisReaderGetObject(_r) (((redisReader *)(_r))->reply)
#define redisReaderGetError(_r) (((redisReader *)(_r))->errstr)
