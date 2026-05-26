WASM_CC ?= clang
WASM_TARGET ?= wasm32-wasip1
BUILD_DIR ?= build
OUT := $(BUILD_DIR)/sc_echo_c.wasm

CFLAGS := --target=$(WASM_TARGET) -std=c23 -Wall -Wextra -Wpedantic -Werror -Os -ffreestanding
LDFLAGS := -nostdlib -Wl,--no-entry -Wl,--allow-undefined \
	-Wl,--export=memory \
	-Wl,--export=sc_plugin_guest_abi_version \
	-Wl,--export=sc_plugin_init \
	-Wl,--export=sc_plugin_shutdown \
	-Wl,--export=sc_echo_c_invoke

.PHONY: all clean manifest

all: $(OUT)

$(OUT): sc_echo_c.c
	mkdir -p $(BUILD_DIR)
	$(WASM_CC) $(CFLAGS) $(LDFLAGS) $< -o $@

manifest: $(OUT)
	sed "s|@ARTIFACT_PATH@|$$(pwd)/$(OUT)|" manifest.template.json > $(BUILD_DIR)/manifest.json

clean:
	rm -rf $(BUILD_DIR)
