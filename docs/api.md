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
| `GET /v1/models` | Yes | Models and variants enabled by the active profile, plus its ID and per-category defaults. |
| `GET /v1/endpoints` | Yes | Registered methods, paths and authentication requirements; also available offline with `mica-server endpoints`. |
| `GET /v1/catalog` | Yes | Complete curated registry, filterable by modality, capability, engine, or artifact family. |
| `GET /admin/models` | Yes | Active profile, memory budget, policies, and resident workers. |
| `POST /admin/profile/activate` | Yes | Switch to an installed workload profile while retaining compatible workers. |
| `POST /admin/server/stop` | Yes | Gracefully stop the server and workers; used by `mica-server stop`. A service supervisor may have its own restart policy. |

Endpoint discovery lists the route contract, not a claim that all modalities
are available. Inspect `/v1/models` for the active models' usable capabilities.
`serve --host 0.0.0.0 --port 8092` listens on all IPv4 interfaces with the same
authentication rules. Use a trusted LAN and firewall; HTTP is not TLS.

Activate an already-installed schema-5 workload without restarting the proxy:

```sh
curl "$MICA_BASE_URL/admin/profile/activate" \
  -H "Authorization: Bearer $MICA_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"profile":"gguf-low-memory"}'
```

The response names retained and unloaded workers and any startup models still
warming. Watch `GET /ready` before sending inference to the new task. Mica
rejects a task if its requirements exceed the existing global RAM/VRAM
allocation or it needs a backend that setup has not installed. Run `setup`
and restart to install a missing engine or enlarge the allocation. Requests
already running on incompatible workers drain before those workers unload;
the switch waits up to 30 seconds, then returns HTTP 409 without switching.

Registry examples:

```sh
curl "$MICA_BASE_URL/v1/catalog?modality=asr&engine=mlx-audio" \
  -H "Authorization: Bearer $MICA_API_KEY"

./build/mica-server registry list --modality video-text-to-text --engine mlx-vlm
./build/mica-server registry ping --modality asr
```

`registry ping` performs a remote availability check for each selected curated
Hugging Face repository. It does not download weights.

## Model selection

Inference requests may omit `model`. Mica then checks the active workload profile's
`selection.defaults` for the requested operation and input modalities—for
example, `chat.generate` with text-plus-image or `audio.transcribe` with audio.
Without a matching default, the highest-priority eligible model wins; ties
break by model ID. An explicit model must belong to the active profile and
accept the request's modalities. `GET /v1/models` currently exposes these
resolved defaults using legacy `text`, `image`, `video`, `asr`, and `tts` keys;
the public response shape has not yet been migrated to operation IDs.

`engine` selects a concrete runtime such as `mlx-lm`, `mlx-vlm`, `mlx-audio`,
`llama-cpp`, or `audio-cpp`. The legacy `backend` filter selects an artifact
family (`mlx`, `gguf`, or `vllm`). Each schema-5 workload selection includes its
description, modalities, license, repositories/revisions, and variant records
with format, exact quantization type, artifact/download bytes, component roles,
projector size,
size provenance, and memory reservation.

## Model IDs

An unsuffixed ID, such as `spark-x25-4b`, resolves to the exact execution
selected by the active workload profile. The explicit form is:

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

### Thinking controls

`GET /v1/models` reports `thinking_modes` and `thinking_budget_supported` for
each active model variant. Use the values reported there; Mica rejects a mode
not present in the model's chat template. The current pinned templates provide:

| Model | `reasoning_effort` values | Template default |
| --- | --- | --- |
| Ternary Bonsai 2 27B | `none`, `low`, `medium`, `xhigh` | `xhigh` |
| Spark-X2.5-4B | `none`, `on` | `on` |
| MiniCPM-V 4.6 Thinking | `none`, `on` | `on` |

Set `thinking_budget_tokens` to a nonnegative integer to cap thinking while
leaving the remaining `max_tokens` for the visible answer. Mica maps it to
Prism llama.cpp's `reasoning_budget_tokens` or MLX-VLM's `thinking_budget`.
The alias `reasoning_budget_tokens` is also accepted; do not send both. The
budget is available on the pinned Prism and MLX-VLM workers, not on arbitrary
vLLM or MLX-LM workers. `reasoning_effort: "none"` disables thinking and makes
a budget unnecessary. An effort level changes the template instructions; only
the token budget provides a hard thinking cap.

```json
{
  "model": "ternary-bonsai-2-27b",
  "messages": [{"role": "user", "content": "Explain the bug briefly."}],
  "reasoning_effort": "low",
  "thinking_budget_tokens": 64,
  "max_tokens": 384
}
```

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
| `llm_reasoning_effort`, `vlm_reasoning_effort` | Template-supported thinking mode for each agent model. |
| `llm_thinking_budget_tokens`, `vlm_thinking_budget_tokens` | Optional thinking cap, 0–768 tokens; the agent uses a 1,024-token output cap, leaving roughly 256 tokens after the maximum thinking budget. |

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

## Speaker diarization

`POST /v1/audio/diarizations` accepts multipart `file` and optional `model`.
For the Nemotron offline adapter, provide a 16 kHz mono PCM WAV of at most
60 seconds. This endpoint returns speaker turns, not an ASR transcript:

```json
{
  "model": "nemotron-3-diarization",
  "timestamp_unit": "seconds",
  "speaker_turns": [
    {"start": 0.12, "end": 2.99, "speaker_id": "speaker_0"}
  ]
}
```

`confidence` is optional and is retained only when the engine reports it;
Mica does not fabricate confidence for the MLX segment API. Speaker IDs are
anonymous within each recording. `stream: true` is rejected until an adapter
with a certified streaming contract is selected. Upstream native batch/live
diarization is not yet a public Mica endpoint. See [model details](models.md#nemotron-3-diarization).

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
