set shell := ["bash", "-cu"]

clang_format := env_var_or_default("CLANG_FORMAT", "clang-format")
cc := env_var_or_default("CC", "cc")
zig_global_cache_dir := env_var_or_default("ZIG_GLOBAL_CACHE_DIR", ".zig-global-cache")
std_flags := env_var_or_default("STD_FLAGS", "-std=c2x")
warn_flags := "-Wall -Wextra -Wpedantic -Werror"
hardening_cflags := "-fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE"
hardening_ldflags := "-Wl,-z,relro,-z,now -pie"
use_sanitizers := env_var_or_default("USE_SANITIZERS", "1")
sanitizer_cflags := if use_sanitizers == "1" { "-fsanitize=address,undefined,leak -fno-omit-frame-pointer" } else { "" }
sanitizer_ldflags := if use_sanitizers == "1" { "-fsanitize=address,undefined,leak" } else { "" }
cflags := "-Og -g " + std_flags + " " + warn_flags + " " + hardening_cflags + " " + sanitizer_cflags
ldflags := hardening_ldflags + " " + sanitizer_ldflags
include_dir := "include"
use_mimalloc := env_var_or_default("USE_MIMALLOC", "0")
mimalloc_cflags := if use_mimalloc == "1" { "-DARENA_USE_MIMALLOC" } else { "" }
mimalloc_ldflags := if use_mimalloc == "1" { "-lmimalloc" } else { "" }
format_files := "src/arena.c include/arena/arena.h testing/tests.c examples/*.c"

default: tests

tests:
    ZIG_GLOBAL_CACHE_DIR={{zig_global_cache_dir}} {{cc}} {{cflags}} {{mimalloc_cflags}} -I{{include_dir}} -c -o arena.o src/arena.c
    ZIG_GLOBAL_CACHE_DIR={{zig_global_cache_dir}} {{cc}} {{cflags}} -Wno-newline-eof {{mimalloc_cflags}} -I{{include_dir}} -o test testing/tests.c src/rktest.c arena.o -lm {{ldflags}} {{mimalloc_ldflags}}

test:
    USE_SANITIZERS=0 just tests
    echo "Running tests under valgrind..."
    valgrind ./test
    just clean
    echo "Testing complete."

clean:
    echo "Removing executables..."
    rm -rf .zig-cache
    rm -rf zig-out
    rm -f test
    rm -f *.o
    for src in examples/*.c; do \
        name="$${src##*/}"; \
        name="$${name%.c}"; \
        rm -f "$${name}"; \
    done
    echo "Executables removed."

format:
    zig fmt build.zig
    {{clang_format}} -i {{format_files}}

fmt: format
