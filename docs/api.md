# Mica API reference

Mica exposes one OpenAI-compatible base URL for workload-selected models.
Unless otherwise noted, routes require a bearer token. Requests can bind to
different workloads on the same server; see [global scheduling](scheduling.md).

```sh
export MICA_BASE_URL=http://127.0.0.1:55000
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

## Dashboard and administration

Enable the bundled, buildless dashboard with `start --dashboard` or
`"dashboard": true` in `~/.mica/config/server.json`. It shares the Mica listener
at `/dashboard`; localhost remains the default. Add `--host 0.0.0.0` only when
intentionally allowing trusted-network access. A third device needs only a
browser; operations execute on the dashboard's host.

The dashboard is disabled by default. `start --dashboard` is a one-launch
override; `config set --dashboard --restart` saves it for later CLI/TUI starts.
`config set --no-dashboard --restart` disables it persistently. Configuration
changes while stopped do not launch Mica. A second `start --dashboard` cannot
modify an already-running listener; use the configuration command instead.
`mica-server dashboard` reports local/network URLs; `--open` opens this host's
browser. `status` includes `api_url`, `dashboard_url` (null when disabled),
`network_dashboard_urls`, and the running build revision. The API port defaults
to 55000. `/dashboard` always uses that
same API port, not a separate web server.

Mica Client uses its own device-local listener on 55001. Web apps share
`/apps/APP_ID/` on their hosting server/client; private app services use
55200–55399 and inference workers 55900–55999. These are reusable bounded pools,
not prestarted workers. See [ports](ports.md) and [client setup](client.md).

Enabling it creates a private admin key at `~/.mica/secrets/admin-api-key`
(override with `admin_api_key_file` or `serve --admin-api-key-file PATH`). The
existing `api_key`/`api_key_file` remains the inference credential.
**Inference keys cannot call `/admin/*`** once separate administration is
enabled. Admin keys also allow inference for endpoint testing. Non-dashboard
installations retain legacy single-key behavior until an admin file is
configured/created; separation persists afterward, even with the dashboard
disabled. The keys must be distinct.

```sh
mica-server start --dashboard --host 0.0.0.0 --port 55000
mica-server config set --rotate-admin-api-key
mica-server config set --rotate-api-key
```

Copy the admin key privately on its host for the initial browser login. The
browser remembers it in this tab's session storage across refreshes, not in
permanent local storage. Disconnect, invalid authentication and external key
rotation clear/revoke access. The TUI can reveal/copy a newly rotated key;
normal inventory, status and job logs never include keys.

`POST /admin/keys/rotate` accepts `{"kind":"admin"}` or
`{"kind":"inference"}`. It saves the replacement atomically with private
permissions and returns it once in an authenticated, `Cache-Control: no-store`
response. Old keys stop authorizing new requests immediately; accepted streams
continue. The dashboard replaces its in-memory admin key; other browser sessions
must reconnect. Admin rotation leaves inference keys unchanged. Lost responses
can be recovered through local CLI/SSH and the private key file. HTTP is not
safe on an untrusted network; use an encrypted overlay or separately managed
HTTPS reverse proxy.

| Management route | Behavior |
| --- | --- |
| `GET /admin/dashboard/inventory` | Native TUI inventory, machine policy and dashboard route discovery. |
| `GET /admin/dashboard/workloads/{id}` | Workload YAML; no arbitrary filesystem path. |
| `POST /admin/dashboard/jobs` | Submit a typed action; return job ID after any required draining. |
| `GET /admin/dashboard/jobs/{id}` | State, error/exit code and last 32 KiB of worker output. |
| `POST /admin/inference/pause` | Drain workers (up to 30 seconds) and unload; keep HTTP/dashboard available. |
| `POST /admin/inference/resume` | Resume lazy inference; blocked during maintenance. |

Actions: `workload.prepare`, `workload.save`, `workload.clone`, `app.install`,
`app.start`, `app.stop`, `app.uninstall`, `app.source-add`, `app.source-remove`,
`settings.save`, `update.check`, `update.install`. No shell commands,
caller-selected executables or arbitrary output paths are accepted. Job
records/logs live privately in `~/.mica/state/dashboard-jobs/`; workers are
native Mica processes. Downloads/builds retain hardware selection and memory
safeguards. Workload preparation drains/unloads models for safe engine/artifact
installation, restores the prior default and resumes serving on completion;
models reload lazily. App installation/opening does not restart Mica.
Installing app files and preparing its workload remain separate actions.
Browser apps remain loopback-only internally. **Open** starts a closed app,
waits for its job, then opens `/apps/APP_ID/PATH` on Mica's own listener.
An authenticated `POST /admin/dashboard/apps/APP_ID/open` creates an eight-hour,
HttpOnly/SameSite app cookie. Admin keys/cookies are never forwarded to the app.
Cross-origin requests are rejected; admin rotation and dashboard Disconnect
revoke sessions. Streaming and multipart uploads use a bounded native bridge.
Install only trusted code: same-origin apps are not sandboxed and can access
origin-scoped browser storage.

`POST /admin/dashboard/apps/APP_ID/download` accepts `mode` (`remote` or `local`),
`server_url` (credential-free origin) and optional `workload`. It returns a
verified `.tar.gz` client package, with no keys, without installing on the host.
Extraction and explicit installer execution are required on the receiving
device. Local inference is optional/separately confirmed. Native Dictation has
no browser UI; microphone, shortcuts and setup TUI run on its installed host.

Initial workload setup is required; this is not a zero-model bootstrap service.
Stopping Mica also stops its dashboard; restart through CLI/SSH or an external
supervisor. Listener, key import and allocation edits use restart handling;
rotation, default workload and strategy changes apply live. Updates/listener
changes may disconnect the browser; keep SSH access for recovery.

## Health and discovery

| Method and route | Authentication | Purpose |
| --- | --- | --- |
| `GET /health` | No | Process liveness. |
| `GET /ready` | No | Warmup/readiness; returns 503 while required startup work fails. |
| `GET /v1/models` | Yes | Models and variants enabled by the active profile, plus its ID and per-category defaults. |
| `GET /v1/workloads` | Yes | Workload IDs/descriptions, selected models/quants, active workload, memory requirements and known activation blockers. |
| `GET /v1/endpoints` | Yes | Registered methods, paths and authentication requirements; also available offline with `mica-server endpoints`. |
| `GET /v1/catalog` | Yes | Complete curated registry, filterable by modality, capability, engine, or artifact family. |
| `GET /admin/models` | Yes | Active profile, memory budget, policies, and resident workers. |
| `POST /admin/profile/activate` | Yes | Switch to an installed workload profile while retaining compatible workers. |
| `POST /admin/scheduler` | Yes | Apply `high`, `single` or `balanced` residency globally without restarting. |
| `POST /admin/server/stop` | Yes | Gracefully stop the server and workers; used by `mica-server stop`. A service supervisor may have its own restart policy. |

Endpoint discovery lists the route contract, not a claim that all modalities
are available. Inspect `/v1/models` for the active models' usable capabilities.
Model entries also expose `supported_tasks` (intended uses such as coding or
OCR), separately from `abilities` and `supported_interactions`. Tasks do not
enable routes: coding and OCR may share chat completions, while speech
transcription needs the appropriate model interaction and engine adapter.
The offline registry supports `mica-server registry list --task ocr`; this does
not add a `task` query parameter to the HTTP catalog.
`supports_tools` reports effective model/engine tool support. `tool_call_formats`
and `tool_call_parsers` come from the model YAML. Each endpoint contract also
reports `engine_supports_tools` separately; this alone does not enable tools
for every model on that engine.
`serve --host 0.0.0.0 --port 55000` listens on all IPv4 interfaces with the same
authentication rules. Use a trusted LAN and firewall; HTTP is not TLS.

Activate an already-installed schema-5 workload without restarting the proxy:

```sh
curl "$MICA_BASE_URL/admin/profile/activate" \
  -H "Authorization: Bearer $MICA_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{"profile":"gguf-low-memory"}'
```

This selects the default for requests without a workload selector. Resident
workers and in-flight streams remain intact; the global scheduler reconciles
workers on subsequent requests. Requirements must fit global allocations and
the selected engine must be installed.

Use `X-Mica-Workload: ID`, or a `workload` JSON/multipart field, to select a
workload per inference request. Conflicting selectors are rejected. Discovery
supports `GET /v1/models?workload=ID`. Compatible workers can be shared across
workloads; incompatible configurations wait for busy work to drain before
replacement. The assistant Settings picker changes its app-specific binding,
not the server default. Models load on demand and must fit estimated memory
reservations; uncached weights can download on acquisition.

`POST /admin/scheduler` accepts `{"strategy":"single"}` (also `high` and
`balanced`) as a temporary live override. Use `mica-server config set --strategy
balanced` to apply and persist it. Strategy/default changes do not restart Mica;
allocation/listener/credential changes still follow the settings restart flow.

Agent chat replies, their final streaming `done` event, and saved assistant
messages include `generation_metrics`. When available, these contain
engine-reported `decode_tokens_per_second`, completion tokens, and decode
seconds. This covers final-answer generation (including reasoning tokens),
not prefill, prior tool calls, loading, or TTS. Engines without decode timing
return `available: false`; Mica does not invent a rate from text length or
whole-request latency. Enable **Show generation speed** in Chat Settings to
display the metric beside replies; the browser saves this preference.

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

## Embeddings

`POST /v1/embeddings` selects an `embedding.generate` model in the active
workload. It uses the same authentication, lazy loading, memory admission and
sequential swapping as chat; an embedding model is not eligible for chat.

```json
{
  "model": "embeddinggemma-2",
  "input": [
    "task: search result | query: Why is the sky blue?",
    "title: none | text: Air molecules scatter blue sunlight strongly."
  ],
  "encoding_format": "float"
}
```

Each input returns an indexed, normalized 768-dimensional vector under
`data[].embedding`. Text batches are supported in one request. To embed media,
use an input object with a `content` array, for example:

```json
{
  "model": "embeddinggemma-2",
  "input": [{"content": [
    {"type": "image_url", "image_url": {"url": "data:image/png;base64,..."}}
  ]}],
  "encoding_format": "float"
}
```

`input_audio` and `input_video` use the same content parts as the native chat
engine. EmbeddingGemma-2 shares an 8192-token input budget across modalities;
the GGUF's larger architectural position limit is **not** the publisher's
supported embedding input budget. Its workload uses 8192 input tokens and zero
output-token reservation. Only embedding models may use a zero output budget.
Embeddings do not decode an output token or maintain an autoregressive KV cache.

Streaming and generation token parameters are rejected. The current route
returns native dimensions: `dimensions` is rejected rather than silently
ignored. For Matryoshka retrieval, truncate vectors client-side to 512, 256 or
128 dimensions, then L2-normalize again; use matching dimensions for queries
and documents. Model task prefixes are explicit client input, never silently
injected by Mica. See [researcher validation](researcher-validation.md).

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
