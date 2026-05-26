// Copyright 2007 - 2021, Alan Antonuk and the rabbitmq-c contributors.
// SPDX-License-Identifier: mit

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <assert.h>
#include <limits.h>
#include <stdckdint.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include "rabbitmq/amqp_private.h"

char const *amqp_version() { return AMQP_VERSION_STRING; }

uint32_t amqp_version_number() { return AMQP_VERSION; }

void init_amqp_pool(amqp_pool_t *pool, size_t pagesize) {
  pool->pagesize = pagesize ? pagesize : 4096;

  pool->pages.num_blocks = 0;
  pool->pages.blocklist = nullptr;

  pool->large_blocks.num_blocks = 0;
  pool->large_blocks.blocklist = nullptr;

  pool->next_page = 0;
  pool->alloc_block = nullptr;
  pool->alloc_used = 0;
}

static void empty_blocklist(amqp_pool_blocklist_t *x) {
  int i;

  if (x->blocklist != nullptr) {
    for (i = 0; i < x->num_blocks; i++) {
      free(x->blocklist[i]);
    }
    free(x->blocklist);
  }
  x->num_blocks = 0;
  x->blocklist = nullptr;
}

void recycle_amqp_pool(amqp_pool_t *pool) {
  empty_blocklist(&pool->large_blocks);
  pool->next_page = 0;
  pool->alloc_block = nullptr;
  pool->alloc_used = 0;
}

void empty_amqp_pool(amqp_pool_t *pool) {
  recycle_amqp_pool(pool);
  empty_blocklist(&pool->pages);
}

/* Returns 1 on success, 0 on failure */
static int record_pool_block(amqp_pool_blocklist_t *x, void *block) {
  size_t next_num_blocks;
  size_t blocklistlength;

  if (x->num_blocks < 0 || x->num_blocks == INT_MAX) {
    return 0;
  }

  next_num_blocks = (size_t)x->num_blocks;
  if (ckd_add(&next_num_blocks, next_num_blocks, (size_t)1) ||
      ckd_mul(&blocklistlength, next_num_blocks, sizeof(void *))) {
    return 0;
  }

  if (x->blocklist == nullptr) {
    x->blocklist = calloc(1, blocklistlength);
    if (x->blocklist == nullptr) {
      return 0;
    }
  } else {
    void *newbl = realloc(x->blocklist, blocklistlength);
    if (newbl == nullptr) {
      return 0;
    }
    x->blocklist = newbl;
  }

  x->blocklist[x->num_blocks] = block;
  x->num_blocks++;
  return 1;
}

void *amqp_pool_alloc(amqp_pool_t *pool, size_t amount) {
  size_t aligned_amount;

  if (amount == 0) {
    return nullptr;
  }

  if (ckd_add(&aligned_amount, amount, (size_t)7)) {
    return nullptr;
  }
  amount =
      aligned_amount & ~(size_t)7; /* round up to nearest 8-byte boundary */

  if (amount > pool->pagesize) {
    void *result = calloc(1, amount);
    if (result == nullptr) {
      return nullptr;
    }
    if (!record_pool_block(&pool->large_blocks, result)) {
      free(result);
      return nullptr;
    }
    return result;
  }

  if (pool->alloc_block != nullptr) {
    size_t new_alloc_used;

    assert(pool->alloc_used <= pool->pagesize);

    if (!ckd_add(&new_alloc_used, pool->alloc_used, amount) &&
        new_alloc_used <= pool->pagesize) {
      void *result = pool->alloc_block + pool->alloc_used;
      pool->alloc_used = new_alloc_used;
      return result;
    }
  }

  if (pool->next_page >= pool->pages.num_blocks) {
    pool->alloc_block = calloc(1, pool->pagesize);
    if (pool->alloc_block == nullptr) {
      return nullptr;
    }
    if (!record_pool_block(&pool->pages, pool->alloc_block)) {
      free(pool->alloc_block);
      pool->alloc_block = nullptr;
      pool->alloc_used = 0;
      return nullptr;
    }
    pool->next_page = pool->pages.num_blocks;
  } else {
    pool->alloc_block = pool->pages.blocklist[pool->next_page];
    pool->next_page++;
  }

  pool->alloc_used = amount;

  return pool->alloc_block;
}

void amqp_pool_alloc_bytes(amqp_pool_t *pool, size_t amount,
                           amqp_bytes_t *output) {
  output->len = amount;
  output->bytes = amqp_pool_alloc(pool, amount);
}

amqp_bytes_t amqp_cstring_bytes(char const *cstr) {
  amqp_bytes_t result;
  result.len = strlen(cstr);
  result.bytes = (void *)cstr;
  return result;
}

amqp_bytes_t amqp_bytes_malloc_dup(amqp_bytes_t src) {
  amqp_bytes_t result;
  result.len = src.len;
  result.bytes = calloc(1, src.len);
  if (result.bytes != nullptr) {
    memcpy(result.bytes, src.bytes, src.len);
  }
  return result;
}

amqp_bytes_t amqp_bytes_malloc(size_t amount) {
  amqp_bytes_t result;
  result.len = amount;
  result.bytes = calloc(1, amount); /* will return nullptr if it fails */
  return result;
}

void amqp_bytes_free(amqp_bytes_t bytes) { free(bytes.bytes); }

amqp_pool_t *amqp_get_or_create_channel_pool(amqp_connection_state_t state,
                                             amqp_channel_t channel) {
  amqp_pool_table_entry_t *entry;
  size_t index = channel % POOL_TABLE_SIZE;

  entry = state->pool_table[index];

  for (; nullptr != entry; entry = entry->next) {
    if (channel == entry->channel) {
      return &entry->pool;
    }
  }

  entry = calloc(1, sizeof(amqp_pool_table_entry_t));
  if (nullptr == entry) {
    return nullptr;
  }

  entry->channel = channel;
  entry->next = state->pool_table[index];
  state->pool_table[index] = entry;

  init_amqp_pool(&entry->pool, state->frame_max);

  return &entry->pool;
}

amqp_pool_t *amqp_get_channel_pool(amqp_connection_state_t state,
                                   amqp_channel_t channel) {
  amqp_pool_table_entry_t *entry;
  size_t index = channel % POOL_TABLE_SIZE;

  entry = state->pool_table[index];

  for (; nullptr != entry; entry = entry->next) {
    if (channel == entry->channel) {
      return &entry->pool;
    }
  }

  return nullptr;
}

int amqp_bytes_equal(amqp_bytes_t r, amqp_bytes_t l) {
  if (r.len == l.len &&
      (r.bytes == l.bytes || 0 == memcmp(r.bytes, l.bytes, r.len))) {
    return 1;
  }
  return 0;
}
