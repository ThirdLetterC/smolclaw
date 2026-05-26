# Parson

## About
Parson is a lightweight [JSON](http://json.org) parser/serializer for modern C23 projects. It ships as two files (`parson.c` and `parson.h`) with no external dependencies and a stable API.

## Highlights
- Minimal footprint: drop two files into your tree or link the provided static library.
- Dot-notation access for nested fields (`objectA.objectB.value`).
- Comment-tolerant parsing helpers and configurable serialization (allocators, float formatting, slash escaping).
- Deterministic, pretty or compact output.
- Built and tested with `-std=c23 -Wall -Wextra -Wpedantic -Werror`.

## Requirements
- C23-capable compiler and libc.
- [Zig](https://ziglang.org) for the build/test driver and optionally [just](https://just.systems) for command aliases.

## Build and install
- `just build` — compile the static library.
- `just install` — install artifacts to `zig-out` (`zig-out/include/parson.h`, `zig-out/lib/libparson.a`).
- `just test` — run the main test suite.
- `just test-collisions` — stress hash table collision handling (`PARSON_FORCE_HASH_COLLISIONS`).
- `just test-security` — run the dedicated security regression suite.
- Without `just`: `zig build`, `zig build install`, `zig build test`, `zig build test-collisions`, and `zig build test-security`.
- Optional: pass `-Dsanitize=off`, `-Dsanitize=trap`, or `-Dsanitize=full` to Zig builds. Debug defaults to `full`; non-debug defaults to `off`.

## Using Parson
- Copy `parson.c` and `parson.h` into your project and compile them with your own flags, or link against `zig-out/lib/libparson.a` while adding `zig-out/include` to the include path.
- Optional configuration:
  - `json_set_allocation_functions` to supply custom allocators.
  - `json_set_escape_slashes`, `json_set_float_serialization_format`, or `json_set_number_serialization_function` to tune serialization.

## Quick start
```c
#include <stdio.h>
#include "parson.h"

[[nodiscard]] bool print_person() {
    constexpr char payload[] =
        "{\"name\":\"Ada\",\"age\":28,"
        "\"skills\":[\"math\",\"code\"]}";

    auto value = json_parse_string(payload);
    if (value == nullptr) {
        return false;
    }

    auto object = json_value_get_object(value);
    auto skills = json_object_get_array(object, "skills");
    if (object == nullptr || skills == nullptr) {
        json_value_free(value);
        return false;
    }

    printf("%s (%g) first skill: %s\n",
           json_object_get_string(object, "name"),
           json_object_get_number(object, "age"),
           json_array_get_string(skills, 0));

    json_value_free(value);
    return true;
}

int main() {
    return print_person() ? 0 : 1;
}
```

### Building JSON
```c
#include <stdio.h>
#include "parson.h"

[[nodiscard]] char *build_profile() {
    auto root_value = json_value_init_object();
    if (root_value == nullptr) {
        return nullptr;
    }

    auto root_object = json_value_get_object(root_value);
    json_object_set_string(root_object, "name", "Ada Lovelace");
    json_object_set_number(root_object, "age", 36);
    json_object_dotset_string(root_object, "contact.email",
                              "ada@example.com");
    json_object_dotset_value(
        root_object, "skills",
        json_parse_string("[\"math\",\"analysis\",\"computing\"]"));

    auto serialized = json_serialize_to_string_pretty(root_value);
    json_value_free(root_value);
    return serialized;
}

int main() {
    auto json = build_profile();
    if (json != nullptr) {
        puts(json);
        json_free_serialized_string(json);
        return 0;
    }
    return 1;
}
```

## Contributing
Bug fixes are always welcome. For API changes, open an issue first so we can agree on the direction before you invest in an implementation. Match the existing code style, include tests for new behavior, and add focused cases to the security regression suite when the change affects parsing, serialization, memory ownership, or global configuration hooks.

## License
[MIT](LICENSE)
