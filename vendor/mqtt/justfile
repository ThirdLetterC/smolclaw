set shell := ["bash", "-euo", "pipefail", "-c"]

os := `uname -o 2>/dev/null || uname -s`
cc := env_var_or_default("CC", "gcc")
debug := env_var_or_default("DEBUG", "0")
sanitizers := env_var_or_default("SANITIZERS", "-fsanitize=address -fsanitize=undefined -fsanitize=leak")
std_probe := `{{cc}} -std=c23 -E -x c /dev/null >/dev/null 2>&1 && echo c23 || echo c2x`
std_flag := "-std=" + std_probe
cflags_base := std_flag + " -Wall -Wextra -Wpedantic -Werror -D_POSIX_C_SOURCE=200809L -Iinclude"
cflags := if debug == "1" { cflags_base + " -g " + sanitizers } else { cflags_base }
ldflags := if debug == "1" { sanitizers } else { "" }
msflags := if os == "Msys" { "-lws2_32" } else { "" }
sources := "src/mqtt.c src/mqtt_pal.c"
bindir := "bin"

alias default := all
alias test := check

all: build-tests build-examples

build-examples: simple-examples reconnect-examples openssl-examples

build-tests: bin-dir
    {{cc}} {{cflags}} testing/tests.c {{sources}} {{msflags}} {{ldflags}} -o {{bindir}}/tests

simple-examples: bin-dir
    for example in simple_publisher simple_subscriber; do {{cc}} {{cflags}} examples/${example}.c {{sources}} -lpthread {{msflags}} {{ldflags}} -o {{bindir}}/${example}; done

reconnect-examples: bin-dir
    for example in reconnect_subscriber; do {{cc}} {{cflags}} examples/${example}.c {{sources}} -lpthread {{msflags}} {{ldflags}} -o {{bindir}}/${example}; done

openssl-examples: bin-dir
    for example in openssl_publisher; do {{cc}} {{cflags}} $(pkg-config --cflags openssl) -D MQTT_USE_BIO examples/${example}.c {{sources}} -lpthread {{msflags}} $(pkg-config --libs openssl) {{ldflags}} -o {{bindir}}/${example}; done

check: build-tests
    ./{{bindir}}/tests

format:
    clang-format -i --style=file src/*.c include/mqtt/mqtt.h examples/*.c examples/templates/*.h testing/tests.c

fmt: format

clean:
    rm -rf {{bindir}} zig-out .zig-cache *.o

bin-dir:
    mkdir -p {{bindir}}
