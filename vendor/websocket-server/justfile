# Default port for the server
port := "8080"

# List available commands
default:
	@just --list

# Build the project using Zig
build:
	zig build

# Run tests
test:
	zig build test

# Run the server (accepts optional port argument)
run p=port:
	zig build run -- {{p}}

# Clean build artifacts
clean:
	rm -rf zig-out zig-cache .zig-cache

# Install dependencies (Ubuntu/Debian example)
deps-apt:
	sudo apt update && sudo apt install -y libuv1-dev zig

# Install dependencies (macOS example)
deps-brew:
	brew install libuv zig

# Check C code for memory leaks using Valgrind (Linux only)
check-leaks:
	zig build -Dcpu=x86_64_v2
	valgrind --leak-check=full --show-leak-kinds=all ./zig-out/bin/ws_server {{port}}

# Test the WebSocket connection using wscat (requires npm install -g wscat)
test-client:
	wscat -c ws://localhost:{{port}}

# Format all source files
fmt:
	zig fmt build.zig
	clang-format -i src/*.c include/websocket-server/*.h examples/*.c testing/*.c
