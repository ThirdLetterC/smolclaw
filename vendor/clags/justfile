set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

cc := env_var_or_default("CC", "cc")
fuzz_cc := env_var_or_default("FUZZ_CC", "clang")
std := env_var_or_default("STD", "-std=c2x")
warn := env_var_or_default("WARN", "-Wall -Wextra -Wpedantic -Werror")
hard := env_var_or_default("HARD", "-fstack-protector-strong -D_FORTIFY_SOURCE=3 -fPIE")
ld_hard := env_var_or_default("LD_HARD", "-Wl,-z,relro,-z,now -pie")
san := env_var_or_default("SAN", "-fsanitize=address,undefined,leak -fno-omit-frame-pointer")
fuzz_san := env_var_or_default("FUZZ_SAN", "-fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer")
base_cflags := std + " " + warn + " " + hard
examples := "examples/01_basic examples/02_types examples/03_1_lists examples/03_2_multi-lists examples/04_choices examples/05_paths examples/06_custom examples/07_subcommands examples/08_image_processor examples/09_log_error_handling"

default: all

all: examples-build tests-build

examples-build:
    @for src in examples/*.c; do exe="${src%.c}"; {{cc}} {{base_cflags}} -Iinclude -o "$exe" "$src" src/clags.c {{ld_hard}}; done

examples-debug:
    @for src in examples/*.c; do exe="${src%.c}"; {{cc}} {{base_cflags}} -Iinclude -g3 {{san}} -o "$exe" "$src" src/clags.c {{san}} {{ld_hard}}; done

tests-build:
    {{cc}} {{base_cflags}} -Iinclude -o testing/tests testing/tests.c src/clags.c -lm {{ld_hard}}

tests-debug:
    {{cc}} {{base_cflags}} -Iinclude -g3 {{san}} -o testing/tests testing/tests.c src/clags.c -lm {{san}} {{ld_hard}}

test: tests-build
    ./testing/tests

test-debug: tests-debug
    ./testing/tests

fuzz-build:
    {{fuzz_cc}} {{base_cflags}} -Iinclude -g3 -o testing/fuzz_parse testing/fuzz_parse.c src/clags.c -lm {{fuzz_san}} {{ld_hard}}

fuzz: fuzz-build
    ./testing/fuzz_parse -max_total_time=60

zig-all:
    zig build

zig-examples:
    zig build examples

zig-tests:
    zig build tests

zig-test:
    zig build test

format:
    clang-format -i include/clags/clags.h src/clags.c examples/*.c testing/tests.c testing/fuzz_parse.c

clean:
    rm -f {{examples}} testing/tests testing/fuzz_parse

clean-zig:
    rm -rf .zig-cache zig-out
