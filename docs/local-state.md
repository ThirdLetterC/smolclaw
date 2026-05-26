# SmolClaw Local State

SmolClaw is local-first. The runtime has no telemetry, hosted callbacks, license checks, or cloud tenancy assumptions. Remote traffic is limited to explicitly configured model providers, configured channels, configured network tools, or dependencies the operator enables.

Default locations:

| State | Location |
|---|---|
| Config | `$SMOLCLAW_CONFIG`, otherwise `smolclaw.toml`. |
| Secrets | Environment variables named by config, or secret config fields that are redacted by config export and doctor output. |
| Workspace | `$SMOLCLAW_WORKSPACE`, otherwise `workspace/` beside the config file. |
| Memory | `workspace/memory/brain.db` when SQLite memory is enabled; `none` has no durable memory state. |
| Sessions | `workspace/sessions/sessions.db` and `workspace/sessions/*.jsonl` when channel persistence is enabled. |
| Receipts | `workspace/receipts/` for durable receipt chains when a caller saves them. In-process receipt chains stay memory-only. |
| Logs | Standard error for foreground runs, or the service manager journal/event log when run as a service. |
| Caches | `workspace/cache/` for explicit local caches. No provider cache is created unless configured. |
| Emergency stop | `workspace/state/emergency_stop.state`. |
| Heartbeat | `workspace/heartbeat.state` when heartbeat output is enabled. |

Examples and fixtures use placeholder credentials and neutral payloads only. Do not put real provider keys, raw prompts, private channel messages, customer data, authorization headers, pairing codes, or OTP material into examples, fixtures, generated docs, receipts, metrics labels, or logs.
