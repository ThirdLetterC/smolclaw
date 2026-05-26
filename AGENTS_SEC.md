# SmolClaw Security Review Instructions

You are reviewing the current SmolClaw codebase, not a generic C project.
Treat every external input as hostile, but keep findings grounded in the code
that exists under this repository.

## Current Codebase Shape

Primary implementation lives in the repository root.

- Public ABI: `include/sc/*.h`.
- First-party implementation: `src/**`.
- Generated first-party code: `src/config/sc_config_schema.c` and `include/sc/sc_config_schema.h`.
- Build system: `CMakeLists.txt` plus `cmake/*.cmake`.
- Tests: `tests/unit/**`, `tests/support/**`, and
  `tests/plugins/**`.
- Validation scripts: `tools/*.py`.
- Config schema inputs: `schema/**`.
- Runtime config fixtures: `tests/fixtures/**` and
  `smolclaw-config/**`.
- Documentation and rules: repository-root `*.md`, and `docs/**`.
- Vendored dependencies: `vendor/**`.

Do not report style or security defects in vendored dependency source as
first-party defects unless the issue is in SmolClaw's wrapper, build flags, or
usage contract. Vendored projects may have their own `AGENTS.md` and
`AGENTS_SEC.md`; follow those only when reviewing that vendor subtree directly.

## Active Build And Tooling Facts

The current CMake project is `SmolClawC`.

- C standard: C23 with extensions disabled.
- C++ standard: C++17 for public-header consumer tests.
- Main static library: `sc_core`.
- Main executable: `smolclaw`.
- Warning policy on first-party C targets includes `-Wall`, `-Wextra`,
  `-Wpedantic`, `-Werror`, `-Wconversion`, `-Wshadow`,
  `-Wstrict-prototypes`, and `-Wmissing-prototypes` when supported.
- Hardening is enabled by default through `SC_HARDENING=ON`.
- Sanitizers are opt-in through `SC_SANITIZERS=ON` and currently enable
  AddressSanitizer, UndefinedBehaviorSanitizer, and LeakSanitizer on
  Clang/GCC-like toolchains.

Use the repository validation script before claiming a change is clean:

```sh
tools/validate.py fast
tools/validate.py sanitizer
tools/validate.py static
tools/validate.py docs
```

`all` runs the full validation sequence:

```sh
tools/validate.py all
```

If a required local tool is unavailable, report the exact command that could
not run and continue with the strongest available checks.

## Current Module Map

Review first-party modules through their actual directories:

- `src/app/` and `src/app/cli/`: process entry and CLI parsing.
- `src/autonomy/`: autonomy policy data and limits.
- `src/channels/`: channel orchestration and Telegram surface.
- `src/config/`: config loading, source handling, schema integration.
- `src/contracts/`: shared public contracts and testable interfaces.
- `src/core/`: allocator, buffer, string, vector, map, result, time, build info.
- `src/gateway/`: gateway control-plane surface.
- `src/hardware/`: hardware/peripheral control abstractions.
- `src/i18n/`: localization catalog handling.
- `src/json/`: JSON wrapper surface.
- `src/media/`: media and voice/audio abstraction surface.
- `src/memory/`: none, markdown, and SQLite memory backends.
- `src/net/`: URL wrapper and network boundary helpers.
- `src/observability/`: logs and observer implementations.
- `src/plugins/`: dynamic C ABI plugin loading and capability checks.
- `src/providers/`: compatible provider layer, HTTP providers, mock provider,
  reliable provider, and provider router.
- `src/registry/`: registries and descriptor lookup.
- `src/runtime/`: agent, bootstrap, and loop logic.
- `src/security/`: approval, audit, domain matching, emergency stop, policy,
  receipts, sandbox, Landlock backend, workspace boundary.
- `src/tools/`: file read, shell, MCP, memory/search tools, wrappers, shared
  tool helpers.

When adding findings or fixes, tie them to these module boundaries and to the
public header contract when one exists.

## Security Review Priorities

Review every first-party C change for these classes, in this order:

1. Policy bypass before side effects in `src/tools/`, `src/runtime/`,
   `src/gateway/`, `src/channels/`, `src/plugins/`, and `src/hardware/`.
2. Workspace escape, symlink, path normalization, path truncation, and TOCTOU
   bugs in file and shell tools.
3. SSRF, unsafe redirects, unsafe scheme handling, DNS/IP policy gaps, and
   unbounded network data in provider, gateway, and URL code.
4. Memory-safety defects: use-after-free, double-free, leaks on error paths,
   invalid frees, out-of-bounds access, stale output parameters, and ownership
   confusion.
5. Integer defects: overflow in allocation or offset math, truncation between
   `size_t` and narrower types, signed/unsigned mismatches, division by zero,
   and invalid shifts.
6. Parser defects in JSON, TOML/config, URL, provider streaming, plugin
   metadata, channel payloads, and gateway requests.
7. Secret disclosure through logs, receipts, errors, metrics labels, fixtures,
   generated files, and docs.
8. ABI safety: public struct size/version checks, opaque object lifetime,
   plugin descriptor validation, callback ownership, and C++ header usability.
9. Resource handling: file descriptors, dynamic library handles, sockets,
   memory backends, locks, cancellation, and timeout cleanup.
10. Build-system regressions that weaken warnings, sanitizers, hardening,
    dependency isolation, or generated-artifact drift checks.

Use `SECURITY.md` for the threat model and trust boundaries. Use
`SECURITY_PATTERNS.md` for required implementation patterns. Use
`MEMORY_OWNERSHIP.md`, `API_CONTRACTS.md`, `PLUGIN_RULES.md`,
`GATEWAY_RULES.md`, `TOOL_RULES.md`, `PROVIDER_RULES.md`,
`MEMORY_BACKEND_RULES.md`, `CONFIG_I18N_RULES.md`, and `HARDWARE_RULES.md`
when the reviewed code touches those areas.

## C23 Security Coding Requirements

First-party C code must stay compatible with the repository C23 policy:

- Use `nullptr`, not `NULL`.
- Use built-in `bool`, `true`, and `false`; do not include `<stdbool.h>`.
- Use `constexpr` for compile-time constants where the compiler supports the
  C23 feature in this codebase.
- Prefer `auto` only where the initializer makes the type obvious.
- Use `[[nodiscard]]` for functions returning owning pointers or statuses that
  must be checked.
- Use `[[maybe_unused]]` and `[[fallthrough]]` for intentional cases.
- Prefer `calloc` for new allocations and check against `nullptr`.
- Use `size_t` for sizes and `ptrdiff_t` for pointer differences.
- Use `<stdckdint.h>` checked arithmetic for allocation sizes, buffer lengths,
  offsets, and counts derived from untrusted input.
- Do not introduce VLAs.
- Do not use `strcpy`, `strcat`, `sprintf`, `gets`, or unbounded `scanf`
  formats.
- Destroy functions must tolerate `nullptr` and partially initialized objects.
- Error paths should leave objects in a documented state and should not update
  persistent state after validation failure.

Generated code may be mechanically shaped by its generator, but the generator
and generated output must still satisfy memory-safety, bounds, and validation
requirements.

## Dependency And Vendor Rules

`vendor/**` is not owned as SmolClaw style code. Security review for
dependencies should focus on:

- Whether `cmake/deps.cmake` compiles the dependency with isolated
  include paths, compatibility headers, and sanitizer support.
- Whether raw dependency types leak into public `include/sc/*.h` headers.
- Whether wrappers enforce size limits, lifetime rules, error mapping, and
  redaction.
- Whether `docs/dependency-inventory.md` stays current for adopted libraries.
- Whether tests cover malicious inputs through the SmolClaw wrapper, not only
  the vendor's own happy path.

Current optional external/library surfaces include vendored or system-backed
JSON, TOML, CLI parsing, logging, cron, URL, RabbitMQ/MQTT/WebSocket pieces,
libcurl, cmark, libuv, mimalloc, and jemalloc depending on configuration.

## Expected Finding Format

For review findings, lead with bugs and risks. Do not bury findings under a
long summary. For each issue, include:

- Severity: Critical, High, Medium, Low.
- Exact file and line.
- A short title describing the broken invariant.
- Impact in concrete terms: policy bypass, workspace escape, memory corruption,
  denial of service, secret leak, ABI break, etc.
- CWE and/or CERT C rule when it is clear.
- Minimal fix direction tied to the local module pattern.
- Test or validation command that should catch the issue.

Use this compact shape:

```text
High - src/tools/shell.c:123 - Policy check happens after command expansion
Impact: attacker-controlled arguments can trigger filesystem/process side
effects before approval denial.
Rule: CWE-863, CWE-78, CERT ENV33-C.
Fix: validate args, build policy request, deny before expansion/execution, then
emit denial receipt. Add a unit test that denial occurs before command setup.
```

If no issues are found, say that clearly and list the checks actually run plus
any unrun residual risk.

## Review Exclusions And Cautions

- Do not flag optional modules merely because documentation mentions behavior
  behind disabled build flags.
- Do not require network credentials, real provider accounts, or real hardware
  for normal tests.
- Do not log or paste real secrets from `smolclaw-config/**`; treat that tree as
  potentially sensitive local configuration.
- Do not modify vendored code directly unless the task explicitly targets a
  vendor subtree.
- Do not weaken warning, sanitizer, or hardening settings to make a review pass.
- Do not convert security denials into success statuses unless the public API
  explicitly models "denied" as a successful policy result and a failed
  operation result.

## Minimum Security Gate For Code Changes

For security-sensitive changes, the minimum acceptable gate is:

```sh
tools/validate.py fast
tools/validate.py sanitizer
```

Also run `static` when touching parsers, allocation logic, public headers,
plugins, tools, gateway, providers, or security policy. Run `docs` when touching
schema, generated docs, dependency inventory, CLI behavior, or public
contracts.
