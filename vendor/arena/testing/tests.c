#include "rktest/rktest.h"

#define ARENA_DEBUG
#define ARENA_SECURE_WIPE_ON_CLEAR
#define ARENA_SECURE_WIPE_ON_DESTROY
#include "arena/arena.h"

TEST(arena_init_tests, init) {
  Arena arena;
  constexpr size_t data_size = 256;
  char data[256];
  arena_init(&arena, data, data_size);
  EXPECT_TRUE(arena.region == data);
  EXPECT_LONG_EQ((long)arena.size, (long)data_size);
  EXPECT_LONG_EQ((long)arena.index, 0);
}

TEST(arena_init_tests, rejects_region_and_size_mismatch) {
  char sentinel_region[8];
  Arena_Allocation sentinel_allocation;
  Arena arena = {.region = sentinel_region,
                 .index = 4,
                 .size = sizeof(sentinel_region),
                 .allocations = 7,
                 .head_allocation = &sentinel_allocation};
  char other_region[16];

  arena_init(&arena, nullptr, sizeof(other_region));
  EXPECT_TRUE(arena.region == sentinel_region);
  EXPECT_LONG_EQ((long)arena.index, 4);
  EXPECT_LONG_EQ((long)arena.size, (long)sizeof(sentinel_region));
  EXPECT_LONG_EQ((long)arena.allocations, 7);
  EXPECT_TRUE(arena.head_allocation == &sentinel_allocation);

  arena_init(&arena, other_region, 0);
  EXPECT_TRUE(arena.region == sentinel_region);
  EXPECT_LONG_EQ((long)arena.index, 4);
  EXPECT_LONG_EQ((long)arena.size, (long)sizeof(sentinel_region));
  EXPECT_LONG_EQ((long)arena.allocations, 7);
  EXPECT_TRUE(arena.head_allocation == &sentinel_allocation);
}

TEST(arena_create_tests, basic) {
  constexpr size_t arena_size = 8;
  auto arena = arena_create(arena_size);
  ASSERT_TRUE(arena != nullptr);
  EXPECT_LONG_EQ((long)arena->size, (long)arena_size);
  arena_destroy(arena);
}

TEST(arena_create_tests, zero_bytes) {
  ASSERT_TRUE(arena_create(0) == nullptr);
}

TEST(arena_alloc_tests, basic) {
  constexpr size_t region_size = 256;
  char region[256];
  Arena arena;
  arena_init(&arena, region, region_size);
  auto bytes = arena_alloc(&arena, 8);
  ASSERT_TRUE(bytes != nullptr);
  EXPECT_TRUE(arena.region == bytes);
  EXPECT_LONG_EQ((long)arena.index, 8);
  EXPECT_LONG_EQ((long)arena.allocations, 1);
  arena_clear(&arena);
}

TEST(arena_alloc_tests, zero_size) {
  char region[32];
  Arena arena;
  arena_init(&arena, region, sizeof(region));
  arena.index = 4;

  auto bytes = arena_alloc(&arena, 0);
  ASSERT_TRUE(bytes == nullptr);
  EXPECT_LONG_EQ((long)arena.index, 4);
  EXPECT_LONG_EQ((long)arena.allocations, 0);
}

TEST(arena_alloc_tests, rejects_out_of_bounds_index) {
  char region[32];
  Arena arena;
  arena_init(&arena, region, sizeof(region));
  arena.index = sizeof(region) + 1;

  auto bytes = arena_alloc(&arena, 1);
  ASSERT_TRUE(bytes == nullptr);
  EXPECT_LONG_EQ((long)arena.index, (long)sizeof(region) + 1);
  EXPECT_LONG_EQ((long)arena.allocations, 0);
}

TEST(arena_alloc_tests, rejects_null_arena) {
  EXPECT_TRUE(arena_alloc(nullptr, 8) == nullptr);
  EXPECT_TRUE(arena_alloc_aligned(nullptr, 8, 8) == nullptr);
}

TEST(arena_alloc_tests, rejects_arena_without_region) {
  Arena arena = {.region = nullptr, .index = 0, .size = 32};
  EXPECT_TRUE(arena_alloc(&arena, 8) == nullptr);
  EXPECT_TRUE(arena_alloc_aligned(&arena, 8, 8) == nullptr);
}

TEST(arena_alloc_aligned_tests, edge_case_tight_space) {
  constexpr size_t region_size = 30;
  char region[30];
  Arena arena;
  arena_init(&arena, region, region_size);
  arena.index = 10;

  void *ptr = arena_alloc_aligned(&arena, 14, 16);

  ASSERT_TRUE(ptr != nullptr);
  EXPECT_LONG_EQ((long)arena.index, 30);
  EXPECT_LONG_EQ((long)arena.allocations, 1);
  arena_clear(&arena);
}

TEST(arena_alloc_aligned_tests, respects_requested_alignment) {
  char region[64];
  Arena arena;
  arena_init(&arena, region, sizeof(region));
  arena.index = 3;

  void *ptr = arena_alloc_aligned(&arena, 8, 16);

  ASSERT_TRUE(ptr != nullptr);
  EXPECT_TRUE((((uintptr_t)ptr) & (uintptr_t)(16 - 1)) == 0u);
  EXPECT_LONG_EQ((long)arena.index, 24);
  EXPECT_LONG_EQ((long)arena.allocations, 1);
  auto allocation = arena_get_allocation_struct(&arena, ptr);
  ASSERT_TRUE(allocation != nullptr);
  EXPECT_LONG_EQ((long)allocation->index, 16);
  EXPECT_LONG_EQ((long)allocation->size, 8);
  EXPECT_TRUE(allocation->pointer == ptr);
  arena_clear(&arena);
}

TEST(arena_alloc_aligned_tests, aligns_against_absolute_pointer_address) {
  char raw_region[96];
  Arena arena;
  arena_init(&arena, raw_region + 1, sizeof(raw_region) - 1);

  void *ptr = arena_alloc_aligned(&arena, 8, 16);

  ASSERT_TRUE(ptr != nullptr);
  EXPECT_TRUE((((uintptr_t)ptr) & (uintptr_t)(16 - 1)) == 0u);
  EXPECT_TRUE((char *)ptr >= arena.region);
  EXPECT_TRUE(((char *)ptr + 8) <= (arena.region + arena.size));
  EXPECT_TRUE(arena.index >= 8);
  EXPECT_TRUE(arena.index <= arena.size);
  EXPECT_LONG_EQ((long)arena.allocations, 1);
  arena_clear(&arena);
}

TEST(arena_alloc_aligned_tests, rejects_zero_alignment) {
  char region[64];
  Arena arena;
  arena_init(&arena, region, sizeof(region));

  void *ptr = arena_alloc_aligned(&arena, 8, 0);

  ASSERT_TRUE(ptr == nullptr);
  EXPECT_LONG_EQ((long)arena.index, 0);
  EXPECT_LONG_EQ((long)arena.allocations, 0);
}

TEST(arena_alloc_aligned_tests, rejects_non_power_of_two_alignment) {
  char region[64];
  Arena arena;
  arena_init(&arena, region, sizeof(region));

  void *ptr = arena_alloc_aligned(&arena, 8, 6);

  ASSERT_TRUE(ptr == nullptr);
  EXPECT_LONG_EQ((long)arena.index, 0);
  EXPECT_LONG_EQ((long)arena.allocations, 0);
}

TEST(arena_alloc_aligned_tests, rejects_overflowing_index_updates) {
  char region[1];
  Arena arena;
  arena_init(&arena, region, SIZE_MAX);
  arena.index = SIZE_MAX - 4;

  void *ptr = arena_alloc_aligned(&arena, 8, 8);

  ASSERT_TRUE(ptr == nullptr);
  EXPECT_TRUE(arena.index == SIZE_MAX - 4);
  EXPECT_LONG_EQ((long)arena.allocations, 0);
}

TEST(arena_alloc_aligned_tests, rejects_insufficient_space_after_padding) {
  char region[32];
  Arena arena;
  arena_init(&arena, region, sizeof(region));
  arena.index = 24;

  void *ptr = arena_alloc_aligned(&arena, 8, 16);

  ASSERT_TRUE(ptr == nullptr);
  EXPECT_LONG_EQ((long)arena.index, 24);
  EXPECT_LONG_EQ((long)arena.allocations, 0);
}

TEST(arena_copy_tests, clamps_source_index_to_source_size) {
  char src_region[8] = {1, 2, 3, 4, 5, 6, 7, 8};
  char dest_region[64];
  Arena src;
  Arena dest;

  arena_init(&src, src_region, sizeof(src_region));
  arena_init(&dest, dest_region, sizeof(dest_region));
  src.index = 16;

  const size_t bytes_copied = arena_copy(&dest, &src);
  EXPECT_LONG_EQ((long)bytes_copied, (long)sizeof(src_region));
  EXPECT_LONG_EQ((long)dest.index, (long)sizeof(src_region));
}

TEST(arena_copy_tests, truncates_to_destination_capacity_and_copies_bytes) {
  char src_region[8] = {9, 8, 7, 6, 5, 4, 3, 2};
  char dest_region[5] = {0, 0, 0, 0, 0};
  Arena src;
  Arena dest;

  arena_init(&src, src_region, sizeof(src_region));
  arena_init(&dest, dest_region, sizeof(dest_region));
  src.index = sizeof(src_region);

  const size_t bytes_copied = arena_copy(&dest, &src);
  EXPECT_LONG_EQ((long)bytes_copied, (long)sizeof(dest_region));
  EXPECT_LONG_EQ((long)dest.index, (long)sizeof(dest_region));
  EXPECT_EQ((int)(unsigned char)dest_region[0], 9);
  EXPECT_EQ((int)(unsigned char)dest_region[1], 8);
  EXPECT_EQ((int)(unsigned char)dest_region[2], 7);
  EXPECT_EQ((int)(unsigned char)dest_region[3], 6);
  EXPECT_EQ((int)(unsigned char)dest_region[4], 5);
}

TEST(arena_copy_tests, handles_overlapping_regions_safely) {
  unsigned char shared_region[16] = {0, 1, 2,  3,  4,  5,  6,  7,
                                     8, 9, 10, 11, 12, 13, 14, 15};
  Arena src;
  Arena dest;

  arena_init(&src, shared_region, 8);
  arena_init(&dest, shared_region + 2, 8);
  src.index = 8;

  const size_t bytes_copied = arena_copy(&dest, &src);
  EXPECT_LONG_EQ((long)bytes_copied, 8);
  EXPECT_LONG_EQ((long)dest.index, 8);
  EXPECT_EQ((int)shared_region[0], 0);
  EXPECT_EQ((int)shared_region[1], 1);
  EXPECT_EQ((int)shared_region[2], 0);
  EXPECT_EQ((int)shared_region[3], 1);
  EXPECT_EQ((int)shared_region[4], 2);
  EXPECT_EQ((int)shared_region[5], 3);
  EXPECT_EQ((int)shared_region[6], 4);
  EXPECT_EQ((int)shared_region[7], 5);
  EXPECT_EQ((int)shared_region[8], 6);
  EXPECT_EQ((int)shared_region[9], 7);
}

TEST(arena_copy_tests, returns_zero_for_invalid_inputs) {
  char src_region[8] = {0};
  char dest_region[8] = {0};
  Arena src;
  Arena dest;
  Arena no_region = {0};

  arena_init(&src, src_region, sizeof(src_region));
  arena_init(&dest, dest_region, sizeof(dest_region));
  src.index = sizeof(src_region);

  EXPECT_LONG_EQ((long)arena_copy(nullptr, &src), 0);
  EXPECT_LONG_EQ((long)arena_copy(&dest, nullptr), 0);
  EXPECT_LONG_EQ((long)arena_copy(&dest, &no_region), 0);
  EXPECT_LONG_EQ((long)arena_copy(&no_region, &src), 0);
}

TEST(arena_debug_tests, lookup_returns_metadata_for_pointer) {
  char region[64];
  Arena arena;
  arena_init(&arena, region, sizeof(region));

  void *ptr_a = arena_alloc(&arena, 8);
  void *ptr_b = arena_alloc_aligned(&arena, 4, 16);
  ASSERT_TRUE(ptr_a != nullptr);
  ASSERT_TRUE(ptr_b != nullptr);

  auto metadata_a = arena_get_allocation_struct(&arena, ptr_a);
  auto metadata_b = arena_get_allocation_struct(&arena, ptr_b);
  ASSERT_TRUE(metadata_a != nullptr);
  ASSERT_TRUE(metadata_b != nullptr);
  EXPECT_LONG_EQ((long)metadata_a->index, 0);
  EXPECT_LONG_EQ((long)metadata_a->size, 8);
  EXPECT_TRUE(metadata_a->pointer == ptr_a);
  EXPECT_LONG_EQ((long)metadata_b->index, 16);
  EXPECT_LONG_EQ((long)metadata_b->size, 4);
  EXPECT_TRUE(metadata_b->pointer == ptr_b);

  arena_clear(&arena);
}

TEST(arena_debug_tests, lookup_returns_null_for_unknown_pointer) {
  char region[64];
  char unrelated = 0;
  Arena arena;
  arena_init(&arena, region, sizeof(region));

  auto ptr = arena_alloc(&arena, 8);
  ASSERT_TRUE(ptr != nullptr);
  EXPECT_TRUE(arena_get_allocation_struct(&arena, &unrelated) == nullptr);
  EXPECT_TRUE(arena_get_allocation_struct(nullptr, ptr) == nullptr);
  EXPECT_TRUE(arena_get_allocation_struct(&arena, nullptr) == nullptr);

  arena_clear(&arena);
}

TEST(arena_debug_tests, clear_resets_index_and_allocation_list) {
  char region[64];
  Arena arena;
  arena_init(&arena, region, sizeof(region));

  ASSERT_TRUE(arena_alloc(&arena, 8) != nullptr);
  ASSERT_TRUE(arena_alloc(&arena, 8) != nullptr);
  EXPECT_LONG_EQ((long)arena.allocations, 2);
  EXPECT_TRUE(arena.head_allocation != nullptr);

  arena_clear(&arena);

  EXPECT_LONG_EQ((long)arena.index, 0);
  EXPECT_LONG_EQ((long)arena.allocations, 0);
  EXPECT_TRUE(arena.head_allocation == nullptr);
}

TEST(arena_destroy_tests,
     non_owning_destroy_resets_state_without_freeing_external_region) {
  char region[32] = {0x5A};
  Arena arena;
  arena_init(&arena, region, sizeof(region));

  auto allocation = arena_alloc(&arena, 8);
  ASSERT_TRUE(allocation != nullptr);
  EXPECT_TRUE(arena.owns_self == false);
  EXPECT_TRUE(arena.owns_region == false);

  arena_destroy(&arena);

  EXPECT_TRUE(arena.region == nullptr);
  EXPECT_LONG_EQ((long)arena.index, 0);
  EXPECT_LONG_EQ((long)arena.size, 0);
  EXPECT_TRUE(arena.owns_self == false);
  EXPECT_TRUE(arena.owns_region == false);
  EXPECT_EQ((int)(unsigned char)region[0], 0x5A);
}

TEST(arena_security_tests, clear_securely_wipes_active_bytes) {
  unsigned char region[32];
  for (size_t i = 0; i < sizeof(region); ++i) {
    region[i] = 0xA5;
  }

  Arena arena;
  arena_init(&arena, region, sizeof(region));

  auto ptr = arena_alloc(&arena, 8);
  ASSERT_TRUE(ptr != nullptr);
  auto allocation = arena_get_allocation_struct(&arena, ptr);
  ASSERT_TRUE(allocation != nullptr);
  const size_t start = allocation->index;
  for (size_t i = 0; i < 8; ++i) {
    ((unsigned char *)ptr)[i] = 0x5A;
  }

  arena_clear(&arena);

  for (size_t i = 0; i < 8; ++i) {
    EXPECT_EQ((int)region[start + i], 0);
  }
}
