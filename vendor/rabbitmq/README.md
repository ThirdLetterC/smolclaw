# rabbitmq

Minimal RabbitMQ C23 client.

## Highlights

- Strict C23 codebase with `nullptr`, `constexpr`, `auto`, and standard attributes.
- Zero-warning policy: `-std=c23 -Wall -Wextra -Wpedantic -Werror`.
- Focused core library with examples, tools, and tests.

## Requirements

- Zig toolchain (recommended) or a C23-capable compiler.
- A RabbitMQ server for integration tests and examples.

## Build

Use the provided Zig build:

```
zig build
```

## Layout

- `include/` public headers
- `src/` library sources
- `examples/` and `tools/` sample programs
- `tests/` test suites

## Usage

Headers are exported under `include/rabbitmq/`. Start with:

```
#include "rabbitmq/amqp.h"
```

Examples live in `examples/` and `tools/` and are built by the Zig build targets above.

## Contributing

- Keep code strictly C23 and prefer modern features (`nullptr`, `constexpr`, `auto`).
- Avoid legacy APIs (`strcpy`, `strcat`, `sprintf`).
- Maintain the zero-warning policy for new code.
