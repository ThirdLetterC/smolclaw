# Default port for the server
port := "8080"

# List available commands
default:
	@just --list

# Build the project using Zig
build:
	zig build

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
	zig build valgrind -- {{port}}

# Test the JSON-RPC connection using netcat
test-client:
	nc localhost {{port}}

# Format all source files
fmt:
	zig fmt build.zig
	clang-format -i src/*.c src/*.h
