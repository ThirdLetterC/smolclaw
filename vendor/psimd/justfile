set shell := ["sh", "-cu"]

cc := env_var_or_default("CC", "clang")
cflags := env_var_or_default("CFLAGS", "-O2 -std=c23 -Wall -Wextra -Wpedantic -Werror")
sanitize_cflags := env_var_or_default("SANITIZE_CFLAGS", "-O1 -g -std=c23 -Wall -Wextra -Wpedantic -Werror -fsanitize=address,undefined,leak -fno-omit-frame-pointer")
ldflags := env_var_or_default("LDFLAGS", "-lm")
sanitize_ldflags := env_var_or_default("SANITIZE_LDFLAGS", "-lm -fsanitize=address,undefined,leak")
build_dir := "build"
sources := "psimd.h test.c"

# List available commands.
default:
  @just --list

# Run the local test suite.
test: test-all

# Build and run scalar, SSE4.1, and AVX2+FMA tests.
test-all: test-scalar test-sse41 test-avx2-if-supported

# Compile the scalar test binary.
build-scalar:
  mkdir -p {{build_dir}}
  {{cc}} {{cflags}} -DPSIMD_FORCE_SCALAR test.c {{ldflags}} -o {{build_dir}}/test-scalar

# Run the scalar test binary.
test-scalar: build-scalar
  ./{{build_dir}}/test-scalar

# Compile the scalar test binary with sanitizers.
build-sanitize:
  mkdir -p {{build_dir}}
  {{cc}} {{sanitize_cflags}} -DPSIMD_FORCE_SCALAR test.c {{sanitize_ldflags}} -o {{build_dir}}/test-sanitize

# Run the scalar sanitizer test binary.
test-sanitize: build-sanitize
  ./{{build_dir}}/test-sanitize

# Compile the SSE4.1 test binary.
build-sse41:
  mkdir -p {{build_dir}}
  {{cc}} {{cflags}} -msse4.1 test.c {{ldflags}} -o {{build_dir}}/test-sse41

# Run the SSE4.1 test binary.
test-sse41: build-sse41
  ./{{build_dir}}/test-sse41

# Compile the AVX2+FMA test binary.
build-avx2:
  mkdir -p {{build_dir}}
  {{cc}} {{cflags}} -mavx2 -mfma test.c {{ldflags}} -o {{build_dir}}/test-avx2

# Run the AVX2+FMA test binary.
test-avx2: build-avx2
  ./{{build_dir}}/test-avx2

# Run AVX2+FMA tests only when the current CPU supports them.
test-avx2-if-supported:
  if grep -qw avx2 /proc/cpuinfo 2>/dev/null && grep -qw fma /proc/cpuinfo 2>/dev/null; then \
    just test-avx2; \
  else \
    echo "Skipping AVX2+FMA tests: CPU support not detected."; \
  fi

# Build every local test binary.
build: build-scalar build-sse41 build-avx2

# Format C sources with clang-format.
format:
  clang-format -i {{sources}}

# Check C source formatting.
format-check:
  clang-format --dry-run --Werror {{sources}}

# Remove local build outputs.
clean:
  rm -rf {{build_dir}} test
