set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

zig := env_var_or_default("ZIG", "zig")
clang_format := env_var_or_default("CLANG_FORMAT", "clang-format")
format_files := "src/websocket_client.c include/websocket_client/websocket_client.h examples/simple.c testing/tests.c"
zig_cache_dir := env_var_or_default("ZIG_CACHE_DIR", ".zig-cache")
zig_global_cache_dir := env_var_or_default("ZIG_GLOBAL_CACHE_DIR", ".zig-global-cache")
zig_build := zig + " build --cache-dir " + zig_cache_dir + " --global-cache-dir " + zig_global_cache_dir

default: build

build:
    {{zig_build}}

build-no-sanitize:
    {{zig_build}} -Dsanitize=false

build-release:
    {{zig_build}} -Doptimize=ReleaseSafe -Dsanitize=false

test:
    {{zig_build}} test

coverage:
    {{zig_build}} coverage

run-example:
    {{zig_build}} run-example

run-example-args +args:
    {{zig_build}} run-example -- {{args}}

steps:
    {{zig_build}} -l

uninstall:
    {{zig_build}} uninstall

format:
    {{clang_format}} -i {{format_files}}

format-check:
    {{clang_format}} --dry-run --Werror {{format_files}}

clean:
    rm -rf .zig-cache .zig-global-cache zig-out
