# Security Model

Trust boundaries:
- All inputs to the public API (`Arena*`, region pointers, sizes, alignments) are untrusted until validated by the caller and/or this library's parameter checks.
- Any size/alignment values derived from external data remain attacker-controlled at the call site.
- Custom allocator/copy hooks (`ARENA_MALLOC`, `ARENA_FREE`, `ARENA_MEMCPY`, `ARENA_MEMMOVE`) are part of the trusted computing base when overridden.

Attacker model:
- An attacker can influence allocation sizes, alignment values, and allocation cadence through application-level inputs.
- An attacker can attempt integer-overflow edge cases using near-`SIZE_MAX` indexes, sizes, and alignment padding.
- An attacker can induce allocation failure paths by exhausting memory.
- An attacker can trigger repeated clear/copy/allocation transitions to stress allocator state handling.

Protected assets:
- Process memory safety for allocations performed through this arena.
- Arena state consistency (`index`, `size`, region boundaries) during success and failure paths.
- Service availability under malformed or adversarial allocation requests.

Defensive posture:
- Initialization enforces region/size consistency: `arena_init` rejects mismatched `(region == nullptr)` vs `(size == 0)`.
- Creation fails closed: `arena_create(0)` returns `nullptr`; allocation failures return `nullptr` and clean up partial state.
- Allocation APIs fail closed: `arena_alloc_aligned` rejects zero-sized requests, null arena/region pointers, and invalid `index > size` state.
- Alignment validation enforces power-of-two constraints and rejects invalid alignments.
- Index, address, and end-offset math use checked addition (`ckd_add` from `<stdckdint.h>` when available, with local overflow-checked fallbacks otherwise).
- Allocation bounds are enforced before pointer return (`end_index <= arena->size`).
- Failed allocation attempts do not advance `arena->index`.
- Copy operations validate arena pointers/regions and clamp byte count to `min(dest->size, min(src->index, src->size))`.
- Copy uses `memmove` semantics for overlap safety and skips data movement on zero bytes.
- Debug allocation tracking (`ARENA_DEBUG`) is optional and best-effort; metadata allocation failure does not corrupt arena state.
- `arena_clear` resets allocation cursor and frees debug tracking nodes when enabled.
- `arena_destroy` frees debug metadata first, then only frees memory explicitly owned by the arena.
- `arena_init` produces a non-owning arena; `arena_create` produces an owning arena.
- Default allocation path uses zero-initializing `calloc` for arena objects and regions (or mimalloc `mi_calloc` when enabled).
- Optional secure wipe hooks are available for sensitive-memory use cases:
  - `ARENA_SECURE_WIPE(ptr, size)` allows platform/application-specific wipe primitives.
  - `ARENA_SECURE_WIPE_ON_CLEAR` wipes active bytes before cursor reset.
  - `ARENA_SECURE_WIPE_ON_DESTROY` wipes owned regions before release.
- Public APIs return explicit failure values (`nullptr`/`0`) instead of partial-success semantics.

Limitations and caller responsibilities:
- This library is not thread-safe; callers must synchronize shared arenas.
- Sensitive-data zeroization is opt-in; define `ARENA_SECURE_WIPE_ON_CLEAR` and/or
  `ARENA_SECURE_WIPE_ON_DESTROY` (and optionally `ARENA_SECURE_WIPE`) when secrets may reside in arena memory.
- Pointer provenance/lifetime remains a caller contract; passing invalid or stale arena/region pointers is out of scope.
- Overrunning returned arena slices is a caller bug and can still violate memory safety.

Build hardening:
- Build defaults enforce C23/C2x mode with `-Wall -Wextra -Wpedantic -Werror`.
- Hardened builds include stack and libc hardening flags (`-fstack-protector-strong`, `-D_FORTIFY_SOURCE=3`, `-fPIE`, and PIE linking).
- Link hardening in the `justfile` includes RELRO/now (`-Wl,-z,relro,-z,now -pie`).
- Debug/test workflows support sanitizers (`-fsanitize=address,undefined,leak`) and stack traces (`-fno-omit-frame-pointer`).
- Zig debug builds enable C sanitization (`sanitize_c = .full`) for tests/examples.
