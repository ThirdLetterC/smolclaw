set shell := ["bash", "-cu"]

default := ("test")

build:
    zig build

install:
    zig build install

fmt: format

format:
    clang-format -i src/*.c include/parson/*.h examples/*.c

# Run the main test suite (pass extra args after `--`).
test *args:
    zig build test -Dsanitize=full -- {{args}}

# Run the collision-focused test variant (pass extra args after `--`).
test-collisions *args:
    zig build -Dsanitize=full test-collisions -- {{args}}

# Run the security regression suite (pass extra args after `--`).
test-security *args:
    zig build -Dsanitize=full test-security -- {{args}}

clean:
    rm -rf zig-out zig-cache
