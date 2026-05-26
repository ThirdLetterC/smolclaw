set shell := ["bash", "-uec"]

zig := env_var_or_default("ZIG", "zig")
clang_format := env_var_or_default("CLANG_FORMAT", "clang-format")

# Show available recipes.
default:
  @just --list

# Build libraries (static + shared by default).
build *args:
  {{zig}} build {{args}}

# Build libraries with sanitizer hardening enabled.
build-sanitize *args:
  {{zig}} build -Dsanitize=true {{args}}

# Build example programs.
examples *args:
  {{zig}} build examples {{args}}

# Remove build artifacts.
clean:
  {{zig}} build clean

# Format C sources with clang-format.
format:
  @files="$(rg --files -g '*.c' -g '*.h' src examples adapters)"; \
  if [ -z "$files" ]; then \
    echo "No source files found."; \
  else \
    {{clang_format}} -i $files; \
  fi
