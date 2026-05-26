#include <stdio.h> // printf

#define ARENA_DEBUG
#define ARENA_IMPLEMENTATION
#include "arena/arena.h"

int main() {
  constexpr size_t arena_size = 1'024;
  auto arena = arena_create(arena_size);
  if (arena == nullptr) {
    return 1;
  }
  int exit_code = 0;

  char *x = arena_alloc(arena, 5);
  char *y = arena_alloc(arena, 25);
  if (x == nullptr || y == nullptr) {
    exit_code = 1;
    goto cleanup;
  }

  auto x_allocation = arena_get_allocation_struct(arena, x);
  auto y_allocation = arena_get_allocation_struct(arena, y);
  if (x_allocation == nullptr || y_allocation == nullptr) {
    exit_code = 1;
    goto cleanup;
  }

  printf("X index in region: %zu\n", x_allocation->index);
  printf("X size in region: %zu\n", x_allocation->size);

  printf("Y index in region: %zu\n", y_allocation->index);
  printf("Y size in region: %zu\n", y_allocation->size);

cleanup:
  arena_destroy(arena);

  return exit_code;
}
