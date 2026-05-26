# C23 ABI Policy

This policy defines the public ABI rules for the repository root.

## ABI Scope

The ABI includes:

- Public headers under `include/sc/`.
- Exported symbols from shared libraries or dynamic modules.
- Plugin descriptors and plugin entry points.
- Structs, enums, callbacks, and vtables passed across module, plugin, or
  embedder boundaries.
- Ownership and cleanup contracts that external callers rely on.

Private headers under `include/<subsystem>/` or beside subsystem sources are
not ABI. They may change freely within a release as long as public contracts and
tests continue to pass.

## ABI Version

Define one project ABI version constant in the public base header:

```c
#define SC_ABI_VERSION_MAJOR 0u
#define SC_ABI_VERSION_MINOR 1u
#define SC_ABI_VERSION_PATCH 0u
#define SC_ABI_VERSION ((SC_ABI_VERSION_MAJOR << 16) | \
                        (SC_ABI_VERSION_MINOR << 8) | \
                        SC_ABI_VERSION_PATCH)
```

Generated `include/sc/version.h` defines the exact value and is included by
`sc/api.h`, so external consumers can read the current ABI version without
depending on build-system internals.

Before `SC_ABI_VERSION_MAJOR` reaches `1`, ABI-breaking changes are allowed but
must be recorded in the changelog. After `1.0.0`, breaking ABI changes require
a major version increment and compatibility notes.

## Public Struct Rules

Public structs crossing module or plugin boundaries must be versioned or
size-tagged:

```c
typedef struct sc_plugin_descriptor {
    size_t size;
    uint32_t abi_version;
    sc_str name;
    sc_str version;
    uint64_t capabilities;
} sc_plugin_descriptor;
```

Rules:

- Initialize `size` to `sizeof(struct_type)`.
- Check `size` before reading appended fields.
- Append fields only at the end.
- Do not reorder fields.
- Do not change field meaning without an ABI version change.
- Keep appended fields optional until the minimum supported size includes them.

Concrete provider, channel, tool, memory, observer, runtime, peripheral,
sandbox, gateway, or plugin implementation structs must remain private.

## Opaque Handles And Vtables

Public subsystem objects use opaque handles:

```c
typedef struct sc_provider sc_provider;
```

C contract-like behavior is represented with `const` vtables. The base handle
contains the vtable pointer as its first field when runtime dispatch requires
it. Required vtable functions must be non-null at registration. Optional
functions must be checked before dispatch and return `SC_ERR_UNSUPPORTED` or a
documented equivalent when absent.

Destroy functions must tolerate `nullptr` and partially initialized objects.

## Dependency Boundary

Public headers must not expose dependency types, constants, callbacks,
allocator types, or error codes. Public headers may include standard C headers
and other `sc/*.h` headers only.

Wrappers translate dependency behavior into SmolClaw-owned values,
`sc_status`, and explicit cleanup functions.

## Plugin Compatibility Checks

Every plugin load must verify:

- Expected symbol names are present.
- Descriptor `size` is large enough for fields the host reads.
- Descriptor `abi_version` is compatible with the host.
- Required vtable entries are non-null.
- Requested capabilities are known and granted by policy.
- Plugin-owned memory is not freed by the host unless the plugin contract
  explicitly supplies a compatible release function.

Unknown ABI major versions are rejected. Newer minor versions may load only if
all fields used by the host are present and the plugin does not require unknown
mandatory capabilities.

## Deprecation Process

Before removing or changing public ABI:

1. Mark the API deprecated in documentation and, where portable, with compiler
   attributes.
2. Provide the replacement API.
3. Keep tests for both old and new behavior during the deprecation window.
4. Record removal timing in the changelog.
5. Remove only on a major ABI version change after `1.0.0`.

Pre-`1.0.0` removals are allowed, but still require notes so plugin and embedder
authors can follow the churn.

## Compiler And Header Compatibility

Public headers must remain consumable by the first milestone compiler matrix:

- Clang for Linux and macOS.
- GCC for Linux.

Use CMake feature probes and compatibility macros for C23 features that are not
uniformly available. Do not rely on newer C23 syntax in public headers until the
compiler matrix proves support.

Avoid public-header use of:

- Variable length arrays.
- Compiler-specific extensions without a documented fallback.
- Dependency headers.
- Concrete implementation structs.
- Raw owning `char *` returns without a status and cleanup contract.

## ABI Validation

As the build system matures, CI should add:

- Public header compile tests.
- C++ consumer compile smoke tests where reasonable.
- Exported symbol checks for shared builds.
- Plugin descriptor compatibility tests.
- Struct size/version tests.
- ABI drift reports before stable releases.
