set shell := ["bash", "-cu"]

zig := env_var_or_default("ZIG", "zig")
clang_format := env_var_or_default("CLANG_FORMAT", "clang-format")

default: help

help:
    just --list

build:
    {{zig}} build

test:
    {{zig}} build tests

examples:
    {{zig}} build examples

tools:
    {{zig}} build tools

clean:
    {{zig}} build clean

format:
    {{zig}} fmt build.zig
    echo "Running clang-format..."
    find include src tests tools examples -type f \
        \( -name '*.c' -o -name '*.h' \) -print0 \
        | xargs -0 {{clang_format}} -i

fmt: format

format-check:
    echo "Checking clang-format..."
    find include src tests tools examples -type f \
        \( -name '*.c' -o -name '*.h' \) -print0 \
        | xargs -0 {{clang_format}} --dry-run --Werror
