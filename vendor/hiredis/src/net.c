/* Extracted from anet.c to work properly with Hiredis error reporting.
 *
 * Copyright (c) 2009-2011, Salvatore Sanfilippo <antirez at gmail dot com>
 * Copyright (c) 2010-2014, Pieter Noordhuis <pcnoordhuis at gmail dot com>
 * Copyright (c) 2015, Matt Stancliff <matt at genges dot com>,
 *                     Jan-Erik Rediger <janerik at fnordig dot com>
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

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/poll.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#include "hiredis/alloc.h"
#include "hiredis/net.h"

/* Defined in hiredis.c */
void __redisSetError(redisContext *c, int type, const char *str);

int redisContextUpdateCommandTimeout(redisContext *c,
                                     const struct timeval *timeout);

void redisNetClose(redisContext *c) {
  if (c && c->fd != REDIS_INVALID_FD) {
    close(c->fd);
    c->fd = REDIS_INVALID_FD;
  }
}

ssize_t redisNetRead(redisContext *c, char *buf, size_t bufcap) {
  auto nread = recv(c->fd, buf, bufcap, 0);
  if (nread == -1) {
    if ((errno == EWOULDBLOCK && !(c->flags & REDIS_BLOCK)) ||
        (errno == EINTR)) {
      /* Try again later */
      return 0;
    } else if (errno == ETIMEDOUT && (c->flags & REDIS_BLOCK)) {
      __redisSetError(c, REDIS_ERR_TIMEOUT, "recv timeout");
      return -1;
    } else {
      __redisSetError(c, REDIS_ERR_IO, strerror(errno));
      return -1;
    }
  } else if (nread == 0) {
    __redisSetError(c, REDIS_ERR_EOF, "Server closed the connection");
    return -1;
  } else {
    return nread;
  }
}

ssize_t redisNetWrite(redisContext *c) {
  auto nwritten = send(c->fd, c->obuf, sdslen(c->obuf), 0);
  if (nwritten < 0) {
    if ((errno == EWOULDBLOCK && !(c->flags & REDIS_BLOCK)) ||
        (errno == EINTR)) {
      /* Try again */
      return 0;
    } else {
      __redisSetError(c, REDIS_ERR_IO, strerror(errno));
      return -1;
    }
  }

  return nwritten;
}

static void __redisSetErrorFromErrno(redisContext *c, int type,
                                     const char *prefix) {
  int errorno = errno; /* snprintf() may change errno */
  constexpr size_t error_buf_size = 128;
  char buf[error_buf_size] = {0};
  size_t len = 0;

  if (prefix != nullptr) {
    auto written = snprintf(buf, sizeof(buf), "%s: ", prefix);
    if (written > 0) {
      len = (size_t)written;
      if (len >= sizeof(buf))
        len = sizeof(buf) - 1;
    }
  }
  {
    const char *errstr = strerror(errorno);
    if (errstr == nullptr)
      errstr = "Unknown error";
    (void)snprintf((char *)(buf + len), sizeof(buf) - len, "%s", errstr);
  }
  __redisSetError(c, type, buf);
}

static int redisSetReuseAddr(redisContext *c) {
  int on = 1;
  if (setsockopt(c->fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on)) == -1) {
    __redisSetErrorFromErrno(c, REDIS_ERR_IO, nullptr);
    redisNetClose(c);
    return REDIS_ERR;
  }
  return REDIS_OK;
}

static int redisCreateSocket(redisContext *c, int type) {
  redisFD s;
  int flags = SOCK_STREAM;

#ifdef SOCK_CLOEXEC
  if (c->flags & REDIS_OPT_SET_SOCK_CLOEXEC) {
    flags |= SOCK_CLOEXEC;
  }
#endif

  if ((s = socket(type, flags, 0)) == REDIS_INVALID_FD) {
    __redisSetErrorFromErrno(c, REDIS_ERR_IO, nullptr);
    return REDIS_ERR;
  }
  c->fd = s;

  if (type == AF_INET) {
    if (redisSetReuseAddr(c) == REDIS_ERR) {
      return REDIS_ERR;
    }
  }
  return REDIS_OK;
}

static int redisSetBlocking(redisContext *c, int blocking) {
  int flags;

  /* Set the socket nonblocking.
   * Note that fcntl(2) for F_GETFL and F_SETFL can't be
   * interrupted by a signal. */
  if ((flags = fcntl(c->fd, F_GETFL)) == -1) {
    __redisSetErrorFromErrno(c, REDIS_ERR_IO, "fcntl(F_GETFL)");
    redisNetClose(c);
    return REDIS_ERR;
  }

  if (blocking)
    flags &= ~O_NONBLOCK;
  else
    flags |= O_NONBLOCK;

  if (fcntl(c->fd, F_SETFL, flags) == -1) {
    __redisSetErrorFromErrno(c, REDIS_ERR_IO, "fcntl(F_SETFL)");
    redisNetClose(c);
    return REDIS_ERR;
  }
  return REDIS_OK;
}

int redisKeepAlive(redisContext *c, int interval) {
  int val = 1;
  redisFD fd = c->fd;

  /* TCP_KEEPALIVE makes no sense with AF_UNIX connections */
  if (c->connection_type == REDIS_CONN_UNIX)
    return REDIS_ERR;

  if (setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &val, sizeof(val)) == -1) {
    __redisSetError(c, REDIS_ERR_OTHER, strerror(errno));
    return REDIS_ERR;
  }

  val = interval;

#if defined(__APPLE__) && defined(__MACH__)
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPALIVE, &val, sizeof(val)) < 0) {
    __redisSetError(c, REDIS_ERR_OTHER, strerror(errno));
    return REDIS_ERR;
  }
#else
#if defined(__GLIBC__) && !defined(__FreeBSD_kernel__)
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &val, sizeof(val)) < 0) {
    __redisSetError(c, REDIS_ERR_OTHER, strerror(errno));
    return REDIS_ERR;
  }

  val = interval / 3;
  if (val == 0)
    val = 1;
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &val, sizeof(val)) < 0) {
    __redisSetError(c, REDIS_ERR_OTHER, strerror(errno));
    return REDIS_ERR;
  }

  val = 3;
  if (setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &val, sizeof(val)) < 0) {
    __redisSetError(c, REDIS_ERR_OTHER, strerror(errno));
    return REDIS_ERR;
  }
#endif
#endif
  return REDIS_OK;
}

int redisSetTcpNoDelay(redisContext *c) {
  int yes = 1;
  if (setsockopt(c->fd, IPPROTO_TCP, TCP_NODELAY, &yes, sizeof(yes)) == -1) {
    __redisSetErrorFromErrno(c, REDIS_ERR_IO, "setsockopt(TCP_NODELAY)");
    redisNetClose(c);
    return REDIS_ERR;
  }
  return REDIS_OK;
}

int redisContextSetTcpUserTimeout(redisContext *c,
                                  [[maybe_unused]] unsigned int timeout) {
  int res;
#ifdef TCP_USER_TIMEOUT
  res = setsockopt(c->fd, IPPROTO_TCP, TCP_USER_TIMEOUT, &timeout,
                   sizeof(timeout));
#else
  res = -1;
  errno = ENOTSUP;
#endif
  if (res == -1) {
    __redisSetErrorFromErrno(c, REDIS_ERR_IO, "setsockopt(TCP_USER_TIMEOUT)");
    redisNetClose(c);
    return REDIS_ERR;
  }
  return REDIS_OK;
}

static constexpr long MAX_MSEC = (LONG_MAX - 999L) / 1'000L;

static int redisContextTimeoutMsec(redisContext *c, long *result) {
  const struct timeval *timeout = c->connect_timeout;
  long msec = -1;
  constexpr long microseconds_per_second = 1'000'000;
  constexpr long milliseconds_per_second = 1'000;
  constexpr long microseconds_per_millisecond = 1'000;
  constexpr long round_up_usec = 999;

  /* Only use timeout when not nullptr. */
  if (timeout != nullptr) {
    if (timeout->tv_usec < 0 || timeout->tv_usec >= microseconds_per_second ||
        timeout->tv_sec < 0 || timeout->tv_sec > MAX_MSEC) {
      __redisSetError(c, REDIS_ERR_IO, "Invalid timeout specified");
      *result = msec;
      return REDIS_ERR;
    }

    msec = (timeout->tv_sec * milliseconds_per_second) +
           ((timeout->tv_usec + round_up_usec) / microseconds_per_millisecond);

    if (msec < 0 || msec > INT_MAX) {
      msec = INT_MAX;
    }
  }

  *result = msec;
  return REDIS_OK;
}

static long redisPollMillis() {
  struct timespec now = {0};
  if (clock_gettime(CLOCK_MONOTONIC, &now) != 0)
    return 0;

  constexpr long milliseconds_per_second = 1'000;
  constexpr long nanoseconds_per_millisecond = 1'000'000;
  if (now.tv_sec >
      (LONG_MAX - (nanoseconds_per_millisecond - 1)) / milliseconds_per_second)
    return LONG_MAX;

  return (now.tv_sec * milliseconds_per_second) +
         now.tv_nsec / nanoseconds_per_millisecond;
}

static int redisContextWaitReady(redisContext *c, long msec) {
  struct pollfd wfd;
  long end;
  int res;

  if (errno != EINPROGRESS) {
    __redisSetErrorFromErrno(c, REDIS_ERR_IO, nullptr);
    redisNetClose(c);
    return REDIS_ERR;
  }

  wfd.fd = c->fd;
  wfd.events = POLLOUT;
  if (msec >= 0) {
    auto now = redisPollMillis();
    if (now > LONG_MAX - msec) {
      end = LONG_MAX;
    } else {
      end = now + msec;
    }
  } else {
    end = 0;
  }

  while ((res = poll(&wfd, 1, msec)) <= 0) {
    if (res < 0 && errno != EINTR) {
      __redisSetErrorFromErrno(c, REDIS_ERR_IO, "poll(2)");
      redisNetClose(c);
      return REDIS_ERR;
    } else if (res == 0 || (msec >= 0 && redisPollMillis() >= end)) {
      errno = ETIMEDOUT;
      __redisSetErrorFromErrno(c, REDIS_ERR_IO, nullptr);
      redisNetClose(c);
      return REDIS_ERR;
    } else {
      /* res < 0 && errno == EINTR, try again */
    }
  }

  if (redisCheckConnectDone(c, &res) != REDIS_OK || res == 0) {
    redisCheckSocketError(c);
    return REDIS_ERR;
  }

  return REDIS_OK;
}

int redisCheckConnectDone(redisContext *c, int *completed) {
  auto rc = connect(c->fd, (const struct sockaddr *)c->saddr, c->addrlen);
  if (rc == 0) {
    *completed = 1;
    return REDIS_OK;
  }
  auto error = errno;
  if (error == EINPROGRESS) {
    /* must check error to see if connect failed.  Get the socket error */
    int fail, so_error;
    socklen_t optlen = sizeof(so_error);
    fail = getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &so_error, &optlen);
    if (fail == 0) {
      if (so_error == 0) {
        /* Socket is connected! */
        *completed = 1;
        return REDIS_OK;
      }
      /* connection error; */
      errno = so_error;
      error = so_error;
    }
  }
  switch (error) {
  case EISCONN:
    *completed = 1;
    return REDIS_OK;
  case EALREADY:
  case EWOULDBLOCK:
    *completed = 0;
    return REDIS_OK;
  default:
    return REDIS_ERR;
  }
}

int redisCheckSocketError(redisContext *c) {
  int err = 0, errno_saved = errno;
  socklen_t errlen = sizeof(err);

  if (getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &errlen) == -1) {
    __redisSetErrorFromErrno(c, REDIS_ERR_IO, "getsockopt(SO_ERROR)");
    return REDIS_ERR;
  }

  if (err == 0) {
    err = errno_saved;
  }

  if (err) {
    errno = err;
    __redisSetErrorFromErrno(c, REDIS_ERR_IO, nullptr);
    return REDIS_ERR;
  }

  return REDIS_OK;
}

int redisContextSetTimeout(redisContext *c, const struct timeval tv) {
  const void *to_ptr = &tv;
  size_t to_sz = sizeof(tv);

  if (redisContextUpdateCommandTimeout(c, &tv) != REDIS_OK) {
    __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
    return REDIS_ERR;
  }
  if (setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, to_ptr, to_sz) == -1) {
    __redisSetErrorFromErrno(c, REDIS_ERR_IO, "setsockopt(SO_RCVTIMEO)");
    return REDIS_ERR;
  }
  if (setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, to_ptr, to_sz) == -1) {
    __redisSetErrorFromErrno(c, REDIS_ERR_IO, "setsockopt(SO_SNDTIMEO)");
    return REDIS_ERR;
  }
  return REDIS_OK;
}

int redisContextUpdateConnectTimeout(redisContext *c,
                                     const struct timeval *timeout) {
  if (timeout == nullptr) {
    hi_free(c->connect_timeout);
    c->connect_timeout = nullptr;
    return REDIS_OK;
  }

  /* Same timeval struct, short circuit */
  if (c->connect_timeout == timeout)
    return REDIS_OK;

  /* Allocate context timeval if we need to */
  if (c->connect_timeout == nullptr) {
    c->connect_timeout = hi_malloc(sizeof(*c->connect_timeout));
    if (c->connect_timeout == nullptr)
      return REDIS_ERR;
  }

  memcpy(c->connect_timeout, timeout, sizeof(*c->connect_timeout));
  return REDIS_OK;
}

int redisContextUpdateCommandTimeout(redisContext *c,
                                     const struct timeval *timeout) {
  if (timeout == nullptr) {
    hi_free(c->command_timeout);
    c->command_timeout = nullptr;
    return REDIS_OK;
  }

  /* Same timeval struct, short circuit */
  if (c->command_timeout == timeout)
    return REDIS_OK;

  /* Allocate context timeval if we need to */
  if (c->command_timeout == nullptr) {
    c->command_timeout = hi_malloc(sizeof(*c->command_timeout));
    if (c->command_timeout == nullptr)
      return REDIS_ERR;
  }

  memcpy(c->command_timeout, timeout, sizeof(*c->command_timeout));
  return REDIS_OK;
}

static int _redisContextConnectTcp(redisContext *c, const char *addr, int port,
                                   const struct timeval *timeout,
                                   const char *source_addr) {
  redisFD s;
  int rv, n;
  constexpr size_t port_buf_size = 6; /* strlen("65535"); */
  char port_buf[port_buf_size];
  struct addrinfo hints = {0};
  struct addrinfo *servinfo, *bservinfo, *p, *b;
  auto blocking = (c->flags & REDIS_BLOCK);
  auto reuseaddr = (c->flags & REDIS_REUSEADDR);
  int reuses = 0;
  long timeout_msec = -1;
  constexpr size_t error_buf_size = 128;

  servinfo = nullptr;
  if (addr == nullptr || port < 0 || port > 65'535) {
    __redisSetError(c, REDIS_ERR_OTHER, "Invalid TCP endpoint");
    return REDIS_ERR;
  }

  c->connection_type = REDIS_CONN_TCP;
  c->tcp.port = port;

  /* We need to take possession of the passed parameters
   * to make them reusable for a reconnect.
   * We also carefully check we don't free data we already own,
   * as in the case of the reconnect method.
   *
   * This is a bit ugly, but atleast it works and doesn't leak memory.
   **/
  if (c->tcp.host != addr) {
    hi_free(c->tcp.host);

    c->tcp.host = hi_strdup(addr);
    if (c->tcp.host == nullptr)
      goto oom;
  }

  if (timeout) {
    if (redisContextUpdateConnectTimeout(c, timeout) == REDIS_ERR)
      goto oom;
  } else {
    hi_free(c->connect_timeout);
    c->connect_timeout = nullptr;
  }

  if (redisContextTimeoutMsec(c, &timeout_msec) != REDIS_OK) {
    goto error;
  }

  if (source_addr == nullptr) {
    hi_free(c->tcp.source_addr);
    c->tcp.source_addr = nullptr;
  } else if (c->tcp.source_addr != source_addr) {
    hi_free(c->tcp.source_addr);
    c->tcp.source_addr = hi_strdup(source_addr);
    if (c->tcp.source_addr == nullptr)
      goto oom;
  }

  snprintf(port_buf, sizeof(port_buf), "%d", port);
  hints.ai_family = AF_INET;
  hints.ai_socktype = SOCK_STREAM;

  /* DNS lookup. To use dual stack, set both flags to prefer both IPv4 and
   * IPv6. By default, for historical reasons, we try IPv4 first and then we
   * try IPv6 only if no IPv4 address was found. */
  if (c->flags & REDIS_PREFER_IPV6 && c->flags & REDIS_PREFER_IPV4)
    hints.ai_family = AF_UNSPEC;
  else if (c->flags & REDIS_PREFER_IPV6)
    hints.ai_family = AF_INET6;
  else
    hints.ai_family = AF_INET;

  rv = getaddrinfo(c->tcp.host, port_buf, &hints, &servinfo);
  if (rv != 0 && hints.ai_family != AF_UNSPEC) {
    /* Try again with the other IP version. */
    hints.ai_family = (hints.ai_family == AF_INET) ? AF_INET6 : AF_INET;
    rv = getaddrinfo(c->tcp.host, port_buf, &hints, &servinfo);
  }
  if (rv != 0) {
    __redisSetError(c, REDIS_ERR_OTHER, gai_strerror(rv));
    return REDIS_ERR;
  }
  for (p = servinfo; p != nullptr; p = p->ai_next) {
  addrretry: {
    int sock_type = p->ai_socktype;

#ifdef SOCK_CLOEXEC
    if (c->flags & REDIS_OPT_SET_SOCK_CLOEXEC) {
      sock_type |= SOCK_CLOEXEC;
    }
#endif

    if ((s = socket(p->ai_family, sock_type, p->ai_protocol)) ==
        REDIS_INVALID_FD)
      continue;
  }

    c->fd = s;

    if (redisSetBlocking(c, 0) != REDIS_OK)
      goto error;
    if (c->tcp.source_addr) {
      int bound = 0;
      /* Using getaddrinfo saves us from self-determining IPv4 vs IPv6 */
      if ((rv = getaddrinfo(c->tcp.source_addr, nullptr, &hints, &bservinfo)) !=
          0) {
        char buf[error_buf_size];
        snprintf(buf, sizeof(buf), "Can't get addr: %s", gai_strerror(rv));
        __redisSetError(c, REDIS_ERR_OTHER, buf);
        goto error;
      }

      if (reuseaddr) {
        n = 1;
        if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, (char *)&n, sizeof(n)) <
            0) {
          freeaddrinfo(bservinfo);
          goto error;
        }
      }

      for (b = bservinfo; b != nullptr; b = b->ai_next) {
        if (bind(s, b->ai_addr, b->ai_addrlen) != -1) {
          bound = 1;
          break;
        }
      }
      freeaddrinfo(bservinfo);
      if (!bound) {
        char buf[error_buf_size];
        snprintf(buf, sizeof(buf), "Can't bind socket: %s", strerror(errno));
        __redisSetError(c, REDIS_ERR_OTHER, buf);
        goto error;
      }
    }

    /* For repeat connection */
    hi_free(c->saddr);
    c->saddr = hi_malloc(p->ai_addrlen);
    if (c->saddr == nullptr)
      goto oom;

    memcpy(c->saddr, p->ai_addr, p->ai_addrlen);
    c->addrlen = p->ai_addrlen;

    if (connect(s, p->ai_addr, p->ai_addrlen) == -1) {
      if (errno == EHOSTUNREACH) {
        redisNetClose(c);
        continue;
      } else if (errno == EINPROGRESS) {
        if (blocking) {
          goto wait_for_ready;
        }
        /* This is ok.
         * Note that even when it's in blocking mode, we unset blocking
         * for `connect()`
         */
      } else if (errno == EADDRNOTAVAIL && reuseaddr) {
        if (++reuses >= REDIS_CONNECT_RETRIES) {
          goto error;
        } else {
          redisNetClose(c);
          goto addrretry;
        }
      } else {
      wait_for_ready:
        if (redisContextWaitReady(c, timeout_msec) != REDIS_OK)
          goto error;
        if (redisSetTcpNoDelay(c) != REDIS_OK)
          goto error;
      }
    }
    if (blocking && redisSetBlocking(c, 1) != REDIS_OK)
      goto error;

    c->flags |= REDIS_CONNECTED;
    rv = REDIS_OK;
    goto end;
  }
  if (p == nullptr) {
    char buf[error_buf_size];
    snprintf(buf, sizeof(buf), "Can't create socket: %s", strerror(errno));
    __redisSetError(c, REDIS_ERR_OTHER, buf);
    goto error;
  }

oom:
  __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
error:
  rv = REDIS_ERR;
end:
  if (servinfo) {
    freeaddrinfo(servinfo);
  }

  return rv; // Need to return REDIS_OK if alright
}

int redisContextConnectTcp(redisContext *c, const char *addr, int port,
                           const struct timeval *timeout) {
  return _redisContextConnectTcp(c, addr, port, timeout, nullptr);
}

int redisContextConnectBindTcp(redisContext *c, const char *addr, int port,
                               const struct timeval *timeout,
                               const char *source_addr) {
  return _redisContextConnectTcp(c, addr, port, timeout, source_addr);
}

int redisContextConnectUnix(redisContext *c, const char *path,
                            const struct timeval *timeout) {
  int blocking = (c->flags & REDIS_BLOCK);
  struct sockaddr_un *sa;
  long timeout_msec = -1;

  if (path == nullptr) {
    __redisSetError(c, REDIS_ERR_OTHER, "Invalid UNIX socket path");
    return REDIS_ERR;
  }

  if (redisCreateSocket(c, AF_UNIX) < 0)
    return REDIS_ERR;
  if (redisSetBlocking(c, 0) != REDIS_OK)
    return REDIS_ERR;

  c->connection_type = REDIS_CONN_UNIX;
  if (c->unix_sock.path != path) {
    hi_free(c->unix_sock.path);

    c->unix_sock.path = hi_strdup(path);
    if (c->unix_sock.path == nullptr)
      goto oom;
  }

  if (timeout) {
    if (redisContextUpdateConnectTimeout(c, timeout) == REDIS_ERR)
      goto oom;
  } else {
    hi_free(c->connect_timeout);
    c->connect_timeout = nullptr;
  }

  if (redisContextTimeoutMsec(c, &timeout_msec) != REDIS_OK)
    return REDIS_ERR;

  /* Don't leak sockaddr if we're reconnecting */
  if (c->saddr)
    hi_free(c->saddr);

  sa = (struct sockaddr_un *)(c->saddr = hi_malloc(sizeof(struct sockaddr_un)));
  if (sa == nullptr)
    goto oom;

  c->addrlen = sizeof(struct sockaddr_un);
  memset(sa, 0, sizeof(*sa));
  size_t path_len = strlen(path);
  if (path_len >= sizeof(sa->sun_path)) {
    __redisSetError(c, REDIS_ERR_OTHER, "UNIX socket path too long");
    return REDIS_ERR;
  }
  sa->sun_family = AF_UNIX;
  memcpy(sa->sun_path, path, path_len);
  sa->sun_path[path_len] = '\0';
  if (connect(c->fd, (struct sockaddr *)sa, sizeof(*sa)) == -1) {
    if ((errno == EAGAIN || errno == EINPROGRESS) && !blocking) {
      /* This is ok. */
    } else {
      if (redisContextWaitReady(c, timeout_msec) != REDIS_OK)
        return REDIS_ERR;
    }
  }

  /* Reset socket to be blocking after connect(2). */
  if (blocking && redisSetBlocking(c, 1) != REDIS_OK)
    return REDIS_ERR;

  c->flags |= REDIS_CONNECTED;
  return REDIS_OK;
oom:
  __redisSetError(c, REDIS_ERR_OOM, "Out of memory");
  return REDIS_ERR;
}
