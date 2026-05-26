#include <stdio.h>  // printf
#include <string.h> // memcpy

#define ARENA_IMPLEMENTATION
#include "arena/arena.h"

int main() {
  constexpr size_t arena_size = 1'024; // Allocate a 1kB arena
  auto arena = arena_create(arena_size);
  if (arena == nullptr) {
    return 1;
  }
  int exit_code = 0;

  constexpr size_t greeting_size = 7;
  char *first_half = arena_alloc(arena, greeting_size);
  char *second_half = arena_alloc(arena, greeting_size);
  if (first_half == nullptr || second_half == nullptr) {
    exit_code = 1;
    goto cleanup;
  }

  memcpy(first_half, "Hello \0", greeting_size);
  memcpy(second_half, "world!\0", greeting_size);

  printf("%s%s\n", first_half, second_half);

  arena_clear(arena); // "free" all memory allocated in an arena.
                      // 2 allocations, 1 "free"

  constexpr size_t numbers_label_size = 14;
  char *first_half_2 = arena_alloc(arena, numbers_label_size);
  constexpr size_t numbers_count = 3;
  int *second_half_2 = arena_alloc(arena, sizeof(int) * numbers_count);
  if (first_half_2 == nullptr || second_half_2 == nullptr) {
    exit_code = 1;
    goto cleanup;
  }

  memcpy(first_half_2, "Numbers 1-3: \0", numbers_label_size);
  int array_to_copy[3] = {1, 2, 3};
  memcpy(second_half_2, array_to_copy, sizeof(int) * numbers_count);

  printf("%s\n", first_half_2);
  for (size_t i = 0; i < numbers_count; i++) {
    printf("%d\n", second_half_2[i]);
  }

  // Notice we don't have to clear here.

cleanup:
  arena_destroy(arena); // Free the allocated arena and everything in it

  return exit_code;
}
