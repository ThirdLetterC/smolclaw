# SmolClaw Local Media Services

SmolClaw can use local CPU-only media services for speech-to-text and
text-to-speech:

- Piper TTS on `http://127.0.0.1:5000`
- whisper.cpp ASR on `http://127.0.0.1:2022/v1/audio/transcriptions`

The Dockerfiles bake in the default models, so the basic setup does not require
runtime model mounts.

## Build Images

From the repository root:

```sh
docker build -f Dockerfile.piper -t smolclaw-piper .
docker build -f Dockerfile.whisper -t smolclaw-whisper .
```

## Run Services

Run Piper TTS:

```sh
docker run -d --rm -p 5000:5000 --name smolclaw-piper smolclaw-piper
```

Run whisper.cpp:

```sh
docker run -d --rm -p 2022:2022 --name smolclaw-whisper smolclaw-whisper
```

## Verify Services

Check Piper health and synthesize a WAV:

```sh
curl -fsS http://127.0.0.1:5000/healthz
curl -fsS \
  -H 'Content-Type: application/json' \
  -d '{"text":"hello world"}' \
  http://127.0.0.1:5000/ \
  -o hello.wav
```

Check whisper.cpp health and transcribe the WAV:

```sh
curl -fsS http://127.0.0.1:2022/health
curl -fsS \
  -F file=@hello.wav \
  -F model=whisper-1 \
  -F response_format=json \
  http://127.0.0.1:2022/v1/audio/transcriptions
```

## SmolClaw Config

Add this to the SmolClaw TOML config:

```toml
[media.asr]
enabled = true
backend = "whisper_cpp"

[media.asr.whisper]
endpoint_url = "http://127.0.0.1:2022/v1/audio/transcriptions"
model = "whisper-1"
response_format = "json"
timeout_ms = 30000
max_audio_bytes = 26214400
max_response_bytes = 65536

[media.tts]
enabled = true
backend = "piper"
reply_mode = "text_and_audio"

[media.tts.piper]
base_url = "http://127.0.0.1:5000"
timeout_ms = 30000
max_audio_bytes = 8388608
```

The endpoint URLs above match SmolClaw's defaults. The required switches are
`media.asr.enabled = true` and `media.tts.enabled = true`.

## Runtime Model Overrides

The default images include:

- Piper voice: `/voices/en_US-lessac-medium.onnx`
- whisper.cpp model: `/models/ggml-base.bin`

To use a different Piper voice:

```sh
docker run --rm -p 5000:5000 \
  -v "$PWD/voices:/voices:ro" \
  -e PIPER_MODEL=/voices/custom.onnx \
  --name smolclaw-piper \
  smolclaw-piper
```

Make sure the matching Piper config exists beside the model, usually
`custom.onnx.json`.

To use a different whisper.cpp model:

```sh
docker run --rm -p 2022:2022 \
  -v "$PWD/models:/models:ro" \
  -e WHISPER_MODEL=/models/ggml-small.bin \
  --name smolclaw-whisper \
  smolclaw-whisper
```

Mounting `/voices` or `/models` hides the baked-in files, so the mounted
directory must contain the selected model files.

## Docker Networking

SmolClaw currently allows these local media endpoints only on loopback HTTP
hosts such as `127.0.0.1` and `localhost`.

If SmolClaw runs directly on the host, the `-p` commands above are enough.

If SmolClaw itself runs inside Docker on Linux, run the SmolClaw container with
host networking so `127.0.0.1:5000` and `127.0.0.1:2022` resolve from the
SmolClaw process:

```sh
docker run --rm --network host smolclaw
```

## Useful Environment Variables

Piper:

- `PIPER_HOST`, default `0.0.0.0`
- `PIPER_PORT`, default `5000`
- `PIPER_MODEL`, default `/voices/en_US-lessac-medium.onnx`
- `PIPER_VOICES_DIR`, default `/voices`
- `PIPER_MAX_TEXT_BYTES`, default `65536`

whisper.cpp:

- `WHISPER_HOST`, default `0.0.0.0`
- `WHISPER_PORT`, default `2022`
- `WHISPER_MODEL`, default `/models/ggml-base.bin`
- `WHISPER_THREADS`, default `4`
- `WHISPER_PROCESSORS`, default `1`
- `WHISPER_INFERENCE_PATH`, default `/v1/audio/transcriptions`
- `WHISPER_TMPDIR`, default `/tmp/whisper`
