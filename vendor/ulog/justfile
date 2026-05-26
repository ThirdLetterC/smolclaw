set shell := ["bash", "--noprofile", "--norc", "-uc"]

ZIG := env_var_or_default("ZIG", "zig")
CLANG_FORMAT := env_var_or_default("CLANG_FORMAT", "clang-format")

default:
    @just --list

# Zig build helpers
build:
    zig build

run:
    zig build run

run-all-features:
    zig build run-all-features

test:
    {{ZIG}} build test

test-sanitized:
    {{ZIG}} build --global-cache-dir .zig-global-cache -Dsanitizers=true test

format:
    {{CLANG_FORMAT}} -i \
        include/ulog/ulog.h \
        src/ulog.c \
        testing/tests.c \
        examples/ulog_example.c \
        examples/ulog_all_features.c \
        extensions/ulog_syslog.c \
        extensions/ulog_syslog.h \
        extensions/ulog_microlog6.h

# Direct Zig compiler helpers
cc-example out="ulog_example":
    {{ZIG}} cc -std=c23 -Wall -Wextra -Wpedantic -Werror -Iinclude \
        src/ulog.c examples/ulog_example.c -o {{out}}

cc-all-features out="ulog_all_features":
    {{ZIG}} cc -std=c23 -Wall -Wextra -Wpedantic -Werror -DULOG_BUILD_DYNAMIC_CONFIG=1 \
        -Iinclude -Iextensions src/ulog.c extensions/ulog_syslog.c \
        examples/ulog_all_features.c -o {{out}}

cc-debug-example out="ulog_example_dbg":
    {{ZIG}} cc -std=c23 -Wall -Wextra -Wpedantic -Werror -O0 -g \
        -fsanitize=address,undefined,leak -Iinclude \
        src/ulog.c examples/ulog_example.c -o {{out}}

cc-debug-all-features out="ulog_all_features_dbg":
    {{ZIG}} cc -std=c23 -Wall -Wextra -Wpedantic -Werror -O0 -g \
        -fsanitize=address,undefined,leak -DULOG_BUILD_DYNAMIC_CONFIG=1 \
        -Iinclude -Iextensions src/ulog.c extensions/ulog_syslog.c \
        examples/ulog_all_features.c -o {{out}}

cc-debug-tests out="ulog_tests_dbg":
    {{ZIG}} cc -std=c23 -Wall -Wextra -Wpedantic -Werror -O0 -g \
        -fsanitize=address,undefined,leak -DULOG_BUILD_DYNAMIC_CONFIG=1 \
        -Iinclude src/ulog.c testing/tests.c -o {{out}}

clean:
    rm -rf zig-out zig-cache ulog_example ulog_all_features
