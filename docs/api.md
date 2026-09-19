# Mica API reference

Mica exposes one OpenAI-compatible base URL for every model in the active
profile. Unless otherwise noted, routes require a bearer token.

```sh
export MICA_BASE_URL=http://127.0.0.1:8080
export MICA_API_KEY="$(< "$HOME/.mica/secrets/api-key")"
```

Send authentication as:

```text
Authorization: Bearer <token>
```

Setup generates a random token by default. To provide one without exposing it
in shell history, store it in a mode-0600 file and use
`setup --api-key-file PATH`. `serve --api-key-file PATH` provides a runtime
override suitable for rotation and secret managers. `serve --api-key TOKEN`
also works but may expose the token in shell history and process listings.
The same `api_key` or `api_key_file` fields may be set in
`~/.mica/config/server.json`; CLI values take precedence. Tokens must contain
16 to 512 printable characters.

## Health and discovery

| Method and route | Authentication | Purpose |
| --- | --- | --- |
| `GET /health` | No | Process liveness. |
| `GET /ready` | No | Warmup/readiness; returns 503 while required startup work fails. |
| `GET /v1/models` | Yes | Models and variants enabled by the active profile. |
| `GET /v1/catalog` | Yes | Complete curated registry, filterable by modality, capability, engine, or artifact family. |
| `GET /admin/models` | Yes | Active profile, memory budget, policies, and resident workers. |

Registry examples:

```sh
curl "$MICA_BASE_URL/v1/catalog?modality=asr&engine=mlx-audio" \
  -H "Authorization: Bearer $MICA_API_KEY"

./build/mica-server registry list --modality video-text-to-text --engine mlx-vlm
./build/mica-server registry ping --modality asr
```

`registry ping` performs a remote availability check for each selected curated
Hugging Face repository. It does not download weights.

`engine` selects a concrete runtime such as `mlx-lm`, `mlx-vlm`, `mlx-audio`,
`llama-cpp`, or `audio-cpp`. The legacy `backend` filter selects an artifact
family (`mlx`, `gguf`, or `vllm`). Each schema-2 ledger entry includes its
description, modalities, license, repositories/revisions, and variant records
with format, exact quantization type, artifact/download bytes, projector size,
size provenance, and memory reservation.

## Model IDs

An unsuffixed ID, such as `spark-x25-4b`, resolves to the exact execution
selected by the active schema-2 profile. The explicit form is:

```text
model-id@backend:quantization
```

For example, `spark-x25-4b@gguf:q4`. An explicit variant is rejected unless it
matches the active profile.

## Chat completions

`POST /v1/chat/completions`

```sh
curl "$MICA_BASE_URL/v1/chat/completions" \
  -H "Authorization: Bearer $MICA_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "spark-x25-4b",
    "messages": [{"role": "user", "content": "Explain unified memory."}],
    "temperature": 0.2,
    "max_tokens": 256,
    "stream": false
  }'
```

Set `stream` to `true` for OpenAI-style server-sent completion chunks. The
profile's output-token ceiling is enforced before forwarding the request.

`POST /v1/completions` is a legacy adapter. It accepts one string `prompt`,
translates it to a user chat message, and returns `choices[].text`.

## Speech recognition

`POST /v1/audio/transcriptions` accepts multipart form data:

```sh
curl "$MICA_BASE_URL/v1/audio/transcriptions" \
  -H "Authorization: Bearer $MICA_API_KEY" \
  -F model=granite-speech-5 \
  -F file=@sample.wav
```

The direct route proxies the selected backend's response. The agent streaming
route adds stable transcript events for the chat application.

## Text to speech

`POST /v1/audio/speech`

```sh
curl "$MICA_BASE_URL/v1/audio/speech" \
  -H "Authorization: Bearer $MICA_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "audio8-tts-06b",
    "input": "Mica is ready.",
    "response_format": "wav"
  }' \
  --output reply.wav
```

For direct voice cloning, MLX workers accept `ref_audio` and `ref_text`; GGUF
workers accept `voice_ref` and `reference_text`. The agent route normalizes
these differences through `tts_voice` and `tts_voice_text` multipart fields.

## Multimodal agent

`POST /v1/agent/chat` accepts multipart form data. Common fields are:

| Field | Meaning |
| --- | --- |
| `session_id` | Optional stable ID using letters, digits, dot, dash, or underscore. |
| `text` | Typed instruction. |
| `voice` | Recorded instruction or supporting voice context. |
| `files` | Repeatable image, video, or PDF attachment. |
| `llm_model` | Main reasoning model; defaults to Spark. |
| `asr_model` | Voice transcription model; defaults to Granite. |
| `vlm_model` | Media-analysis model; defaults to MiniCPM-V. |
| `tts_model` | Reply voice model; defaults to Audio8. |
| `tts_voice` | Optional voice-reference audio. |
| `tts_voice_text` | Exact transcript required with `tts_voice`. |
| `llm_system_prompt` | Optional main-assistant persona or behavior prompt. |
| `vlm_system_prompt` | Optional media-analysis behavior prompt. |

Each custom system prompt is limited to 16 KiB. Its content is otherwise
user-defined. Mica independently retains its infrastructure contract: the LLM
must route attached media through the bounded VLM tool, attachment paths remain
allowlisted, and instructions found inside media are treated as untrusted data.

```sh
curl "$MICA_BASE_URL/v1/agent/chat" \
  -H "Authorization: Bearer $MICA_API_KEY" \
  -F session_id=demo \
  -F text='Summarize the attachment.' \
  -F llm_system_prompt='Answer concisely in Spanish.' \
  -F files=@document.pdf
```

If only voice is supplied, its transcript is the instruction. If text and
voice are supplied, text is the instruction and the transcript is supporting
context. Voice-originated turns receive both text and one final WAV response.

`POST /v1/agent/chat/stream` returns server-sent events:

| Event | Meaning |
| --- | --- |
| `state` | Current transient state. |
| `transcript_delta`, `transcript_end` | Live ASR output. |
| `attachments` | Accepted media manifest. |
| `tool_call`, `tool_result` | Bounded VLM-tool activity. |
| `text_delta`, `text_end` | Main-agent response. |
| `audio_chunk`, `audio_end` | Playable TTS chunks and finalization. |
| `done` | Complete persisted result. |
| `error` | Terminal agent error. |

Audio chunks are assembled into one final WAV in the session. Clients may play
chunks as they arrive without presenting multiple permanent audio messages.

## Sessions

| Method and route | Purpose |
| --- | --- |
| `GET /v1/agent/sessions/<id>` | Read the session JSON; a missing session returns an empty session. |
| `GET /v1/agent/sessions/<id>/media/<message>/<media>` | Open authenticated session media. |
| `GET /v1/agent/sessions/<id>/export` | Export JSON and raw media as a ZIP, up to 512 MiB. |
| `POST /v1/agent/sessions/import` | Import a Mica session ZIP in multipart field `archive`. |
| `GET /v1/agent/tools` | Return the bounded VLM function schema. |

Session IDs and media paths are validated. Imports reject traversal paths,
symbolic links, duplicate paths, malformed manifests, and oversized archives.

## Errors

Mica errors use this envelope:

```json
{
  "error": {
    "code": "model_unavailable",
    "message": "human-readable detail"
  }
}
```

Common status codes are `400` for invalid input, `401` for a missing or invalid
token, `404` for absent session media, `413` for oversized uploads, `503` for
warmup/model admission failures, and `502` when a worker disappears while
proxying a request.
