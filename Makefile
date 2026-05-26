.DEFAULT_GOAL := help

CMAKE ?= cmake
CTEST ?= ctest
PYTHON ?= python3.14
PYTHON_EXECUTABLE ?= $(shell command -v $(PYTHON) 2>/dev/null)
PYTHON_CMAKE_ARG := $(if $(PYTHON_EXECUTABLE),-DPython3_EXECUTABLE=$(PYTHON_EXECUTABLE),)

BUILD_DIR ?= build/c23
SANITIZER_BUILD_DIR ?= build/asan
BUILD_TYPE ?= Debug
CONFIG ?= $(BUILD_TYPE)
TARGET ?=
PRESET ?= release
VALIDATE_MODE ?= fast

JOBS ?= $(shell nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)
CMAKE_ARGS ?=
BUILD_ARGS ?=
CTEST_ARGS ?=

SMOLCLAW := $(BUILD_DIR)/smolclaw
VALIDATE := tools/validate.py
CLANG_TIDY := tools/run_clang_tidy.py

.PHONY: help
help: ## Show common tasks.
	@awk 'BEGIN {FS = ":.*##"; printf "Common SmolClaw tasks:\n"} /^[a-zA-Z0-9_.-]+:.*##/ {printf "  %-22s %s\n", $$1, $$2}' $(MAKEFILE_LIST)

.PHONY: configure
configure: ## Configure the default debug build.
	$(CMAKE) -S . -B $(BUILD_DIR) -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) $(PYTHON_CMAKE_ARG) $(CMAKE_ARGS)

.PHONY: build
build: configure ## Build the default tree. Override TARGET=name for one target.
	$(CMAKE) --build $(BUILD_DIR) --config $(CONFIG) --parallel $(JOBS) $(if $(TARGET),--target $(TARGET),) $(BUILD_ARGS)

.PHONY: test
test: build ## Run CTest for the default tree.
	$(CTEST) --test-dir $(BUILD_DIR) --output-on-failure $(CTEST_ARGS)

.PHONY: run
run: build ## Run the smolclaw executable. Pass ARGS="..." for CLI arguments.
	$(SMOLCLAW) $(ARGS)

.PHONY: fast
fast: ## Run the project fast validation baseline.
	$(PYTHON) $(VALIDATE) fast

.PHONY: docs
docs: ## Run dependency inventory and generated-doc validation.
	$(PYTHON) $(VALIDATE) docs

.PHONY: static
static: ## Run Ruff, cppcheck, and clang-tidy validation.
	$(PYTHON) $(VALIDATE) static

.PHONY: sanitizer
sanitizer: ## Configure, build, and test with sanitizers enabled.
	BUILD_DIR=$(SANITIZER_BUILD_DIR) $(PYTHON) $(VALIDATE) sanitizer

.PHONY: validate
validate: ## Run validation mode. Override VALIDATE_MODE=fast|static|sanitizer|docs|all.
	$(PYTHON) $(VALIDATE) $(VALIDATE_MODE)

.PHONY: all-checks
all-checks: ## Run the full validation suite.
	$(PYTHON) $(VALIDATE) all

.PHONY: preset
preset: ## Configure and build a CMake preset. Override PRESET=release|wamr-debug.
	$(CMAKE) --preset $(PRESET) $(PYTHON_CMAKE_ARG)
	$(CMAKE) --build --preset $(PRESET) --parallel $(JOBS) $(BUILD_ARGS)

.PHONY: release
release: ## Configure and build the release preset.
	$(CMAKE) --preset release $(PYTHON_CMAKE_ARG)
	$(CMAKE) --build --preset release --parallel $(JOBS) $(BUILD_ARGS)

.PHONY: wamr-debug
wamr-debug: ## Configure and build the WAMR debug preset.
	$(CMAKE) --preset wamr-debug $(PYTHON_CMAKE_ARG)
	$(CMAKE) --build --preset wamr-debug --parallel $(JOBS) $(BUILD_ARGS)

.PHONY: tidy
tidy: ## Run the clang-tidy helper directly.
	$(PYTHON) $(CLANG_TIDY)

.PHONY: clean
clean: ## Clean the default build tree.
	$(CMAKE) --build $(BUILD_DIR) --target clean $(BUILD_ARGS)

.PHONY: distclean
distclean: ## Remove generated build trees used by this Makefile.
	$(CMAKE) -E rm -rf $(BUILD_DIR) $(SANITIZER_BUILD_DIR) build/validate build/validate-sanitizer build/release build/wamr
