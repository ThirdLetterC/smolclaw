/* Copyright (c) 2024-2025, CK Tan.
 * https://github.com/cktan/tomlc17/blob/main/LICENSE
 */

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "toml/toml.h"

const toml_datum_t DATUM_ZERO = {0};

static toml_option_t toml_option = {false, realloc, free};

#define MALLOC(n) toml_option.mem_realloc(nullptr, n)
#define REALLOC(p, n) toml_option.mem_realloc(p, n)
#define FREE(p) toml_option.mem_free(p)

#define DO(x)  \
  if (x)       \
    return -1; \
  else         \
    (void)0

static inline size_t align8(size_t x) { return (((x) + 7U) & ~((size_t)7)); }

static inline bool is_ctype_arg(int ch) { return 0 <= ch && ch <= UCHAR_MAX; }

static inline bool is_ascii_alnum(int ch) {
  return is_ctype_arg(ch) && isalnum((unsigned char)ch);
}

static inline bool is_ascii_digit(int ch) {
  return is_ctype_arg(ch) && isdigit((unsigned char)ch);
}

static inline int to_ascii_lower(int ch) {
  return is_ctype_arg(ch) ? tolower((unsigned char)ch) : ch;
}

static inline int to_ascii_upper(int ch) {
  return is_ctype_arg(ch) ? toupper((unsigned char)ch) : ch;
}

static inline bool checked_array_realloc_size(int count, size_t elem_size,
                                              size_t *ret_size) {
  if (count < 0 || count == INT_MAX) {
    return false;
  }
  size_t slots = align8((size_t)count + 1U);
  if (slots > SIZE_MAX / elem_size) {
    return false;
  }
  *ret_size = slots * elem_size;
  return true;
}

static inline bool grow_parse_buffer(int current, int *next) {
  if (current < 0 || current >= INT_MAX - 1) {
    return false;
  }
  size_t grown = (size_t)current + ((size_t)current / 2U) +
                 1'000U; if (grown > (size_t)(INT_MAX - 1)) {
    grown = (size_t)(INT_MAX - 1);
  }
  *next = (int)grown;
  return true;
}

/*
 *  Error buffer
 */
typedef struct ebuf_t ebuf_t;
struct ebuf_t {
  char *ptr;
  int len;
};

/*
 *  Format an error into ebuf[]. Always return -1.
 */
static int RETERROR(ebuf_t ebuf, int lineno, const char *fmt, ...) {
  va_list args;
  va_start(args, fmt);
  if (!ebuf.ptr || ebuf.len <= 0) {
    va_end(args);
    return -1;
  }
  char *p = ebuf.ptr;
  char *q = p + ebuf.len;
  if (lineno) {
    snprintf(p, q - p, "(line %d) ", lineno);
    p += strlen(p);
  }
  vsnprintf(p, q - p, fmt, args);
  va_end(args);
  return -1;
}

/*
 *  Memory pool. Allocated a big block once and hand out piecemeal.
 */
typedef struct pool_t pool_t;
struct pool_t {
  int top, max;
  char buf[1];  // first byte starts here
};

/**
 *  Create a memory pool of N bytes. Return the memory pool on
 *  success, or nullptr if out of memory.
 */
[[nodiscard]] static pool_t *pool_create(int N) {
  if (N <= 0) {
    N = 100;  // minimum
  }
  size_t totalsz = sizeof(pool_t) + (size_t)N;
  pool_t *pool = MALLOC(totalsz);
  if (!pool) {
    return nullptr;
  }
  memset(pool, 0, totalsz);
  pool->max = N;
  return pool;
}

/**
 *  Destroy a memory pool.
 */
static void pool_destroy(pool_t *pool) { FREE(pool); }

/**
 *  Allocate n bytes from pool. Return the memory allocated on
 *  success, or nullptr if out of memory.
 */
[[nodiscard]] static char *pool_alloc(pool_t *pool, int n) {
  if (!pool || n < 0 || pool->top < 0 || pool->max < 0 ||
      n > pool->max - pool->top) {
    return nullptr;
  }
  char *ret = pool->buf + pool->top;
  pool->top += n;
  return ret;
}

/* This is a string view. */
typedef struct span_t span_t;
struct span_t {
  const char *ptr;
  int len;
};

static inline bool span_matches_cstr(span_t span, const char *s) {
  if (!s || span.len < 0) {
    return false;
  }
  auto len = (size_t)span.len;
  for (size_t i = 0; i < len; i++) {
    if (s[i] == '\0' || s[i] != span.ptr[i]) {
      return false;
    }
  }
  return s[len] == '\0';
}

static inline bool multipart_key_len(const char *s, size_t *len) {
  if (!s || !len) {
    return false;
  }
  constexpr size_t limit = 127;
  for (size_t i = 0; i < limit; i++) {
    if (s[i] == '\0') {
      *len = i;
      return true;
    }
  }
  return false;
}

static inline bool is_unicode_scalar(uint32_t code) {
  return code <= 0x10FFFFU && !(0xD800U <= code && code <= 0xDFFFU);
}

/* Represents a multi-part key */
static constexpr int KEYPARTMAX = 10;
typedef struct keypart_t keypart_t;
struct keypart_t {
  int nspan;
  span_t span[KEYPARTMAX];
};

[[nodiscard]] static int utf8_to_ucs(const char *s, int len, uint32_t *ret);
[[nodiscard]] static int ucs_to_utf8(uint32_t code, char buf[4]);

// flags for toml_datum_t::flag.
static constexpr uint32_t FLAG_INLINED = 0b001;
static constexpr uint32_t FLAG_STDEXPR = 0b010;
static constexpr uint32_t FLAG_EXPLICIT = 0b100;

// Maximum levels of brackets and braces to prevent
// stack overflow during recursive descent of the parser.
static constexpr int BRACKET_LEVEL_MAX = 30;
static constexpr int BRACE_LEVEL_MAX = 30;

enum toktyp_t {
  TOK_DOT = 1,
  TOK_EQUAL,
  TOK_COMMA,
  TOK_LBRACK,
  TOK_LLBRACK,
  TOK_RBRACK,
  TOK_RRBRACK,
  TOK_LBRACE,
  TOK_RBRACE,
  TOK_LIT,
  TOK_STRING,
  TOK_MLSTRING,
  TOK_LITSTRING,
  TOK_MLLITSTRING,
  TOK_TIME,
  TOK_DATE,
  TOK_DATETIME,
  TOK_DATETIMETZ,
  TOK_INTEGER,
  TOK_FLOAT,
  TOK_BOOL,
  TOK_ENDL,
  TOK_FIN = -5000,  // EOF
};
typedef enum toktyp_t toktyp_t;
typedef struct scanner_t scanner_t;

/* Remember the current state of a scanner */
typedef struct scanner_state_t scanner_state_t;
struct scanner_state_t {
  scanner_t *sp;
  const char *cur;  // points into scanner_t::src[]
  int lineno;       // current line number
  int bracket_level;
  int brace_level;
};

// A scan token
typedef struct token_t token_t;
struct token_t {
  toktyp_t toktyp;
  int lineno;
  span_t str;

  // values represented by str
  union {
    int64_t int64;
    double fp64;
    bool b1;
    struct {
      // validity depends on toktyp for TIME, DATE, DATETIME, DATETIMETZ
      int year, month, day, hour, minute, sec, usec;
      int tz;  // +- minutes
    } tsval;
  } u;
};

// Scanner object
struct scanner_t {
  const char *src;   // src[] is a NUL-terminated string
  const char *endp;  // end of src[]. always pointing at a NUL char.
  const char *cur;   // current char in src[]
  int lineno;        // line number of current char
  char *errmsg;      // point to errbuf if there was an error
  ebuf_t ebuf;

  int bracket_level;  // count depth of [ ]
  int brace_level;    // count depth of { }
};
static void scan_init(scanner_t *sp, const char *src, int len, char *errbuf,
                      int errbufsz);
[[nodiscard]] static int scan_key(scanner_t *sp, token_t *tok);
[[nodiscard]] static int scan_value(scanner_t *sp, token_t *tok);
// restore scanner to state before tok was returned
static scanner_state_t scan_mark(scanner_t *sp);
static void scan_restore(scanner_t *sp, scanner_state_t state);

// Parser object
typedef struct parser_t parser_t;
struct parser_t {
  scanner_t scanner;
  toml_datum_t toptab;   // top table
  toml_datum_t *curtab;  // current table
  pool_t *pool;          // memory pool for strings
  ebuf_t ebuf;
};

// Put key into tab dictionary. Return a place to
// the datum for the key on success, or nullptr otherwise.
[[nodiscard]] static toml_datum_t *tab_emplace(toml_datum_t *tab, span_t key,
                                               const char **reason) {
  assert(tab->type == TOML_TABLE);
  int N = tab->u.tab.size;
  size_t bytes = 0;
  for (int i = 0; i < N; i++) {
    if (tab->u.tab.len[i] == key.len &&
        0 == memcmp(tab->u.tab.key[i], key.ptr, key.len)) {
      return &tab->u.tab.value[i];
    }
  }
  if (!checked_array_realloc_size(N, sizeof(*tab->u.tab.key), &bytes)) {
    *reason = "table is too large";
    return nullptr;
  }
  // Expand pkey[], plen[] and value[]
  {
    char **pkey = REALLOC(tab->u.tab.key, bytes);
    if (!pkey) {
      *reason = "out of memory";
      return nullptr;
    }
    tab->u.tab.key = (const char **)pkey;
  }

  if (!checked_array_realloc_size(N, sizeof(*tab->u.tab.len), &bytes)) {
    *reason = "table is too large";
    return nullptr;
  }
  {
    int *plen = REALLOC(tab->u.tab.len, bytes);
    if (!plen) {
      *reason = "out of memory";
      return nullptr;
    }
    tab->u.tab.len = plen;
  }

  if (!checked_array_realloc_size(N, sizeof(*tab->u.tab.value), &bytes)) {
    *reason = "table is too large";
    return nullptr;
  }
  {
    toml_datum_t *value = REALLOC(tab->u.tab.value, bytes);
    if (!value) {
      *reason = "out of memory";
      return nullptr;
    }
    tab->u.tab.value = value;
  }

  // Append the new key/value
  tab->u.tab.size = N + 1;
  tab->u.tab.key[N] = (char *)key.ptr;
  tab->u.tab.len[N] = key.len;
  tab->u.tab.value[N] = DATUM_ZERO;
  return &tab->u.tab.value[N];
}

// Find key in tab and return its index. If not found, return -1.
[[nodiscard]] static int tab_find(toml_datum_t *tab, span_t key) {
  assert(tab->type == TOML_TABLE);
  for (int i = 0, top = tab->u.tab.size; i < top; i++) {
    if (tab->u.tab.len[i] == key.len &&
        0 == memcmp(tab->u.tab.key[i], key.ptr, key.len)) {
      return i;
    }
  }
  return -1;
}

// Add a new key in tab. Return 0 on success, -1 otherwise.
// On error, *reason will point to an error message.
[[nodiscard]] static int tab_add(toml_datum_t *tab, span_t newkey,
                                 toml_datum_t newvalue, const char **reason) {
  assert(tab->type == TOML_TABLE);
  if (-1 != tab_find(tab, newkey)) {
    *reason = "duplicate key";
    return -1;
  }
  toml_datum_t *pvalue = tab_emplace(tab, newkey, reason);
  if (!pvalue) {
    return -1;
  }
  *pvalue = newvalue;
  return 0;
}

// Add a new element into an array. Return 0 on success, -1 otherwise.
// On error, *reason will point to an error message.
[[nodiscard]] static toml_datum_t *arr_emplace(toml_datum_t *arr,
                                               const char **reason) {
  assert(arr->type == TOML_ARRAY);
  int n = arr->u.arr.size;
  size_t bytes = 0;
  if (!checked_array_realloc_size(n, sizeof(*arr->u.arr.elem), &bytes)) {
    *reason = "array is too large";
    return nullptr;
  }
  toml_datum_t *elem = REALLOC(arr->u.arr.elem, bytes);
  if (!elem) {
    *reason = "out of memory";
    return nullptr;
  }
  arr->u.arr.elem = elem;
  arr->u.arr.size = n + 1;
  elem[n] = DATUM_ZERO;
  return &elem[n];
}

// ------------------- parser section
[[nodiscard]] static int parse_norm(parser_t *pp, token_t tok,
                                    span_t *ret_span);
[[nodiscard]] static int parse_val(parser_t *pp, token_t tok,
                                   toml_datum_t *ret);
[[nodiscard]] static int parse_keyvalue_expr(parser_t *pp, token_t tok);
[[nodiscard]] static int parse_std_table_expr(parser_t *pp, token_t tok);
[[nodiscard]] static int parse_array_table_expr(parser_t *pp, token_t tok);

static toml_datum_t mkdatum(toml_type_t ty) {
  toml_datum_t ret = {0};
  ret.type = ty;
  if (ty == TOML_DATE || ty == TOML_TIME || ty == TOML_DATETIME ||
      ty == TOML_DATETIMETZ) {
    ret.u.ts.year = -1;
    ret.u.ts.month = -1;
    ret.u.ts.day = -1;
    ret.u.ts.hour = -1;
    ret.u.ts.minute = -1;
    ret.u.ts.second = -1;
    ret.u.ts.usec = -1;
    ret.u.ts.tz = -1;
  }
  return ret;
}

// Recursively free any dynamically allocated memory in the datum tree
static void datum_free(toml_datum_t *datum) {
  if (datum->type == TOML_TABLE) {
    for (int i = 0, top = datum->u.tab.size; i < top; i++) {
      datum_free(&datum->u.tab.value[i]);
    }
    FREE(datum->u.tab.key);
    FREE(datum->u.tab.len);
    FREE(datum->u.tab.value);
  } else if (datum->type == TOML_ARRAY) {
    for (int i = 0, top = datum->u.arr.size; i < top; i++) {
      datum_free(&datum->u.arr.elem[i]);
    }
    FREE(datum->u.arr.elem);
  }
  // other types do not allocate memory
  *datum = DATUM_ZERO;
}

// Make a deep copy of src to dst.
// Return 0 on success, -1 otherwise.
[[nodiscard]] static int datum_copy(toml_datum_t *dst, toml_datum_t src,
                                    pool_t *pool, const char **reason) {
  *dst = mkdatum(src.type);
  switch (src.type) {
    case TOML_STRING:
      dst->u.str.ptr = pool_alloc(pool, src.u.str.len + 1);
      if (!dst->u.str.ptr) {
        *reason = "out of memory";
        goto bail;
      }
      dst->u.str.len = src.u.str.len;
      memcpy((char *)dst->u.str.ptr, src.u.str.ptr, src.u.str.len + 1);
      break;
    case TOML_TABLE:
      for (int i = 0; i < src.u.tab.size; i++) {
        span_t newkey = {src.u.tab.key[i], src.u.tab.len[i]};
        toml_datum_t *pvalue = tab_emplace(dst, newkey, reason);
        if (!pvalue) {
          goto bail;
        }
        if (datum_copy(pvalue, src.u.tab.value[i], pool, reason)) {
          goto bail;
        }
      }
      break;
    case TOML_ARRAY:
      for (int i = 0; i < src.u.arr.size; i++) {
        toml_datum_t *pelem = arr_emplace(dst, reason);
        if (!pelem) {
          goto bail;
        }
        if (datum_copy(pelem, src.u.arr.elem[i], pool, reason)) {
          goto bail;
        }
      }
      break;
    default:
      *dst = src;
      break;
  }

  return 0;

bail:
  datum_free(dst);
  return -1;
}

// Check if datum is an array of tables.
static inline bool is_array_of_tables(toml_datum_t datum) {
  bool ret = (datum.type == TOML_ARRAY);
  for (int i = 0; ret && i < datum.u.arr.size; i++) {
    ret = (datum.u.arr.elem[i].type == TOML_TABLE);
  }
  return ret;
}

// Merge src into dst. Return 0 on success, -1 otherwise.
[[nodiscard]] static int datum_merge(toml_datum_t *dst, toml_datum_t src,
                                     pool_t *pool, const char **reason) {
  if (dst->type != src.type) {
    datum_free(dst);
    return datum_copy(dst, src, pool, reason);
  }
  switch (src.type) {
    case TOML_TABLE:
      // for key-value in src:
      //    override key-value in dst.
      for (int i = 0; i < src.u.tab.size; i++) {
        span_t key;
        key.ptr = src.u.tab.key[i];
        key.len = src.u.tab.len[i];
        toml_datum_t *pvalue = tab_emplace(dst, key, reason);
        if (!pvalue) {
          return -1;
        }
        if (pvalue->type) {
          DO(datum_merge(pvalue, src.u.tab.value[i], pool, reason));
        } else {
          datum_free(pvalue);
          DO(datum_copy(pvalue, src.u.tab.value[i], pool, reason));
        }
      }
      return 0;
    case TOML_ARRAY:
      if (is_array_of_tables(src)) {
        // append src array to dst
        for (int i = 0; i < src.u.arr.size; i++) {
          toml_datum_t *pelem = arr_emplace(dst, reason);
          if (!pelem) {
            return -1;
          }
          DO(datum_copy(pelem, src.u.arr.elem[i], pool, reason));
        }
        return 0;
      }
      // fallthru
    default:
      break;
  }
  datum_free(dst);
  return datum_copy(dst, src, pool, reason);
}

// Compare the content of a and b.
static bool datum_equiv(toml_datum_t a, toml_datum_t b) {
  if (a.type != b.type) {
    return false;
  }
  int N;
  switch (a.type) {
    case TOML_STRING:
      return a.u.str.len == b.u.str.len &&
             0 == memcmp(a.u.str.ptr, b.u.str.ptr, a.u.str.len);
    case TOML_INT64:
      return a.u.int64 == b.u.int64;
    case TOML_FP64:
      return a.u.fp64 == b.u.fp64;
    case TOML_BOOLEAN:
      return !!a.u.boolean == !!b.u.boolean;
    case TOML_DATE:
      return a.u.ts.year == b.u.ts.year && a.u.ts.month == b.u.ts.month &&
             a.u.ts.day == b.u.ts.day;
    case TOML_TIME:
      return a.u.ts.hour == b.u.ts.hour && a.u.ts.minute == b.u.ts.minute &&
             a.u.ts.second == b.u.ts.second && a.u.ts.usec == b.u.ts.usec;
    case TOML_DATETIME:
      return a.u.ts.year == b.u.ts.year && a.u.ts.month == b.u.ts.month &&
             a.u.ts.day == b.u.ts.day && a.u.ts.hour == b.u.ts.hour &&
             a.u.ts.minute == b.u.ts.minute && a.u.ts.second == b.u.ts.second &&
             a.u.ts.usec == b.u.ts.usec;
    case TOML_DATETIMETZ:
      return a.u.ts.year == b.u.ts.year && a.u.ts.month == b.u.ts.month &&
             a.u.ts.day == b.u.ts.day && a.u.ts.hour == b.u.ts.hour &&
             a.u.ts.minute == b.u.ts.minute && a.u.ts.second == b.u.ts.second &&
             a.u.ts.usec == b.u.ts.usec && a.u.ts.tz == b.u.ts.tz;
    case TOML_ARRAY:
      N = a.u.arr.size;
      if (N != b.u.arr.size) {
        return false;
      }
      for (int i = 0; i < N; i++) {
        if (!datum_equiv(a.u.arr.elem[i], b.u.arr.elem[i])) {
          return false;
        }
      }
      return true;
    case TOML_TABLE:
      N = a.u.tab.size;
      if (N != b.u.tab.size) {
        return false;
      }
      for (int i = 0; i < N; i++) {
        int len = a.u.tab.len[i];
        if (len != b.u.tab.len[i]) {
          return false;
        }
        if (0 != memcmp(a.u.tab.key[i], b.u.tab.key[i], len)) {
          return false;
        }
        if (!datum_equiv(a.u.tab.value[i], b.u.tab.value[i])) {
          return false;
        }
      }
      return true;
    default:
      break;
  }
  return false;
}

/**
 *  Override values in r1 using r2. Return a new result. All results
 *  (i.e., r1, r2 and the returned result) must be freed using toml_free()
 *  after use.
 *
 *  LOGIC:
 *   ret = copy of r1
 *   for each item x in r2:
 *     if x is not in ret:
 *          override
 *     elif x in ret is NOT of the same type:
 *         override
 *     elif x is an array of tables:
 *         append r2.x to ret.x
 *     elif x is a table:
 *         merge r2.x to ret.x
 *     else:
 *         override
 */
toml_result_t toml_merge(const toml_result_t *r1, const toml_result_t *r2) {
  const char *reason = "";
  toml_result_t ret = {0};
  pool_t *pool = nullptr;
  if (!r1 || !r2) {
    reason = "param error: null result";
    goto bail;
  }
  if (!r1->ok) {
    reason = "param error: r1 not ok";
    goto bail;
  }
  if (!r2->ok) {
    reason = "param error: r2 not ok";
    goto bail;
  }
  if (!r1->__internal || !r2->__internal) {
    reason = "param error: invalid internal state";
    goto bail;
  }
  {
    pool_t *r1pool = (pool_t *)r1->__internal;
    pool_t *r2pool = (pool_t *)r2->__internal;
    if (r1pool->top < 0 || r2pool->top < 0 ||
        r1pool->top > INT_MAX - r2pool->top) {
      reason = "param error: invalid internal state";
      goto bail;
    }
    pool = pool_create(r1pool->top + r2pool->top);
    if (!pool) {
      reason = "out of memory";
      goto bail;
    }
  }

  // Make a copy of r1
  if (datum_copy(&ret.toptab, r1->toptab, pool, &reason)) {
    goto bail;
  }

  // Merge r2 into the result
  if (datum_merge(&ret.toptab, r2->toptab, pool, &reason)) {
    goto bail;
  }

  ret.ok = true;
  ret.__internal = pool;
  return ret;

bail:
  datum_free(&ret.toptab);
  pool_destroy(pool);
  snprintf(ret.errmsg, sizeof(ret.errmsg), "%s", reason);
  return ret;
}

[[nodiscard]] bool toml_equiv(const toml_result_t *r1,
                              const toml_result_t *r2) {
  if (!r1 || !r2) {
    return false;
  }
  if (!(r1->ok && r2->ok)) {
    return false;
  }
  return datum_equiv(r1->toptab, r2->toptab);
}

/**
 * Find a key in a toml_table. Return the value of the key if found,
 * or a TOML_UNKNOWN otherwise.
 */
[[nodiscard]] toml_datum_t toml_get(toml_datum_t datum, const char *key) {
  toml_datum_t ret = {0};
  if (datum.type == TOML_TABLE && key != nullptr) {
    int n = datum.u.tab.size;
    const char **pkey = datum.u.tab.key;
    toml_datum_t *pvalue = datum.u.tab.value;
    for (int i = 0; i < n; i++) {
      span_t entry = {.ptr = pkey[i], .len = datum.u.tab.len[i]};
      if (span_matches_cstr(entry, key)) {
        return pvalue[i];
      }
    }
  }
  return ret;
}

/**
 * Locate a value starting from a toml_table. Return the value of the key if
 * found, or a TOML_UNKNOWN otherwise.
 *
 * Note: the multipart-key is separated by DOT, and must not have any escape
 * chars.
 */
[[nodiscard]] toml_datum_t toml_seek(toml_datum_t table,
                                     const char *multipart_key) {
  if (table.type != TOML_TABLE || multipart_key == nullptr) {
    return DATUM_ZERO;
  }

  char buf[128];
  size_t key_len = 0;
  if (!multipart_key_len(multipart_key, &key_len)) {
    return DATUM_ZERO;
  }
  memcpy(buf, multipart_key, key_len);
  buf[key_len] = '\0';

  // go through the multipart name part by part.
  char *p = buf;
  char *q = strchr(p, '.');
  toml_datum_t datum = table;
  while (q && datum.type == TOML_TABLE) {
    *q = 0;
    datum = toml_get(datum, p);
    if (datum.type == TOML_TABLE) {
      p = q + 1;
      q = strchr(p, '.');
    }
  }

  if (!q && datum.type == TOML_TABLE) {
    return toml_get(datum, p);
  }

  return DATUM_ZERO;
}

/**
 *  Return the default options.
 */
[[nodiscard]] toml_option_t toml_default_option() {
  toml_option_t opt = {false, realloc, free};
  return opt;
}

/**
 *  Override the current options.
 */
void toml_set_option(toml_option_t opt) {
  if (!opt.mem_realloc) {
    opt.mem_realloc = realloc;
  }
  if (!opt.mem_free) {
    opt.mem_free = free;
  }
  toml_option = opt;
}

/**
 *  Free the result returned by toml_parse().
 */
void toml_free(toml_result_t result) {
  datum_free(&result.toptab);
  pool_destroy((pool_t *)result.__internal);
}

/**
 *  Parse a toml document.
 */
toml_result_t toml_parse_file_ex(const char *fname) {
  toml_result_t result = {0};
  if (!fname) {
    snprintf(result.errmsg, sizeof(result.errmsg), "fname must not be null");
    return result;
  }
  FILE *fp = fopen(fname, "rb");
  if (!fp) {
    snprintf(result.errmsg, sizeof(result.errmsg), "fopen: %s", fname);
    return result;
  }
  result = toml_parse_file(fp);
  fclose(fp);
  return result;
}

/**
 *  Parse a toml document.
 */
toml_result_t toml_parse_file(FILE *fp) {
  toml_result_t result = {0};
  char *buf = nullptr;
  int top, max;  // index into buf[]
  top = max = 0;

  if (!fp) {
    snprintf(result.errmsg, sizeof(result.errmsg), "fp must not be null");
    return result;
  }

  // Read file into memory
  for (;;) {
    assert(top <= max);
    if (top == max) {
      // need to extend buf[]
      int tmpmax = 0;
      if (!grow_parse_buffer(max, &tmpmax)) {
        snprintf(result.errmsg, sizeof(result.errmsg),
                 "file is bigger than %d bytes", INT_MAX - 1);
        FREE(buf);
        return result;
      }
      // add an extra byte for terminating NUL
      char *tmp = REALLOC(buf, (size_t)tmpmax + 1U);
      if (!tmp) {
        snprintf(result.errmsg, sizeof(result.errmsg), "out of memory");
        FREE(buf);
        return result;
      }
      buf = tmp;
      max = tmpmax;
    }

    errno = 0;
    size_t nread = fread(buf + top, 1, (size_t)(max - top), fp);
    if (nread > (size_t)(INT_MAX - top)) {
      snprintf(result.errmsg, sizeof(result.errmsg), "file is too large");
      FREE(buf);
      return result;
    }
    top += (int)nread;
    if (ferror(fp)) {
      snprintf(result.errmsg, sizeof(result.errmsg), "%s",
               errno ? strerror(errno) : "Error reading file");
      FREE(buf);
      return result;
    }
    if (nread == 0) {
      break;
    }
  }
  buf[top] = 0;  // NUL terminator

  result = toml_parse(buf, top);
  FREE(buf);
  return result;
}

/**
 *  Parse a toml document.
 */
toml_result_t toml_parse(const char *src, int len) {
  toml_result_t result = {0};
  parser_t parser = {0};
  parser_t *pp = &parser;

  if (!src) {
    snprintf(result.errmsg, sizeof(result.errmsg), "src must not be null");
    goto bail;
  }
  if (len < 0) {
    snprintf(result.errmsg, sizeof(result.errmsg), "len must not be negative");
    goto bail;
  }
  if (len > INT_MAX - 10) {
    snprintf(result.errmsg, sizeof(result.errmsg), "src is too large");
    goto bail;
  }

  // Check that src is NUL terminated.
  if (src[len]) {
    snprintf(result.errmsg, sizeof(result.errmsg),
             "src[] must be NUL terminated");
    goto bail;
  }

  // If user insists, check that src[] is a valid utf8 string.
  if (toml_option.check_utf8) {
    int line = 1;  // keeps track of line number
    for (int i = 0; i < len;) {
      uint32_t ch;
      int n = utf8_to_ucs(src + i, len - i, &ch);
      if (n < 0) {
        snprintf(result.errmsg, sizeof(result.errmsg),
                 "invalid UTF8 char on line %d", line);
        goto bail;
      }
      if (!is_unicode_scalar(ch)) {
        snprintf(result.errmsg, sizeof(result.errmsg),
                 "invalid UTF8 char \\u%04x on line %d", ch, line);
        goto bail;
      }
      line += (ch == '\n' ? 1 : 0);
      i += n;
    }
  }

  // Initialize parser
  pp->toptab = mkdatum(TOML_TABLE);
  pp->curtab = &pp->toptab;
  pp->ebuf.ptr = result.errmsg;
  pp->ebuf.len = sizeof(result.errmsg);

  // Alloc memory pool
  pp->pool =
      pool_create(len + 10);  // add some extra bytes for NUL term and safety
  if (!pp->pool) {
    snprintf(result.errmsg, sizeof(result.errmsg), "out of memory");
    goto bail;
  }

  // Initialize scanner.
  scan_init(&pp->scanner, src, len, pp->ebuf.ptr, pp->ebuf.len);

  // Keep parsing until FIN
  for (;;) {
    token_t tok;
    if (scan_key(&pp->scanner, &tok)) {
      goto bail;
    }
    // break on FIN
    if (tok.toktyp == TOK_FIN) {
      break;
    }
    switch (tok.toktyp) {
      case TOK_ENDL:  // skip blank lines
        continue;
      case TOK_LBRACK:
        if (parse_std_table_expr(pp, tok)) {
          goto bail;
        }
        break;
      case TOK_LLBRACK:
        if (parse_array_table_expr(pp, tok)) {
          goto bail;
        }
        break;
      default:
        // non-blank line: parse an expression
        if (parse_keyvalue_expr(pp, tok)) {
          goto bail;
        }
        break;
    }
    // each expression must be followed by newline
    if (scan_key(&pp->scanner, &tok)) {
      goto bail;
    }
    if (tok.toktyp == TOK_FIN || tok.toktyp == TOK_ENDL) {
      continue;
    }
    RETERROR(pp->ebuf, tok.lineno, "ENDL expected");
    goto bail;
  }

  // return result
  result.ok = true;
  result.toptab = pp->toptab;
  result.__internal = (void *)pp->pool;
  return result;

bail:
  // return error
  datum_free(&pp->toptab);
  pool_destroy(pp->pool);
  result.ok = false;
  assert(result.errmsg[0]);  // make sure there is an errmsg
  return result;
}

// Convert a (LITSTRING, LIT, MLLITSTRING, MLSTRING, or STRING) token to a
// datum.
[[nodiscard]] static int token_to_string(parser_t *pp, token_t tok,
                                         toml_datum_t *ret) {
  *ret = mkdatum(TOML_STRING);
  span_t span;
  DO(parse_norm(pp, tok, &span));
  ret->u.str.ptr = (char *)span.ptr;
  ret->u.str.len = span.len;
  return 0;
}

// Convert TIME token to a datum.
[[nodiscard]] static int token_to_time([[maybe_unused]] parser_t *pp,
                                       token_t tok, toml_datum_t *ret) {
  *ret = mkdatum(TOML_TIME);
  ret->u.ts.hour = tok.u.tsval.hour;
  ret->u.ts.minute = tok.u.tsval.minute;
  ret->u.ts.second = tok.u.tsval.sec;
  ret->u.ts.usec = tok.u.tsval.usec;
  return 0;
}

// Convert a DATE token to a datum.
[[nodiscard]] static int token_to_date([[maybe_unused]] parser_t *pp,
                                       token_t tok, toml_datum_t *ret) {
  *ret = mkdatum(TOML_DATE);
  ret->u.ts.year = tok.u.tsval.year;
  ret->u.ts.month = tok.u.tsval.month;
  ret->u.ts.day = tok.u.tsval.day;
  return 0;
}

// Convert a DATETIME token to a datum.
[[nodiscard]] static int token_to_datetime([[maybe_unused]] parser_t *pp,
                                           token_t tok, toml_datum_t *ret) {
  *ret = mkdatum(TOML_DATETIME);
  ret->u.ts.year = tok.u.tsval.year;
  ret->u.ts.month = tok.u.tsval.month;
  ret->u.ts.day = tok.u.tsval.day;
  ret->u.ts.hour = tok.u.tsval.hour;
  ret->u.ts.minute = tok.u.tsval.minute;
  ret->u.ts.second = tok.u.tsval.sec;
  ret->u.ts.usec = tok.u.tsval.usec;
  return 0;
}

// Convert a DATETIMETZ token to a datum.
[[nodiscard]] static int token_to_datetimetz([[maybe_unused]] parser_t *pp,
                                             token_t tok, toml_datum_t *ret) {
  *ret = mkdatum(TOML_DATETIMETZ);
  ret->u.ts.year = tok.u.tsval.year;
  ret->u.ts.month = tok.u.tsval.month;
  ret->u.ts.day = tok.u.tsval.day;
  ret->u.ts.hour = tok.u.tsval.hour;
  ret->u.ts.minute = tok.u.tsval.minute;
  ret->u.ts.second = tok.u.tsval.sec;
  ret->u.ts.usec = tok.u.tsval.usec;
  ret->u.ts.tz = tok.u.tsval.tz;
  return 0;
}

// Convert an int64 token to a datum.
[[nodiscard]] static int token_to_int64([[maybe_unused]] parser_t *pp,
                                        token_t tok, toml_datum_t *ret) {
  assert(tok.toktyp == TOK_INTEGER);
  *ret = mkdatum(TOML_INT64);
  ret->u.int64 = tok.u.int64;
  return 0;
}

// Convert a fp64 token to a datum.
[[nodiscard]] static int token_to_fp64([[maybe_unused]] parser_t *pp,
                                       token_t tok, toml_datum_t *ret) {
  assert(tok.toktyp == TOK_FLOAT);
  *ret = mkdatum(TOML_FP64);
  ret->u.fp64 = tok.u.fp64;
  return 0;
}

// Convert a boolean token to a datum.
[[nodiscard]] static int token_to_boolean([[maybe_unused]] parser_t *pp,
                                          token_t tok, toml_datum_t *ret) {
  assert(tok.toktyp == TOK_BOOL);
  *ret = mkdatum(TOML_BOOLEAN);
  ret->u.boolean = tok.u.b1;
  return 0;
}

// Parse a multipart key. Return 0 on success, -1 otherwise.
[[nodiscard]] static int parse_key(parser_t *pp, token_t tok,
                                   keypart_t *ret_keypart) {
  ret_keypart->nspan = 0;
  // key = simple-key | dotted_key
  // simple-key = STRING | LITSTRING | LIT
  // dotted-key = simple-key (DOT simple-key)+
  if (tok.toktyp != TOK_STRING && tok.toktyp != TOK_LITSTRING &&
      tok.toktyp != TOK_LIT) {
    return RETERROR(pp->ebuf, tok.lineno, "missing key");
  }

  int n = 0;
  span_t *kpspan = ret_keypart->span;

  // Normalize the first keypart
  if (parse_norm(pp, tok, &kpspan[n])) {
    return RETERROR(pp->ebuf, tok.lineno,
                    "unable to normalize string; probably a unicode issue");
  }
  n++;

  // Scan and normalize the second to last keypart
  while (1) {
    scanner_state_t mark = scan_mark(&pp->scanner);

    // Eat the dot if it is there
    DO(scan_key(&pp->scanner, &tok));

    // If not a dot, we are done with keyparts.
    if (tok.toktyp != TOK_DOT) {
      scan_restore(&pp->scanner, mark);
      break;
    }

    // Scan the n-th key
    DO(scan_key(&pp->scanner, &tok));

    if (tok.toktyp != TOK_STRING && tok.toktyp != TOK_LITSTRING &&
        tok.toktyp != TOK_LIT) {
      return RETERROR(pp->ebuf, tok.lineno, "expects a string in dotted-key");
    }

    if (n >= KEYPARTMAX) {
      return RETERROR(pp->ebuf, tok.lineno, "too many key parts");
    }

    // Normalize the n-th key.
    DO(parse_norm(pp, tok, &kpspan[n]));
    n++;
  }

  // This key has n parts.
  ret_keypart->nspan = n;
  return 0;
}

// Starting at toptab, descend following keypart[]. If a key does not
// exist in the current table, create a new table entry for the
// key. Returns the final table represented by the key.
static toml_datum_t *descend_keypart(parser_t *pp, int lineno,
                                     toml_datum_t *toptab, keypart_t *keypart,
                                     bool stdtabexpr) {
  toml_datum_t *tab = toptab;  // current tab

  for (int i = 0; i < keypart->nspan; i++) {
    const char *reason;
    // Find the i-th keypart
    int j = tab_find(tab, keypart->span[i]);
    // Not found: add a new (key, tab) pair.
    if (j < 0) {
      toml_datum_t newtab = mkdatum(TOML_TABLE);
      newtab.flag |= stdtabexpr ? FLAG_STDEXPR : 0;
      if (tab_add(tab, keypart->span[i], newtab, &reason)) {
        RETERROR(pp->ebuf, lineno, "%s", reason);
        return nullptr;
      }
      tab = &tab->u.tab.value[tab->u.tab.size - 1];  // descend
      continue;
    }

    // Found: extract the value of the key.
    toml_datum_t *value = &tab->u.tab.value[j];

    // If the value is a table, descend.
    if (value->type == TOML_TABLE) {
      tab = value;  // descend
      continue;
    }

    // If the value is an array: locate the last entry and descend.
    if (value->type == TOML_ARRAY) {
      // If empty: error.
      if (value->u.arr.size <= 0) {
        RETERROR(pp->ebuf, lineno, "array %s has no elements",
                 keypart->span[i].ptr);
        return nullptr;
      }

      // Extract the last element of the array.
      value = &value->u.arr.elem[value->u.arr.size - 1];

      // It must be a table!
      if (value->type != TOML_TABLE) {
        RETERROR(pp->ebuf, lineno, "array %s must be array of tables",
                 keypart->span[i].ptr);
        return nullptr;
      }
      tab = value;  // descend
      continue;
    }

    // key not found
    RETERROR(pp->ebuf, lineno, "cannot locate table at key %s",
             keypart->span[i].ptr);
    return nullptr;
  }

  // Return the table corresponding to the keypart[].
  return tab;
}

// Recursively set flags on datum
static void set_flag_recursive(toml_datum_t *datum, uint32_t flag) {
  datum->flag |= flag;
  switch (datum->type) {
    case TOML_ARRAY:
      for (int i = 0, top = datum->u.arr.size; i < top; i++) {
        set_flag_recursive(&datum->u.arr.elem[i], flag);
      }
      break;
    case TOML_TABLE:
      for (int i = 0, top = datum->u.tab.size; i < top; i++) {
        set_flag_recursive(&datum->u.tab.value[i], flag);
      }
      break;
    default:
      break;
  }
}

// Parse an inline array.
[[nodiscard]] static int parse_inline_array(parser_t *pp, token_t tok,
                                            toml_datum_t *ret_datum) {
  assert(tok.toktyp == TOK_LBRACK);
  *ret_datum = mkdatum(TOML_ARRAY);
  int need_comma = 0;

  // loop until RBRACK
  for (;;) {
    // skip ENDL
    do {
      if (scan_value(&pp->scanner, &tok)) {
        goto bail;
      }
    } while (tok.toktyp == TOK_ENDL);

    // If got an RBRACK: done!
    if (tok.toktyp == TOK_RBRACK) {
      break;
    }

    // If got a COMMA: check if it is expected.
    if (tok.toktyp == TOK_COMMA) {
      if (need_comma) {
        need_comma = 0;
        continue;
      }
      RETERROR(pp->ebuf, tok.lineno,
               "syntax error while parsing array: unexpected comma");
      goto bail;
    }

    // Not a comma, but need a comma: error!
    if (need_comma) {
      RETERROR(pp->ebuf, tok.lineno,
               "syntax error while parsing array: missing comma");
      goto bail;
    }

    // This is a valid value!

    // Add the value to the array.
    const char *reason;
    toml_datum_t *pelem = arr_emplace(ret_datum, &reason);
    if (!pelem) {
      RETERROR(pp->ebuf, tok.lineno, "while parsing array: %s", reason);
      goto bail;
    }

    // Parse the value and save into array.
    if (parse_val(pp, tok, pelem)) {
      goto bail;
    }

    // Need comma before the next value.
    need_comma = 1;
  }

  // Set the INLINE flag for all things in this array.
  set_flag_recursive(ret_datum, FLAG_INLINED);
  return 0;

bail:
  datum_free(ret_datum);
  return -1;
}

// Parse an inline table.
[[nodiscard]] static int parse_inline_table(parser_t *pp, token_t tok,
                                            toml_datum_t *ret_datum) {
  assert(tok.toktyp == TOK_LBRACE);
  *ret_datum = mkdatum(TOML_TABLE);
  bool need_comma = false;
  bool was_comma = false;

  // loop until RBRACE
  for (;;) {
    if (scan_key(&pp->scanner, &tok)) {
      goto bail;
    }

    // Got an RBRACE: done!
    if (tok.toktyp == TOK_RBRACE) {
      if (was_comma) {
        /*
        return RETERROR(pp->ebuf, tok.lineno,
                        "extra comma before closing brace");
        */
        // extra comma before RBRACE is allowed for v1.1
        (void)0;
      }
      break;
    }

    // Got a comma: check if it is expected.
    if (tok.toktyp == TOK_COMMA) {
      if (need_comma) {
        need_comma = false;
        was_comma = true;
        continue;
      }
      RETERROR(pp->ebuf, tok.lineno, "unexpected comma");
      goto bail;
    }

    // Newline not allowed in inline table.
    // newline is allowed in v1.1
    if (tok.toktyp == TOK_ENDL) {
      // return RETERROR(pp->ebuf, tok.lineno, "unexpected newline");
      continue;
    }

    // Not a comma, but need a comma: error!
    if (need_comma) {
      RETERROR(pp->ebuf, tok.lineno, "missing comma");
      goto bail;
    }

    // Get the keyparts
    keypart_t keypart = {0};
    int keylineno = tok.lineno;
    if (parse_key(pp, tok, &keypart)) {
      goto bail;
    }

    // Descend to one keypart before last
    span_t lastkeypart = keypart.span[--keypart.nspan];
    toml_datum_t *tab =
        descend_keypart(pp, keylineno, ret_datum, &keypart, false);
    if (!tab) {
      goto bail;
    }

    // If tab is a previously declared inline table: error.
    if (tab->flag & FLAG_INLINED) {
      RETERROR(pp->ebuf, tok.lineno, "inline table cannot be extended");
      goto bail;
    }

    // We are explicitly defining it now.
    tab->flag |= FLAG_EXPLICIT;

    // match EQUAL
    if (scan_value(&pp->scanner, &tok)) {
      goto bail;
    }

    if (tok.toktyp != TOK_EQUAL) {
      if (tok.toktyp == TOK_ENDL) {
        RETERROR(pp->ebuf, tok.lineno, "unexpected newline");
      } else {
        RETERROR(pp->ebuf, tok.lineno, "missing '='");
      }
      goto bail;
    }

    // obtain the value
    toml_datum_t value = DATUM_ZERO;
    if (scan_value(&pp->scanner, &tok)) {
      goto bail;
    }
    if (parse_val(pp, tok, &value)) {
      datum_free(&value);
      goto bail;
    }

    // Add the value to tab.
    const char *reason;
    if (tab_add(tab, lastkeypart, value, &reason)) {
      datum_free(&value);
      RETERROR(pp->ebuf, tok.lineno, "%s", reason);
      goto bail;
    }
    need_comma = true;
    was_comma = false;
  }

  set_flag_recursive(ret_datum, FLAG_INLINED);
  return 0;

bail:
  datum_free(ret_datum);
  return -1;
}

// Parse a value.
[[nodiscard]] static int parse_val(parser_t *pp, token_t tok,
                                   toml_datum_t *ret) {
  // val = string / boolean / array / inline-table / date-time / float / integer
  switch (tok.toktyp) {
    case TOK_STRING:
    case TOK_MLSTRING:
    case TOK_LITSTRING:
    case TOK_MLLITSTRING:
      return token_to_string(pp, tok, ret);
    case TOK_TIME:
      return token_to_time(pp, tok, ret);
    case TOK_DATE:
      return token_to_date(pp, tok, ret);
    case TOK_DATETIME:
      return token_to_datetime(pp, tok, ret);
    case TOK_DATETIMETZ:
      return token_to_datetimetz(pp, tok, ret);
    case TOK_INTEGER:
      return token_to_int64(pp, tok, ret);
    case TOK_FLOAT:
      return token_to_fp64(pp, tok, ret);
    case TOK_BOOL:
      return token_to_boolean(pp, tok, ret);
    case TOK_LBRACK:  // inline-array
      return parse_inline_array(pp, tok, ret);
    case TOK_LBRACE:  // inline-table
      return parse_inline_table(pp, tok, ret);
    default:
      break;
  }
  return RETERROR(pp->ebuf, tok.lineno, "missing value");
}

// Parse a standard table expression, and set the curtab of the parser
// to the table referenced.  A standard table expression is a line
// like [a.b.c.d].
[[nodiscard]] static int parse_std_table_expr(parser_t *pp, token_t tok) {
  // std-table = [ key ]
  // Eat the [
  assert(tok.toktyp == TOK_LBRACK);  // [ ate by caller

  // Read the first keypart
  DO(scan_key(&pp->scanner, &tok));

  // Extract the keypart[]
  int keylineno = tok.lineno;
  keypart_t keypart;
  DO(parse_key(pp, tok, &keypart));

  // Eat the ]
  DO(scan_key(&pp->scanner, &tok));
  if (tok.toktyp != TOK_RBRACK) {
    return RETERROR(pp->ebuf, tok.lineno, "missing right-bracket");
  }

  // Descend to one keypart before last.
  span_t lastkeypart = keypart.span[--keypart.nspan];

  // Descend keypart from the toptab.
  toml_datum_t *tab =
      descend_keypart(pp, keylineno, &pp->toptab, &keypart, true);
  if (!tab) {
    return -1;
  }

  // Look for the last keypart in the final tab
  int j = tab_find(tab, lastkeypart);
  if (j < 0) {
    // If not found: add it.
    if (tab->flag & FLAG_INLINED) {
      return RETERROR(pp->ebuf, keylineno, "inline table cannot be extended");
    }
    const char *reason;
    toml_datum_t newtab = mkdatum(TOML_TABLE);
    newtab.flag |= FLAG_STDEXPR;
    if (tab_add(tab, lastkeypart, newtab, &reason)) {
      return RETERROR(pp->ebuf, keylineno, "%s", reason);
    }
    // this is the new tab
    tab = &tab->u.tab.value[tab->u.tab.size - 1];
  } else {
    // Found: check for errors
    tab = &tab->u.tab.value[j];
    if (tab->flag & FLAG_EXPLICIT) {
      /*
        This is not OK:
        [x.y.z]
        [x.y.z]

        but this is OK:
        [x.y.z]
        [x]
      */
      return RETERROR(pp->ebuf, keylineno, "table defined more than once");
    }
    if (!(tab->flag & FLAG_STDEXPR)) {
      /*
      [t1]			# OK
      t2.t3.v = 0		# OK
      [t1.t2]   		# should FAIL  - t2 was non-explicit but was not
      created by std-table-expr
      */
      return RETERROR(pp->ebuf, keylineno, "table defined before");
    }
  }

  // Set explicit flag on tab
  tab->flag |= FLAG_EXPLICIT;

  // Set tab as curtab of the parser
  pp->curtab = tab;
  return 0;
}

// Parse an array table expression, and set the curtab of the parser
// to the table referenced. A standard array table expresison is a line
// like [[a.b.c.d]].
[[nodiscard]] static int parse_array_table_expr(parser_t *pp, token_t tok) {
  // array-table = [[ key ]]
  assert(tok.toktyp == TOK_LLBRACK);  // [[ ate by caller

  // Read the first keypart
  DO(scan_key(&pp->scanner, &tok));

  int keylineno = tok.lineno;
  keypart_t keypart;
  DO(parse_key(pp, tok, &keypart));

  // eat the ]]
  token_t rrb;
  DO(scan_key(&pp->scanner, &rrb));
  if (rrb.toktyp != TOK_RRBRACK) {
    return RETERROR(pp->ebuf, rrb.lineno, "missing ']]'");
  }

  // remove the last keypart from keypart[]
  span_t lastkeypart = keypart.span[--keypart.nspan];

  // descend the key from the toptab
  toml_datum_t *tab = &pp->toptab;
  for (int i = 0; i < keypart.nspan; i++) {
    span_t curkey = keypart.span[i];
    int j = tab_find(tab, curkey);
    if (j < 0) {
      // If not found: add a new (key,tab) pair
      const char *reason;
      toml_datum_t newtab = mkdatum(TOML_TABLE);
      newtab.flag |= FLAG_STDEXPR;
      if (tab_add(tab, curkey, newtab, &reason)) {
        return RETERROR(pp->ebuf, keylineno, "%s", reason);
      }
      tab = &tab->u.tab.value[tab->u.tab.size - 1];
      continue;
    }

    // Found: get the value
    toml_datum_t *value = &tab->u.tab.value[j];

    // If value is table, then point to that table and continue descent.
    if (value->type == TOML_TABLE) {
      tab = value;
      continue;
    }

    // If value is an array of table, point to the last element of the array and
    // continue descent.
    if (value->type == TOML_ARRAY) {
      if (value->flag & FLAG_INLINED) {
        return RETERROR(pp->ebuf, keylineno, "cannot expand array %s",
                        curkey.ptr);
      }
      if (value->u.arr.size <= 0) {
        return RETERROR(pp->ebuf, keylineno, "array %s has no elements",
                        curkey.ptr);
      }
      value = &value->u.arr.elem[value->u.arr.size - 1];
      if (value->type != TOML_TABLE) {
        return RETERROR(pp->ebuf, keylineno, "array %s must be array of tables",
                        curkey.ptr);
      }
      tab = value;
      continue;
    }

    // keypart not found
    return RETERROR(pp->ebuf, keylineno, "cannot locate table at key %s",
                    curkey.ptr);
  }

  // For the final keypart, make sure entry at key is an array of tables
  const char *reason;
  int idx = tab_find(tab, lastkeypart);
  if (idx == -1) {
    // If not found, add an array of table.
    if (tab_add(tab, lastkeypart, mkdatum(TOML_ARRAY), &reason)) {
      return RETERROR(pp->ebuf, keylineno, "%s", reason);
    }
    idx = tab_find(tab, lastkeypart);
    assert(idx >= 0);
  }
  // Check that this is an array.
  if (tab->u.tab.value[idx].type != TOML_ARRAY) {
    return RETERROR(pp->ebuf, keylineno, "entry must be an array");
  }
  // Add an empty table to the array
  toml_datum_t *arr = &tab->u.tab.value[idx];
  if (arr->flag & FLAG_INLINED) {
    return RETERROR(pp->ebuf, keylineno, "cannot extend a static array");
  }
  toml_datum_t *pelem = arr_emplace(arr, &reason);
  if (!pelem) {
    return RETERROR(pp->ebuf, keylineno, "%s", reason);
  }
  *pelem = mkdatum(TOML_TABLE);

  // Set the last element of this array as curtab of the parser
  pp->curtab = &arr->u.arr.elem[arr->u.arr.size - 1];
  assert(pp->curtab->type == TOML_TABLE);

  return 0;
}

// Parse an expression. A toml doc is just a list of expressions.
[[nodiscard]] static int parse_keyvalue_expr(parser_t *pp, token_t tok) {
  // Obtain the key
  int keylineno = tok.lineno;
  keypart_t keypart;
  DO(parse_key(pp, tok, &keypart));

  // match the '='
  DO(scan_key(&pp->scanner, &tok));
  if (tok.toktyp != TOK_EQUAL) {
    return RETERROR(pp->ebuf, tok.lineno, "expect '='");
  }

  // Obtain the value
  toml_datum_t val = DATUM_ZERO;
  bool has_val = false;
  DO(scan_value(&pp->scanner, &tok));
  if (parse_val(pp, tok, &val)) {
    goto bail;
  }
  has_val = true;

  // Locate the last table using keypart[]
  const char *reason;
  toml_datum_t *tab = pp->curtab;
  for (int i = 0; i < keypart.nspan - 1; i++) {
    int j = tab_find(tab, keypart.span[i]);
    if (j < 0) {
      if (i > 0 && (tab->flag & FLAG_EXPLICIT)) {
        RETERROR(
            pp->ebuf, keylineno,
            "cannot extend a previously defined table using dotted expression");
        goto bail;
      }
      toml_datum_t newtab = mkdatum(TOML_TABLE);
      if (tab_add(tab, keypart.span[i], newtab, &reason)) {
        RETERROR(pp->ebuf, keylineno, "%s", reason);
        goto bail;
      }
      tab = &tab->u.tab.value[tab->u.tab.size - 1];
      continue;
    }
    toml_datum_t *value = &tab->u.tab.value[j];
    if (value->type == TOML_TABLE) {
      tab = value;
      continue;
    }
    if (value->type == TOML_ARRAY) {
      RETERROR(pp->ebuf, keylineno,
               "encountered previously declared array '%s'",
               keypart.span[i].ptr);
      goto bail;
    }
    RETERROR(pp->ebuf, keylineno, "cannot locate table at '%s'",
             keypart.span[i].ptr);
    goto bail;
  }

  // Check for disallowed situations.
  if (tab->flag & FLAG_INLINED) {
    RETERROR(pp->ebuf, keylineno, "inline table cannot be extended");
    goto bail;
  }
  if (keypart.nspan > 1 && (tab->flag & FLAG_EXPLICIT)) {
    RETERROR(
        pp->ebuf, keylineno,
        "cannot extend a previously defined table using dotted expression");
    goto bail;
  }

  // Add a new key/value for tab.
  if (tab_add(tab, keypart.span[keypart.nspan - 1], val, &reason)) {
    RETERROR(pp->ebuf, keylineno, "%s", reason);
    goto bail;
  }
  has_val = false;  // ownership moved to tab

  return 0;

bail:
  if (has_val) {
    datum_free(&val);
  }
  return -1;
}

// Normalize a LIT/STRING/MLSTRING/LITSTRING/MLLITSTRING
// -> unescape all escaped chars
// The returned string is allocated out of pp->sbuf[]
[[nodiscard]] static int parse_norm(parser_t *pp, token_t tok,
                                    span_t *ret_span) {
  // Allocate a buffer to store the normalized string. Add one
  // extra-byte for terminating NUL.
  char *p = pool_alloc(pp->pool, tok.str.len + 1);
  if (!p) {
    return RETERROR(pp->ebuf, tok.lineno, "out of memory");
  }

  // Copy from token string into buffer
  memcpy(p, tok.str.ptr, tok.str.len);
  p[tok.str.len] = 0;  // additional NUL term for safety

  ret_span->ptr = p;
  ret_span->len = tok.str.len;

  switch (tok.toktyp) {
    case TOK_LIT:
    case TOK_LITSTRING:
    case TOK_MLLITSTRING:
      // no need to handle escape chars
      return 0;

    case TOK_STRING:
    case TOK_MLSTRING:
      // need to handle escape chars
      break;

    default:
      return RETERROR(pp->ebuf, 0, "internal: arg must be a string");
  }

  // if there is no escape char, then done!
  p = memchr(ret_span->ptr, '\\', ret_span->len);
  if (!p) {
    return 0;  // success
  }

  // Normalize the escaped chars
  char *dst = p;
  while (*p) {
    if (*p != '\\') {
      *dst++ = *p++;
      continue;
    }
    switch (p[1]) {
      case '"':
      case '\\':
        *dst++ = p[1];
        p += 2;
        continue;
      case 'b':
        *dst++ = '\b';
        p += 2;
        continue;
      case 't':
        *dst++ = '\t';
        p += 2;
        continue;
      case 'n':
        *dst++ = '\n';
        p += 2;
        continue;
      case 'f':
        *dst++ = '\f';
        p += 2;
        continue;
      case 'r':
        *dst++ = '\r';
        p += 2;
        continue;
      case 'e':
        *dst++ = '\x1b';
        p += 2;
        continue;
      case 'x': {
        char buf[3];
        memcpy(buf, p + 2, 2);
        buf[2] = 0;
        auto ucs = (uint32_t)strtoul(buf, nullptr, 16);
        int n = ucs_to_utf8(ucs, dst);
        if (n < 0) {
          return RETERROR(pp->ebuf, tok.lineno,
                          "error converting UCS %s to UTF8", buf);
        }
        dst += n;
        p += 4;
        continue;
      }
      case 'u':
      case 'U': {
        char buf[9];
        int sz = (p[1] == 'u' ? 4 : 8);
        memcpy(buf, p + 2, sz);
        buf[sz] = 0;
        auto ucs = (uint32_t)strtoul(buf, nullptr, 16);
        if (!is_unicode_scalar(ucs)) {
          return RETERROR(pp->ebuf, tok.lineno, "invalid UTF8 char \\u%04x",
                          ucs);
        }
        int n = ucs_to_utf8(ucs, dst);
        if (n < 0) {
          return RETERROR(pp->ebuf, tok.lineno,
                          "error converting UCS %s to UTF8", buf);
        }
        dst += n;
        p += 2 + sz;
        continue;
      }

      case ' ':
      case '\t':
      case '\r':
        // line-ending backslash
        // --- allow for extra whitespace chars after backslash
        // --- skip until newline
        p++;
        p += strspn(p, " \t\r");
        if (*p != '\n') {
          return RETERROR(pp->ebuf, tok.lineno, "internal error");
        }
        // fallthru
      case '\n':
        // skip all whitespaces including newline
        p++;
        p += strspn(p, " \t\r\n");
        continue;
      default:
        *dst++ = *p++;
        continue;
    }
  }
  *dst = 0;
  ret_span->len = dst - ret_span->ptr;
  return 0;
}

// -------------- scanner functions

// Get the next char
static int scan_get(scanner_t *sp) {
  int ret = TOK_FIN;
  const char *p = sp->cur;
  if (p < sp->endp) {
    ret = *p++;
    if (ret == '\r' && p < sp->endp && *p == '\n') {
      ret = *p++;
    }
  }
  sp->cur = p;
  sp->lineno += (ret == '\n' ? 1 : 0);
  return ret;
}

// Check if the next char matches ch.
static inline bool scan_match(scanner_t *sp, int ch) {
  const char *p = sp->cur;
  if (p < sp->endp && *p == ch) {
    return true;
  }
  if (ch == '\n' && p + 1 < sp->endp) {
    return p[0] == '\r' && p[1] == '\n';
  }
  return false;
}

// Check if the next char is in accept[].
static bool scan_matchany(scanner_t *sp, const char *accept) {
  for (; *accept; accept++) {
    if (scan_match(sp, *accept)) {
      return true;
    }
  }
  return false;
}

// Check if the next n chars match ch.
static inline bool scan_nmatch(scanner_t *sp, int ch, int n) {
  assert(ch != '\n');  // not handled
  if (sp->cur + n > sp->endp) {
    return false;
  }
  const char *p = sp->cur;
  int i;
  for (i = 0; i < n && p[i] == ch; i++);
  return i == n;
}

// Initialize a token.
static inline token_t mktoken(scanner_t *sp, toktyp_t typ) {
  token_t tok = {0};
  tok.toktyp = typ;
  tok.str.ptr = sp->cur;
  tok.lineno = sp->lineno;
  return tok;
}

#define S_GET() scan_get(sp)
#define S_MATCH(ch) scan_match(sp, (ch))
#define S_MATCH3(ch) scan_nmatch(sp, (ch), 3)
#define S_MATCH4(ch) scan_nmatch(sp, (ch), 4)
#define S_MATCH6(ch) scan_nmatch(sp, (ch), 6)

static inline bool is_valid_char(int ch) {
  if (ch < 0) {
    return false;
  }
  auto uch = (unsigned char)ch;
  return (0x20 <= uch && uch <= 0x7e) || (uch & 0x80);
}

static inline bool is_hex_char(int ch) {
  ch = to_ascii_upper(ch);
  return ('0' <= ch && ch <= '9') || ('A' <= ch && ch <= 'F');
}

// Initialize a scanner
static void scan_init(scanner_t *sp, const char *src, int len, char *errbuf,
                      int errbufsz) {
  memset(sp, 0, sizeof(*sp));
  sp->src = src;
  sp->endp = src + len;
  assert(*sp->endp == '\0');
  sp->cur = src;
  sp->lineno = 1;
  sp->ebuf.ptr = errbuf;
  sp->ebuf.len = errbufsz;
}

[[nodiscard]] static int scan_multiline_string(scanner_t *sp, token_t *tok) {
  assert(S_MATCH3('"'));
  S_GET(), S_GET(), S_GET();  // skip opening """

  // According to spec: trim first newline after """
  if (S_MATCH('\n')) {
    S_GET();
  }

  *tok = mktoken(sp, TOK_MLSTRING);
  // scan until terminating """
  while (1) {
    if (S_MATCH3('"')) {
      if (S_MATCH4('"')) {
        // special case... """abcd """" -> (abcd ")
        // but sequences of 3 or more double quotes are not allowed
        if (S_MATCH6('"')) {
          return RETERROR(sp->ebuf, sp->lineno,
                          "detected sequences of 3 or more double quotes");
        } else {
          ;  // no problem
        }
      } else {
        break;  // found terminating """
      }
    }
    int ch = S_GET();
    if (ch == TOK_FIN) {
      return RETERROR(sp->ebuf, sp->lineno, "unterminated \"\"\"");
    }
    // If non-escaped char ...
    if (ch != '\\') {
      if (!(is_valid_char(ch) || (ch && strchr(" \t\n", ch)))) {
        return RETERROR(sp->ebuf, sp->lineno, "invalid char in string");
      }
      continue;
    }
    // ch is backslash; handle escape char
    ch = S_GET();
    if (ch && strchr("btnfre\"\\", ch)) {
      // skip \", \\, \b, \f, \n, \r, \t
      continue;
    }
    int top = 0;
    switch (ch) {
      case 'x':
        top = 2;
        break;
      case 'u':
        top = 4;
        break;
      case 'U':
        top = 8;
        break;
      default:
        break;
    }
    if (top) {
      for (int i = 0; i < top; i++) {
        if (!is_hex_char(S_GET())) {
          return RETERROR(sp->ebuf, sp->lineno,
                          "expect %d hex digits after \\%c", top, ch);
        }
      }
      continue;
    }
    // handle line-ending backslash
    if (ch == ' ' || ch == '\t') {
      // Although the spec does not allow for whitespace following a
      // line-ending backslash, some standard tests expect it.
      // Skip whitespace till EOL.
      while (ch != TOK_FIN && ch && strchr(" \t", ch)) {
        ch = S_GET();
      }
      if (ch != '\n') {
        // Got a backslash followed by whitespace, followed by some char
        // before newline
        return RETERROR(sp->ebuf, sp->lineno, "bad escape char in string");
      }
      // fallthru
    }
    if (ch == '\n') {
      // got a line-ending backslash
      // - skip all whitespaces
      while (scan_matchany(sp, " \t\n")) {
        S_GET();
      }
      continue;
    }
    return RETERROR(sp->ebuf, sp->lineno, "bad escape char in string");
  }
  tok->str.len = sp->cur - tok->str.ptr;

  assert(S_MATCH3('"'));
  S_GET(), S_GET(), S_GET();
  return 0;
}

[[nodiscard]] static int scan_string(scanner_t *sp, token_t *tok) {
  assert(S_MATCH('"'));
  if (S_MATCH3('"')) {
    return scan_multiline_string(sp, tok);
  }
  S_GET();  // skip opening "

  // scan until closing "
  *tok = mktoken(sp, TOK_STRING);
  while (!S_MATCH('"')) {
    int ch = S_GET();
    if (ch == TOK_FIN) {
      return RETERROR(sp->ebuf, sp->lineno, "unterminated string");
    }
    // If non-escaped char ...
    if (ch != '\\') {
      if (!(is_valid_char(ch) || ch == ' ' || ch == '\t')) {
        return RETERROR(sp->ebuf, sp->lineno, "invalid char in string");
      }
      continue;
    }
    // ch is backslash; handle escape char
    ch = S_GET();
    if (ch && strchr("btnfre\"\\", ch)) {
      // skip \b, \t, \n, \f, \r, \e, \", \\  .
      continue;
    }
    int top = 0;
    switch (ch) {
      case 'x':
        top = 2;
        break;
      case 'u':
        top = 4;
        break;
      case 'U':
        top = 8;
        break;
      default:
        return RETERROR(sp->ebuf, sp->lineno, "bad escape char in string");
    }
    for (int i = 0; i < top; i++) {
      if (!is_hex_char(S_GET())) {
        return RETERROR(sp->ebuf, sp->lineno, "expect %d hex digits after \\%c",
                        top, ch);
      }
    }
  }
  tok->str.len = sp->cur - tok->str.ptr;

  assert(S_MATCH('"'));
  S_GET();  // skip the terminating "
  return 0;
}

[[nodiscard]] static int scan_multiline_litstring(scanner_t *sp, token_t *tok) {
  assert(S_MATCH3('\''));
  S_GET(), S_GET(), S_GET();  // skip opening '''

  // According to spec: trim first newline after '''
  if (S_MATCH('\n')) {
    S_GET();
  }

  // scan until terminating '''
  *tok = mktoken(sp, TOK_MLLITSTRING);
  while (1) {
    if (S_MATCH3('\'')) {
      if (S_MATCH4('\'')) {
        // special case... '''abcd '''' -> (abcd ')
        // but sequences of 3 or more single quotes are not allowed
        if (S_MATCH6('\'')) {
          return RETERROR(sp->ebuf, sp->lineno,
                          "sequences of 3 or more single quotes");
        } else {
          ;  // no problem
        }
      } else {
        break;  // found terminating '''
      }
    }
    int ch = S_GET();
    if (ch == TOK_FIN) {
      return RETERROR(sp->ebuf, sp->lineno,
                      "unterminated multiline lit string");
    }
    if (!(is_valid_char(ch) || (ch && strchr(" \t\n", ch)))) {
      return RETERROR(sp->ebuf, sp->lineno, "invalid char in string");
    }
  }
  tok->str.len = sp->cur - tok->str.ptr;

  assert(S_MATCH3('\''));
  S_GET(), S_GET(), S_GET();
  return 0;
}

[[nodiscard]] static int scan_litstring(scanner_t *sp, token_t *tok) {
  assert(S_MATCH('\''));
  if (S_MATCH3('\'')) {
    return scan_multiline_litstring(sp, tok);
  }
  S_GET();  // skip opening '

  // scan until closing '
  *tok = mktoken(sp, TOK_LITSTRING);
  while (!S_MATCH('\'')) {
    int ch = S_GET();
    if (ch == TOK_FIN) {
      return RETERROR(sp->ebuf, sp->lineno, "unterminated string");
    }
    if (!(is_valid_char(ch) || ch == '\t')) {
      return RETERROR(sp->ebuf, sp->lineno, "invalid char in string");
    }
  }
  tok->str.len = sp->cur - tok->str.ptr;
  assert(S_MATCH('\''));
  S_GET();
  return 0;
}

static bool is_valid_date(int year, int month, int day) {
  if (!(1 <= year)) {
    return false;
  }
  if (!(1 <= month && month <= 12)) {
    return false;
  }
  int is_leap_year = (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
  int days_in_month[] = {
      31, 28 + is_leap_year, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  return (1 <= day && day <= days_in_month[month - 1]);
}

static bool is_valid_time(int hour, int minute, int sec, int usec) {
  if (!(0 <= hour && hour <= 23)) {
    return false;
  }
  if (!(0 <= minute && minute <= 59)) {
    return false;
  }
  if (!(0 <= sec && sec <= 59)) {
    return false;
  }
  if (!(0 <= usec)) {
    return false;
  }
  return true;
}

static bool is_valid_timezone(int minute) {
  minute = (minute < 0 ? -minute : minute);
  int hour = minute / 60;
  minute = minute % 60;
  if (!(0 <= hour && hour <= 23)) {
    return false;
  }
  if (!(0 <= minute && minute < 60)) {
    return false;
  }
  return true;
}

// Read an int (without signs) from the string p.
[[nodiscard]] static int read_int(const char *p, int *ret) {
  const char *pp = p;
  int val = 0;
  for (; is_ascii_digit(*p); p++) {
    int digit = *p - '0';
    if (val > (INT_MAX - digit) / 10) {
      return 0;  // overflowed
    }
    val = val * 10 + digit;
  }
  *ret = val;
  return p - pp;
}

// Read a date as YYYY-MM-DD from p[]. Return #bytes consumed.
[[nodiscard]] static int read_date(const char *p, int *year, int *month,
                                   int *day) {
  const char *pp = p;
  int n;
  n = read_int(p, year);
  if (n != 4 || p[4] != '-') {
    return 0;
  }
  n = read_int(p += n + 1, month);
  if (n != 2 || p[2] != '-') {
    return 0;
  }
  n = read_int(p += n + 1, day);
  if (n != 2) {
    return 0;
  }
  p += 2;
  assert(p - pp == 10);
  return p - pp;
}

// Read a time as HH:MM:SS.subsec from p[]. Return #bytes consumed.
[[nodiscard]] static int read_time(const char *p, int *hour, int *minute,
                                   int *second, int *usec) {
  const char *pp = p;
  int n;
  *hour = *minute = *second = *usec = 0;
  // scan hours
  n = read_int(p, hour);
  if (n != 2 || p[2] != ':') {
    return 0;
  }
  p += 3;

  // scan minutes
  n = read_int(p, minute);
  if (n != 2) {
    return 0;
  }
  if (p[2] != ':') {
    // seconds are optional in v1.1
    p += 2;
    return p - pp;
  }
  p += 3;

  // scan seconds
  n = read_int(p, second);
  if (n != 2) {
    return 0;
  }
  p += 2;

  if (*p != '.') {
    return p - pp;
  }
  p++;  // skip the period
  if (!is_ascii_digit(*p)) {
    // trailing period
    return 0;
  }
  int micro_factor = 100000;
  while (is_ascii_digit(*p) && micro_factor) {
    *usec += (*p - '0') * micro_factor;
    micro_factor /= 10;
    p++;
  }
  return p - pp;
}

// Reads a timezone from p[]. Return #bytes consumed.
[[nodiscard]] static int read_tzone(const char *p, char *tzsign, int *tzhour,
                                    int *tzminute) {
  const char *pp = p;
  *tzhour = *tzminute = 0;
  *tzsign = '+';
  // look for Zulu
  if (*p == 'Z' || *p == 'z') {
    return 1;
  }

  *tzsign = *p++;
  if (!(*tzsign == '+' || *tzsign == '-')) {
    return 0;
  }

  // look for HH:MM
  int n;
  n = read_int(p, tzhour);
  if (n != 2 || p[2] != ':') {
    return 0;
  }
  n = read_int(p += 3, tzminute);
  if (n != 2) {
    return 0;
  }
  p += 2;
  return p - pp;
}

[[nodiscard]] static int scan_time(scanner_t *sp, token_t *tok) {
  int lineno = sp->lineno;
  char buffer[20];
  int len = sp->endp - sp->cur;
  if (len >= (int)sizeof(buffer)) {
    len = sizeof(buffer) - 1;
  }
  memcpy(buffer, sp->cur, len);
  buffer[len] = 0;  // NUL

  char *p = buffer;
  int hour, minute, sec, usec;
  len = read_time(p, &hour, &minute, &sec, &usec);
  if (len == 0) {
    return RETERROR(sp->ebuf, lineno, "invalid time");
  }
  if (!is_valid_time(hour, minute, sec, usec)) {
    return RETERROR(sp->ebuf, lineno, "invalid time");
  }

  *tok = mktoken(sp, TOK_TIME);
  tok->str.len = len;
  sp->cur += len;
  tok->u.tsval.year = -1;
  tok->u.tsval.month = -1;
  tok->u.tsval.day = -1;
  tok->u.tsval.hour = hour;
  tok->u.tsval.minute = minute;
  tok->u.tsval.sec = sec;
  tok->u.tsval.usec = usec;
  tok->u.tsval.tz = -1;
  return 0;
}

[[nodiscard]] static int scan_timestamp(scanner_t *sp, token_t *tok) {
  int year, month, day, hour, minute, sec, usec, tz;
  int n;
  // make a copy of sp->cur into buffer to ensure NUL terminated string
  char buffer[80];
  int len = sp->endp - sp->cur;
  if (len >= (int)sizeof(buffer)) {
    len = sizeof(buffer) - 1;
  }
  memcpy(buffer, sp->cur, len);
  buffer[len] = 0;  // NUL

  toktyp_t toktyp = TOK_FIN;
  int lineno = sp->lineno;
  const char *p = buffer;
  if (is_ascii_digit(p[0]) && is_ascii_digit(p[1]) && p[2] == ':') {
    year = month = day = hour = minute = sec = usec = tz = -1;
    n = read_time(buffer, &hour, &minute, &sec, &usec);
    if (!n) {
      return RETERROR(sp->ebuf, lineno, "invalid time");
    }
    toktyp = TOK_TIME;
    p += n;
    goto done;
  }

  year = month = day = hour = minute = sec = usec = tz = -1;
  n = read_date(p, &year, &month, &day);
  if (!n) {
    return RETERROR(sp->ebuf, lineno, "invalid date");
  }
  toktyp = TOK_DATE;
  p += n;
  if (!((p[0] == 'T' || p[0] == ' ' || p[0] == 't') && is_ascii_digit(p[1]) &&
        is_ascii_digit(p[2]) && p[3] == ':')) {
    goto done;  // date only
  }

  n = read_time(p += 1, &hour, &minute, &sec, &usec);
  if (!n) {
    return RETERROR(sp->ebuf, lineno, "invalid timestamp");
  }
  toktyp = TOK_DATETIME;
  p += n;
  char tzsign;
  int tzhour, tzminute;
  n = read_tzone(p, &tzsign, &tzhour, &tzminute);
  if (n == 0) {
    goto done;  // datetime only
  }
  toktyp = TOK_DATETIMETZ;
  p += n;
  if (!(0 <= tzminute && tzminute < 60)) {
    return RETERROR(sp->ebuf, lineno, "invalid timezone");
  }
  tz = (tzhour * 60 + tzminute) * (tzsign == '-' ? -1 : 1);
  goto done;  // datetimetz

done:
  *tok = mktoken(sp, toktyp);
  n = p - buffer;
  tok->str.len = n;
  sp->cur += n;

  tok->u.tsval.year = year;
  tok->u.tsval.month = month;
  tok->u.tsval.day = day;
  tok->u.tsval.hour = hour;
  tok->u.tsval.minute = minute;
  tok->u.tsval.sec = sec;
  tok->u.tsval.usec = usec;
  tok->u.tsval.tz = tz;

  switch (tok->toktyp) {
    case TOK_TIME:
      if (!is_valid_time(hour, minute, sec, usec)) {
        return RETERROR(sp->ebuf, lineno, "invalid time");
      }
      break;
    case TOK_DATE:
      if (!is_valid_date(year, month, day)) {
        return RETERROR(sp->ebuf, lineno, "invalid date");
      }
      break;
    case TOK_DATETIME:
    case TOK_DATETIMETZ:
      if (!is_valid_date(year, month, day)) {
        return RETERROR(sp->ebuf, lineno, "invalid date");
      }
      if (!is_valid_time(hour, minute, sec, usec)) {
        return RETERROR(sp->ebuf, lineno, "invalid time");
      }
      if (tok->toktyp == TOK_DATETIMETZ && !is_valid_timezone(tz)) {
        return RETERROR(sp->ebuf, lineno, "invalid timezone");
      }
      break;
    default:
      assert(0);
      return RETERROR(sp->ebuf, lineno, "internal error");
  }

  return 0;
}

[[nodiscard]] static int process_numstr(char *buffer, int base,
                                        const char **reason) {
  // squeeze out _
  char *q = strchr(buffer, '_');
  if (q) {
    for (int i = q - buffer; buffer[i]; i++) {
      if (buffer[i] != '_') {
        *q++ = buffer[i];
        continue;
      }
      int left = (i == 0) ? 0 : buffer[i - 1];
      int right = buffer[i + 1];
      if (!is_ascii_digit(left) && !(base == 16 && is_hex_char(left))) {
        *reason = "underscore only allowed between digits";
        return -1;
      }
      if (!is_ascii_digit(right) && !(base == 16 && is_hex_char(right))) {
        *reason = "underscore only allowed between digits";
        return -1;
      }
    }
    *q = 0;
  }

  // decimal points must be surrounded by digits. Also, convert to lowercase.
  for (int i = 0; buffer[i]; i++) {
    if (buffer[i] == '.') {
      if (i == 0 || !is_ascii_digit(buffer[i - 1]) ||
          !is_ascii_digit(buffer[i + 1])) {
        *reason = "decimal point must be surrounded by digits";
        return -1;
      }
    } else if ('A' <= buffer[i] && buffer[i] <= 'Z') {
      buffer[i] = (char)to_ascii_lower(buffer[i]);
    }
  }

  if (base == 10) {
    // check for leading 0:  '+01' is an error!
    q = buffer;
    q += (*q == '+' || *q == '-') ? 1 : 0;
    if (q[0] == '0' && is_ascii_digit(q[1])) {
      *reason = "leading 0 in numbers";
      return -1;
    }

    // 1e+01 is also an error
    if (0 != (q = strchr(buffer, 'e'))) {
      q++;  // skip 'e'
      q += (*q == '+' || *q == '-') ? 1 : 0;
      if (q[0] == '0' && is_ascii_digit(q[1])) {
        *reason = "leading 0 in numbers";
        return -1;
      }
    }
  }

  return 0;
}

[[nodiscard]] static int scan_float(scanner_t *sp, token_t *tok) {
  char buffer[50];  // need to accomodate "9_007_199_254_740_991.0"
  int len = sp->endp - sp->cur;
  if (len >= (int)sizeof(buffer)) {
    len = sizeof(buffer) - 1;
  }
  memcpy(buffer, sp->cur, len);
  buffer[len] = 0;  // NUL

  int lineno = sp->lineno;
  char *p = buffer;
  p += (*p == '+' || *p == '-') ? 1 : 0;
  if (0 == memcmp(p, "nan", 3) || (0 == memcmp(p, "inf", 3))) {
    p += 3;
  } else {
    p += strspn(p, "_0123456789eE.+-");
  }
  len = p - buffer;
  buffer[len] = 0;

  const char *reason;
  if (process_numstr(buffer, 10, &reason)) {
    return RETERROR(sp->ebuf, lineno, reason);
  }

  errno = 0;
  char *q;
  double fp64 = strtod(buffer, &q);
  if (errno || *q || q == buffer) {
    return RETERROR(sp->ebuf, lineno, "error parsing float");
  }

  *tok = mktoken(sp, TOK_FLOAT);
  tok->u.fp64 = fp64;
  tok->str.len = len;
  sp->cur += len;
  return 0;
}

[[nodiscard]] static int scan_number(scanner_t *sp, token_t *tok) {
  const char *reason;
  char buffer[50];  // need to accomodate "9_007_199_254_740_991.0"
  int len = sp->endp - sp->cur;
  if (len >= (int)sizeof(buffer)) {
    len = sizeof(buffer) - 1;
  }
  memcpy(buffer, sp->cur, len);
  buffer[len] = 0;  // NUL

  char *p = buffer;
  char *q = buffer + len;
  int lineno = sp->lineno;
  // process %0x, %0o or %0b integers
  if (p[0] == '0') {
    const char *span = nullptr;
    int base = 0;
    switch (p[1]) {
      case 'x':
        base = 16;
        span = "_0123456789abcdefABCDEF";
        break;
      case 'o':
        base = 8;
        span = "_01234567";
        break;
      case 'b':
        base = 2;
        span = "_01";
        break;
    }
    if (base) {
      p += 2;
      p += strspn(p, span);
      len = p - buffer;
      buffer[len] = 0;

      if (process_numstr(buffer + 2, base, &reason)) {
        return RETERROR(sp->ebuf, lineno, reason);
      }

      // use strtoll to obtain the value
      *tok = mktoken(sp, TOK_INTEGER);
      errno = 0;
      tok->u.int64 = strtoll(buffer + 2, &q, base);
      if (errno || *q || q == buffer + 2) {
        return RETERROR(sp->ebuf, lineno, "error parsing integer");
      }
      tok->str.len = len;
      sp->cur += len;
      return 0;
    }
  }

  // handle inf/nan
  if (*p == '+' || *p == '-') {
    p++;
  }
  if (*p == 'i' || *p == 'n') {
    return scan_float(sp, tok);
  }

  // regular int or float
  p = buffer;
  p += strspn(p, "0123456789_+-.eE");
  len = p - buffer;
  buffer[len] = 0;

  if (process_numstr(buffer, 10, &reason)) {
    return RETERROR(sp->ebuf, lineno, reason);
  }

  *tok = mktoken(sp, TOK_INTEGER);
  errno = 0;
  tok->u.int64 = strtoll(buffer, &q, 10);
  if (errno || *q || q == buffer) {
    if (*q && strchr(".eE", *q)) {
      return scan_float(sp, tok);  // try to fit a float
    }
    return RETERROR(sp->ebuf, lineno, "error parsing integer");
  }

  tok->str.len = len;
  sp->cur += len;
  return 0;
}

[[nodiscard]] static int scan_bool(scanner_t *sp, token_t *tok) {
  char buffer[10];
  int len = sp->endp - sp->cur;
  if (len >= (int)sizeof(buffer)) {
    len = sizeof(buffer) - 1;
  }
  memcpy(buffer, sp->cur, len);
  buffer[len] = 0;  // NUL

  int lineno = sp->lineno;
  bool val = false;
  const char *p = buffer;
  if (0 == strncmp(p, "true", 4)) {
    val = true;
    p += 4;
  } else if (0 == strncmp(p, "false", 5)) {
    val = false;
    p += 5;
  } else {
    return RETERROR(sp->ebuf, lineno, "invalid boolean value");
  }
  if (*p && !strchr("# \r\n\t,}]", *p)) {
    return RETERROR(sp->ebuf, lineno, "invalid boolean value");
  }

  len = p - buffer;
  *tok = mktoken(sp, TOK_BOOL);
  tok->u.b1 = val;
  tok->str.len = len;
  sp->cur += len;
  return 0;
}

// Check if the next token may be TIME
static inline bool test_time(const char *p, const char *endp) {
  return &p[2] < endp && is_ascii_digit(p[0]) && is_ascii_digit(p[1]) &&
         p[2] == ':';
}

// Check if the next token may be DATE
static inline bool test_date(const char *p, const char *endp) {
  return &p[4] < endp && is_ascii_digit(p[0]) && is_ascii_digit(p[1]) &&
         is_ascii_digit(p[2]) && is_ascii_digit(p[3]) && p[4] == '-';
}

// Check if the next token may be BOOL
static inline bool test_bool(const char *p, const char *endp) {
  return &p[0] < endp && (*p == 't' || *p == 'f');
}

// Check if the next token may be NUMBER
static bool test_number(const char *p, const char *endp) {
  if (&p[0] < endp && *p && strchr("0123456789+-._", *p)) {
    return true;
  }
  if (&p[3] < endp) {
    if (0 == memcmp(p, "nan", 3) || 0 == memcmp(p, "inf", 3)) {
      return true;
    }
  }
  return false;
}

// Scan a literal that is not a string
[[nodiscard]] static int scan_nonstring_literal(scanner_t *sp, token_t *tok) {
  int lineno = sp->lineno;
  if (test_time(sp->cur, sp->endp)) {
    return scan_time(sp, tok);
  }

  if (test_date(sp->cur, sp->endp)) {
    return scan_timestamp(sp, tok);
  }

  if (test_bool(sp->cur, sp->endp)) {
    return scan_bool(sp, tok);
  }

  if (test_number(sp->cur, sp->endp)) {
    return scan_number(sp, tok);
  }
  return RETERROR(sp->ebuf, lineno, "invalid value");
}

// Scan a literal
[[nodiscard]] static int scan_literal(scanner_t *sp, token_t *tok) {
  *tok = mktoken(sp, TOK_LIT);
  const char *p = sp->cur;
  while (p < sp->endp && (is_ascii_alnum(*p) || *p == '_' || *p == '-')) {
    p++;
  }
  tok->str.len = p - tok->str.ptr;
  sp->cur = p;
  return 0;
}

// Save the current state of the scanner
static scanner_state_t scan_mark(scanner_t *sp) {
  scanner_state_t mark;
  mark.sp = sp;
  mark.cur = sp->cur;
  mark.lineno = sp->lineno;
  mark.bracket_level = sp->bracket_level;
  mark.brace_level = sp->brace_level;
  return mark;
}

// Restore the scanner state to a previously saved state
static void scan_restore(scanner_t *sp, scanner_state_t mark) {
  assert(mark.sp == sp);
  sp->cur = mark.cur;
  sp->lineno = mark.lineno;
  sp->bracket_level = mark.bracket_level;
  sp->brace_level = mark.brace_level;
}

// Return the next token
[[nodiscard]] static int scan_next(scanner_t *sp, bool keymode, token_t *tok) {
again:
  *tok = mktoken(sp, TOK_FIN);
  if (sp->errmsg) {
    return -1;
  }

  int ch = S_GET();
  if (ch == TOK_FIN) {
    return 0;
  }

  tok->str.len = 1;
  switch (ch) {
    case '\n':
      tok->toktyp = TOK_ENDL;
      break;

    case ' ':
    case '\t':
      goto again;  // skip whitespace

    case '#':
      // comment: skip until newline
      while (!S_MATCH('\n')) {
        ch = S_GET();
        if (ch == TOK_FIN) break;
        if ((0 <= ch && ch <= 0x8) || (0x0a <= ch && ch <= 0x1f) ||
            (ch == 0x7f)) {
          return RETERROR(sp->ebuf, sp->lineno, "bad control char in comment");
        }
      }
      goto again;  // skip comment

    case '.':
      tok->toktyp = TOK_DOT;
      break;

    case '=':
      tok->toktyp = TOK_EQUAL;
      break;

    case ',':
      tok->toktyp = TOK_COMMA;
      break;

    case '[':
      tok->toktyp = TOK_LBRACK;
      if (keymode && S_MATCH('[')) {
        S_GET();
        tok->toktyp = TOK_LLBRACK;
        tok->str.len = 2;
      }
      break;

    case ']':
      tok->toktyp = TOK_RBRACK;
      if (keymode && S_MATCH(']')) {
        S_GET();
        tok->toktyp = TOK_RRBRACK;
        tok->str.len = 2;
      }
      break;

    case '{':
      tok->toktyp = TOK_LBRACE;
      break;

    case '}':
      tok->toktyp = TOK_RBRACE;
      break;

    case '"':
      sp->cur--;
      DO(scan_string(sp, tok));
      break;

    case '\'':
      sp->cur--;
      DO(scan_litstring(sp, tok));
      break;

    default:
      sp->cur--;
      DO(keymode ? scan_literal(sp, tok) : scan_nonstring_literal(sp, tok));
      break;
  }

  return 0;
}

[[nodiscard]] static int check_overflow(scanner_t *sp, token_t *tok) {
  switch (tok->toktyp) {
    case TOK_LBRACK:
      sp->bracket_level++;
      if (sp->bracket_level > BRACKET_LEVEL_MAX) {
        return RETERROR(sp->ebuf, sp->lineno, "stack overflow");
      }
      break;
    case TOK_RBRACK:
      if (sp->bracket_level <= 0) {
        return RETERROR(sp->ebuf, sp->lineno, "unexpected right-bracket");
      }
      sp->bracket_level--;
      break;
    case TOK_LBRACE:
      sp->brace_level++;
      if (sp->brace_level > BRACE_LEVEL_MAX) {
        return RETERROR(sp->ebuf, sp->lineno, "stack overflow");
      }
      break;
    case TOK_RBRACE:
      if (sp->brace_level <= 0) {
        return RETERROR(sp->ebuf, sp->lineno, "unexpected right-brace");
      }
      sp->brace_level--;
      break;
    default:
      break;
  }
  return 0;
}

[[nodiscard]] static int scan_key(scanner_t *sp, token_t *tok) {
  DO(scan_next(sp, true, tok));
  DO(check_overflow(sp, tok));
  return 0;
}

[[nodiscard]] static int scan_value(scanner_t *sp, token_t *tok) {
  DO(scan_next(sp, false, tok));
  DO(check_overflow(sp, tok));
  return 0;
}

/**
 * Convert a char in utf8 into UCS, and store it in *ret.
 * Return #bytes consumed or -1 on failure.
 */
[[nodiscard]] static int utf8_to_ucs(const char *orig, int len, uint32_t *ret) {
  if (!orig || !ret || len <= 0) {
    return -1;
  }

  const unsigned char *buf = (const unsigned char *)orig;
  auto b0 = buf[0];
  if (b0 <= 0x7FU) {
    *ret = b0;
    return 1;
  }

  if (0xC2U <= b0 && b0 <= 0xDFU) {
    if (len < 2) {
      return -1;
    }
    auto b1 = buf[1];
    if ((b1 & 0xC0U) != 0x80U) {
      return -1;
    }
    *ret = ((uint32_t)(b0 & 0x1FU) << 6) | (uint32_t)(b1 & 0x3FU);
    return 2;
  }

  if (0xE0U <= b0 && b0 <= 0xEFU) {
    if (len < 3) {
      return -1;
    }
    auto b1 = buf[1];
    auto b2 = buf[2];
    if ((b1 & 0xC0U) != 0x80U || (b2 & 0xC0U) != 0x80U) {
      return -1;
    }
    if ((b0 == 0xE0U && b1 < 0xA0U) || (b0 == 0xEDU && b1 >= 0xA0U)) {
      return -1;
    }
    *ret = ((uint32_t)(b0 & 0x0FU) << 12) | ((uint32_t)(b1 & 0x3FU) << 6) |
           (uint32_t)(b2 & 0x3FU);
    return 3;
  }

  if (0xF0U <= b0 && b0 <= 0xF4U) {
    if (len < 4) {
      return -1;
    }
    auto b1 = buf[1];
    auto b2 = buf[2];
    auto b3 = buf[3];
    if ((b1 & 0xC0U) != 0x80U || (b2 & 0xC0U) != 0x80U ||
        (b3 & 0xC0U) != 0x80U) {
      return -1;
    }
    if ((b0 == 0xF0U && b1 < 0x90U) || (b0 == 0xF4U && b1 > 0x8FU)) {
      return -1;
    }
    *ret = ((uint32_t)(b0 & 0x07U) << 18) | ((uint32_t)(b1 & 0x3FU) << 12) |
           ((uint32_t)(b2 & 0x3FU) << 6) | (uint32_t)(b3 & 0x3FU);
    return 4;
  }

  return -1;
}

/**
 * Convert a UCS char to utf8 code, and return it in buf.
 * Return #bytes used in buf to encode the char, or
 * -1 on error.
 */
[[nodiscard]] static int ucs_to_utf8(uint32_t code, char buf[4]) {
  if (!buf || !is_unicode_scalar(code)) {
    return -1;
  }

  /* 0x00000000 - 0x0000007F:
     0xxxxxxx
  */
  if (code <= 0x7F) {
    buf[0] = (unsigned char)code;
    return 1;
  }

  /* 0x00000080 - 0x000007FF:
     110xxxxx 10xxxxxx
  */
  if (code <= 0x000007FF) {
    buf[0] = (unsigned char)(0xc0 | (code >> 6));
    buf[1] = (unsigned char)(0x80 | (code & 0x3f));
    return 2;
  }

  /* 0x00000800 - 0x0000FFFF:
     1110xxxx 10xxxxxx 10xxxxxx
  */
  if (code <= 0x0000FFFF) {
    buf[0] = (unsigned char)(0xe0 | (code >> 12));
    buf[1] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
    buf[2] = (unsigned char)(0x80 | (code & 0x3f));
    return 3;
  }

  /* 0x00010000 - 0x0010FFFF:
     11110xxx 10xxxxxx 10xxxxxx 10xxxxxx
  */
  if (code <= 0x0010FFFF) {
    buf[0] = (unsigned char)(0xf0 | (code >> 18));
    buf[1] = (unsigned char)(0x80 | ((code >> 12) & 0x3f));
    buf[2] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
    buf[3] = (unsigned char)(0x80 | (code & 0x3f));
    return 4;
  }

#ifdef UNDEF
  if (0) {
    // NOTE: these code points taking more than 4 bytes are not supported
    /* 0x00200000 - 0x03FFFFFF:
       111110xx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx
    */
    if (code <= 0x03FFFFFF) {
      buf[0] = (unsigned char)(0xf8 | (code >> 24));
      buf[1] = (unsigned char)(0x80 | ((code >> 18) & 0x3f));
      buf[2] = (unsigned char)(0x80 | ((code >> 12) & 0x3f));
      buf[3] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
      buf[4] = (unsigned char)(0x80 | (code & 0x3f));
      return 5;
    }

    /* 0x04000000 - 0x7FFFFFFF:
       1111110x 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx 10xxxxxx
    */
    if (code <= 0x7FFFFFFF) {
      buf[0] = (unsigned char)(0xfc | (code >> 30));
      buf[1] = (unsigned char)(0x80 | ((code >> 24) & 0x3f));
      buf[2] = (unsigned char)(0x80 | ((code >> 18) & 0x3f));
      buf[3] = (unsigned char)(0x80 | ((code >> 12) & 0x3f));
      buf[4] = (unsigned char)(0x80 | ((code >> 6) & 0x3f));
      buf[5] = (unsigned char)(0x80 | (code & 0x3f));
      return 6;
    }
  }
#endif

  return -1;
}
