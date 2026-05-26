# Configuration

SmolClaw config is schema-backed. The checked-in source of truth is
`schema/fields.yaml`; regenerate `src/config/sc_config_schema.c` and
`include/sc/sc_config_schema.h`
with:

```sh
python3 tools/gen_config_schema.py schema/fields.yaml src/config/sc_config_schema.c include/sc/sc_config_schema.h
```

Use `smolclaw config show` to inspect the effective values and `smolclaw
init-config` or `smolclaw onboard` to create editable starter configs.

## Runtime Limits

The `[agent]` section controls turn-level limits:

```toml
[agent]
max_history_messages = 50
max_tool_iterations = 10
max_tool_result_chars = 50000
max_system_prompt_chars = 0
```

Values must be non-negative, except `max_tool_iterations`, which must be at
least `1`.

## RTK Shell Output Compression

RTK compression is optional and applies only to shell-tool command output. It
does not wrap provider subprocesses, browser helpers, PDF extraction, or other
machine-readable process output.

```toml
[tools.rtk]
enabled = false
command = "rtk"
ultra_compact = true
fallback_passthrough = true
allowed_commands = ["ls", "tree", "cat", "head", "tail", "rg", "grep", "find", "git", "gh", "pytest", "cargo", "go", "npm", "pnpm", "yarn", "docker", "kubectl", "curl", "wget"]
```

When enabled, SmolClaw wraps only simple commands whose basename appears in
`allowed_commands`. Commands with shell-control operators run unchanged.
If `command` is unavailable, `fallback_passthrough = true` keeps the original
shell command running and logs `sc.shell_tool.rtk_unavailable`; set it to
`false` to fail instead.

## Sandbox

The `[security.sandbox]` section controls process sandbox selection and
container execution:

```toml
[security.sandbox]
backend = "auto"
network = "full"
image_name = "alpine:3.23.4"
fallback_order = ["landlock", "bubblewrap", "firejail", "docker", "podman"]
allow_noop_fallback = false
docker_path = ""
podman_path = ""
container_runtime = ""
```

`allow_noop_fallback` is deliberately false by default. Set it only for
explicit insecure test profiles.

## Channels

Shared channel behavior lives under `[channels]`:

```toml
[channels]
session_persistence = true
session_backend = "sqlite"
ack_reactions = false
show_tool_calls = false
max_seen_message_ids = 1024
```

Telegram-specific timing and message-size knobs live under
`[channels.telegram]`:

```toml
[channels.telegram]
poll_timeout_seconds = 30
message_split_bytes = 3900
draft_update_interval_ms = 1000
approval_timeout_secs = 120
```

## Heartbeat

Heartbeat state is opt-in and workspace-relative by default:

```toml
[heartbeat]
enabled = false
state_path = "heartbeat.state"
```

Absolute `state_path` values are used as-is; relative paths are resolved under
the active workspace.
