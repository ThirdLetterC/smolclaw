# Run Telegram Bot With OpenAI

This runbook starts the C23 SmolClaw runtime with the Telegram channel and the OpenAI chat-completions provider.

## Prerequisites

- C dependencies available through the root CMake setup, including libcurl and parson.
- A Telegram bot token from BotFather.
- An OpenAI API key exported in the shell.
- A local config file that is not committed with real credentials.

Use environment variables for credentials:

```bash
export OPENAI_API_KEY='replace-with-your-openai-key'
export SMOLCLAW_TELEGRAM_BOT_TOKEN='replace-with-your-telegram-bot-token'
```

The Telegram bootstrap reads `channels.telegram.bot_token_env` first and only falls back to `channels.telegram.bot_token` when the env var is missing or empty.

## Build

From the repository root:

```bash
cmake -S . -B build/smolclaw-c -DSC_CHANNEL_TELEGRAM=ON
cmake --build build/smolclaw-c --target smolclaw -j
```

## Config

Create a local config, for example:

```toml
schema_version = 2

[providers]
fallback = "openai"

[providers.models.openai]
model = "gpt-4o-mini"
credential_env = "OPENAI_API_KEY"
# Optional; the C OpenAI adapter defaults to this endpoint when omitted.
base_url = "https://api.openai.com/v1/chat/completions"

[reliability]
max_retries = 2
retry_backoff_ms = 500
fallback_providers = []

[channels]
session_persistence = true
session_backend = "sqlite"
ack_reactions = true
show_tool_calls = false
max_seen_message_ids = 1024

[channels.telegram]
enabled = true
bot_token_env = "SMOLCLAW_TELEGRAM_BOT_TOKEN"
bot_token = ""
allowed_users = ["your_telegram_username_or_sender_id"]
mention_only = false
interrupt_on_new_message = false
stream_mode = "off"
draft_update_interval_ms = 1000
approval_timeout_secs = 120

[memory]
backend = "sqlite"
```

Notes:

- `allowed_users` is matched against the sender value parsed by the C Telegram channel. For current polling updates this can be the numeric Telegram sender id; username-based matching is supported when the update provides a username.
- `stream_mode = "off"` is the safest first run. When streaming is enabled, Telegram draft updates use the Bot API `sendMessageDraft` method and fall back to the older send/edit flow only if the draft API is unavailable for the chat.
- Session state is written under the workspace path as `sessions/sessions.db` plus JSONL transcripts.

## Run

Use `smolclaw chat`; the C bootstrap starts when `SMOLCLAW_CONFIG` is set:

```bash
export SMOLCLAW_CONFIG="$PWD/smolclaw-data/tg-openai/config.toml"
export SMOLCLAW_WORKSPACE="$PWD/smolclaw-data/tg-openai/workspace"

build/smolclaw-c/smolclaw chat
```

The Docker image runs the same long-lived `chat` command:

```bash
docker build -t smolclaw:telegram .
docker run --rm \
  -e SMOLCLAW_CONFIG=/etc/smolclaw/config.toml \
  -e SMOLCLAW_WORKSPACE=/var/lib/smolclaw \
  -e OPENAI_API_KEY \
  -e SMOLCLAW_TELEGRAM_BOT_TOKEN \
  -v "$PWD/smolclaw-data/tg-openai/config.toml:/etc/smolclaw/config.toml:ro" \
  -v "$PWD/smolclaw-data/tg-openai/workspace:/var/lib/smolclaw" \
  smolclaw:telegram
```

For a one-poll smoke test:

```bash
SMOLCLAW_ONCE=1 SMOLCLAW_MAX_POLLS=1 build/smolclaw-c/smolclaw chat
```

For richer logs:

```bash
SC_LOG=trace build/smolclaw-c/smolclaw chat
```

## Smoke Test

1. Send a Telegram message to the bot.
2. Confirm the process logs a Telegram poll and provider call.
3. Confirm the bot sends a final reply.
4. Check persistence:

```bash
sqlite3 "$SMOLCLAW_WORKSPACE/sessions/sessions.db" \
  'select session_key, role, substr(content,1,80) from sessions order by id desc limit 10;'

ls "$SMOLCLAW_WORKSPACE/sessions/"*.jsonl
```

## Troubleshooting

- `sc.provider_credential.missing_env`: `OPENAI_API_KEY` is not exported in the process environment.
- `sc.channel_telegram.token_missing`: `SMOLCLAW_TELEGRAM_BOT_TOKEN` is empty or `channels.telegram.bot_token` is empty.
- No messages arrive: verify the bot token, send a direct message to the bot first, and check whether `allowed_users` matches the parsed sender id/username.
- OpenAI HTTP error: verify `model`, account access, key validity, and `base_url`.
- Duplicate messages are skipped by message id until the bounded `channels.max_seen_message_ids` cache evicts old ids.
