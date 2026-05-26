# justfile for toml

set shell := ["bash", "-eu", "-o", "pipefail", "-c"]

ZIG := "zig"

# Format all C sources with clang-format
fmt:
  zig fmt build.zig
  find src examples testing -type f \( -name '*.c' -o -name '*.h' \) -print0 | xargs -0 clang-format -i

format: fmt

# Build the example program
build:
  {{ZIG}} build

# Build and run the example
run: build
  {{ZIG}} build run

# Run the unit tests
test: build
  {{ZIG}} build test

# Clean build artifacts
clean:
  rm -rf zig-cache zig-out examples/simple
