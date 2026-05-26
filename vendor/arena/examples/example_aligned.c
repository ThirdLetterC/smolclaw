#include <stdio.h> // printf

#define ARENA_IMPLEMENTATION
#include "arena/arena.h"

int main() {
  constexpr size_t arena_size = 1'024; // Allocate a 1kB arena
  auto arena = arena_create(arena_size);
  if (arena == nullptr) {
    return 1;
  }
  int exit_code = 0;

  char *char_ptr_1 = arena_alloc_aligned(arena, 10, 4);
  if (char_ptr_1 == nullptr) {
    exit_code = 1;
    goto cleanup;
  }
  printf("%zu\n", arena->index); // 10

  char *char_ptr_2 = arena_alloc_aligned(arena, 10, 4);
  if (char_ptr_2 == nullptr) {
    exit_code = 1;
    goto cleanup;
  }
  printf("%zu\n", arena->index); // 22

  char *char_ptr_3 = arena_alloc_aligned(arena, 10, 4);
  if (char_ptr_3 == nullptr) {
    exit_code = 1;
    goto cleanup;
  }
  printf("%zu\n", arena->index); // 34

cleanup:
  arena_destroy(arena); // Free the allocated arena and everything in it

  return exit_code;
}
