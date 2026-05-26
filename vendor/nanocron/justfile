set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

cc := env_var_or_default("CC", "clang")
std := env_var_or_default("STD", "-std=c23")
warn := env_var_or_default("WARN", "-Wall -Wextra -Wpedantic -Werror")
hard := env_var_or_default("HARD", "-fstack-protector-strong -D_FORTIFY_SOURCE=3 -D_POSIX_C_SOURCE=200809L -fPIE")
inc := env_var_or_default("INC", "-Iinclude")
ld_hard := env_var_or_default("LD_HARD", "-Wl,-z,relro,-z,now -pie")
san := env_var_or_default("SAN", "-fsanitize=address,undefined,leak -fno-omit-frame-pointer")
base_cflags := std + " " + warn + " " + hard + " " + inc

default: all

all: examples-build tests-build

examples-build:
    @for src in examples/*.c; do exe="${src%.c}"; {{cc}} {{base_cflags}} -o "$exe" "$src" src/nanocron.c {{ld_hard}}; done

examples-debug:
    @for src in examples/*.c; do exe="${src%.c}"; {{cc}} {{base_cflags}} -g3 {{san}} -o "$exe" "$src" src/nanocron.c {{san}} {{ld_hard}}; done

tests-build:
    {{cc}} {{base_cflags}} -o testing/tests testing/tests.c src/nanocron.c -lm {{ld_hard}}

tests-debug:
    {{cc}} {{base_cflags}} -g3 {{san}} -o testing/tests testing/tests.c src/nanocron.c -lm {{san}} {{ld_hard}}

test: tests-build
    ./testing/tests

test-debug: tests-debug
    ./testing/tests

zig-all:
    zig build

zig-examples:
    zig build examples

zig-tests:
    zig build tests

zig-test:
    zig build test

format:
    clang-format -i include/nanocron/nanocron.h src/nanocron.c examples/*.c testing/tests.c

clean:
    @for src in examples/*.c; do \
        if [ -f "$src" ]; then \
            rm -f "${src%.c}"; \
        fi; \
    done
    rm -f testing/tests

clean-zig:
    rm -rf .zig-cache zig-out
