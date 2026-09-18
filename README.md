<p align="center">
  <img src="apps/mica-readme-header.svg" width="100%" alt="Mica Server — local models, one adaptive server">
</p>

# Mica Server

<p align="center">
  <strong>Run a useful team of local AI models without keeping every model in memory.</strong>
</p>

<p align="center">
  C++20 · Lua 5.4 · MLX · GGUF · vLLM · OpenAI-compatible API
</p>

Mica is a small local model server: an OpenAI-compatible proxy and load
balancer for multiple inference backends. Its purpose is to make a collection
of specialized AI models practical on consumer hardware.

This proof of concept demonstrates a multi-model workflow with an 8 GiB model
residency budget. Mica keeps the models relevant to the current task in memory
and swaps idle models out when another capability is needed. A profile defines
that working set. You might use a general chatbot profile for everyday work,
then switch to a coder profile that loads only the models and context policy
needed for software development.

The repository also includes a local chat interface for text, voice, images,
video, and documents. It exercises the same public API that other clients use.

## The three-part Mica project

Mica Server is the serving layer of a planned three-part local AI system:

1. **Mica Server — this repository.** Detects hardware, installs inference
   backends, downloads models, exposes one API, and manages model residency.
2. **Mica Harness — next milestone.** Will provide the reusable orchestration
   and validation layer for task workflows, profile selection, and end-to-end
   evaluation.
3. **Mica applications.** User-facing and task-specific clients built on the
   server and harness. The included chat is the first reference application.

The immediate roadmap item is completing the Mica Harness.

> **Status:** Mica is a local-first research prototype. MLX and GGUF have passed
> real inference tests on the hardware documented below. vLLM currently has a
> validation-only model; the four main catalog models remain disabled on vLLM
> until their architecture-specific integrations pass quality gates.

## What it provides

- One authenticated API URL for every enabled model and modality.
- OpenAI-compatible chat, completion, transcription, and speech routes.
- Streaming text, ASR, and TTS; streamed TTS is saved as one final WAV.
- A multimodal agent that uses MiniCPM-V as a bounded media-analysis tool.
- Q4 and Q8 model selection through explicit public model IDs.
- Deterministic warmup, lazy loading, idle eviction, and backend switching.
- Hardware discovery for Apple, NVIDIA, AMD, Intel, CPU, and TPU paths.
- Task profiles with context, concurrency, KV-cache, priority, and residency
  settings.
- A local chat UI with voice input, custom TTS voices, attachments, Markdown,
  session history, and conversation import/export.
- Reproducible conversion, validation, benchmarking, and Hugging Face
  publication tools.

## Supported catalog

| Model | Capability | MLX | GGUF | vLLM |
| --- | --- | --- | --- | --- |
| Spark-X2.5-4B | Text generation | Q4, Q8 | Q4_K_M, Q8_0 | Not certified |
| Granite Speech 5.0 470M TurboCTC | ASR | Q4, Q8 | Q4_K, Q8_0 | Not certified |
| Audio8 TTS Preview 0.6B | TTS and voice cloning | Q4, Q8 | Q4_0, Q8_0 | Future vLLM-Omni adapter |
| MiniCPM-V 4.6 Thinking | Image/video to text | Q4, Q8 | Q4_K_M, Q8_0 | Not certified in Mica |
| Qwen3 0.6B control | vLLM route validation | — | — | Q4 control only |

The four main models use permissive commercial-use licenses. Source revisions,
artifact provenance, protected quantization layers, context limits, and
validation results are recorded in
[docs/model-cards](docs/model-cards).

## Architecture

~~~text
OpenAI client / Mica chat UI
              │
              ▼
      mica-server :8080
      auth · routing · profiles
      warmup · memory admission · eviction
              │
              ▼
      profile-selected engine workers
       ├─ MLX artifacts: mlx-lm / mlx-vlm / mlx-audio
       ├─ GGUF artifacts: llama.cpp / audio.cpp
       └─ GPTQ artifacts: hardware-specific vLLM runtime
~~~

The control plane is a native C++ binary. The model catalog and scheduling
policy are Lua. Python is isolated behind MLX/vLLM workers and the optional
browser playground; GGUF inference uses native <code>llama.cpp</code> and
<code>audio.cpp</code> processes.

This split is intentional: C++ keeps the always-running proxy, hardware
detection, process lifecycle, and scheduler small; Lua is an embeddable
declarative configuration layer. Python exists only when a selected engine
requires it. A GGUF-only assistant never creates a Python environment.

Each schema-2 model entry pins one engine and artifact. The recommended
assistant profiles are engine-family-pure, while the proxy can dispatch a custom
profile containing multiple engines behind the same authenticated URL.

## Requirements

Every platform needs:

- CMake 3.24 or newer;
- a C++20 compiler;
- Lua 5.4 development headers and library;
- Git and cURL;
- enough disk space for the selected Q4/Q8 artifacts.

Python and [uv](https://docs.astral.sh/uv/) are required only when an MLX or
vLLM profile is selected, or when the optional Python playground is used.

Apple Silicon macOS is the recommended MLX development platform:

~~~sh
xcode-select --install
brew install cmake lua uv git libomp
~~~

For Debian or Ubuntu under Linux/WSL:

~~~sh
sudo apt update
sudo apt install build-essential cmake git curl python3 liblua5.4-dev
~~~

Install <code>uv</code> separately if the distribution package is unavailable.
Native Windows is not supported yet; use WSL2 for GGUF or vLLM.

| Engine family | Hosts | Notes |
| --- | --- | --- |
| MLX | Apple Silicon macOS | Uses unified memory; setup creates a Python 3.11 environment. |
| GGUF | macOS, Linux, WSL | Selects Metal, CUDA, HIP/ROCm, SYCL, Vulkan, or CPU. |
| vLLM | macOS, Linux, WSL | Selects Metal, CUDA, ROCm, XPU, TPU, or CPU and creates a Python 3.12 environment. |

## Quick start: Apple Silicon with MLX

Install the build/runtime prerequisites and clone the repository:

~~~sh
xcode-select --install
brew install cmake lua uv git curl libomp
git clone https://github.com/miguelamendez/mica-server.git
cd mica-server
~~~

### 1. Build

~~~sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
~~~

The two-job limit keeps native compilation within the project's 16 GiB
compilation budget.

You may run `./build/mica-server` directly. To install the native CLI and its
Lua/JSON configuration under your user account instead:

~~~sh
cmake --install build --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
mica-server --help
~~~

The installed binary discovers configuration relative to itself or through
`MICA_CONFIG_DIR`; it does not embed the source checkout path.

### 2. Inspect the machine and plan

~~~sh
./build/mica-server detect \
  --output "$HOME/models/mica-server/hardware-profile.json"

./build/mica-server plan \
  --profile mica-assistant-mlx \
  --ram-gib 8
~~~

<code>plan</code> makes no changes. Review the selected hardware target,
startup models, quantizations, and memory reservations before continuing.

### 3. Install the engines

~~~sh
./build/mica-server setup \
  --profile mica-assistant-mlx \
  --ram-gib 8
~~~

Setup creates only the environments required by the profile under
<code>~/models</code>, validates the selected engines, writes
<code>runtime.json</code>, and generates a local API key.
Artifacts are downloaded lazily from the curated Hugging Face repositories
during server warmup or the first request that needs them.

To use another runtime root, add the same <code>--root PATH</code> to setup,
serve, quantization, and helper commands. For example:
<code>--root "$HOME/.mica"</code>.

### 4. Start the server

~~~sh
./build/mica-server serve --port 8080
~~~

The first launch can take several minutes while Mica downloads artifacts,
starts required workers, and performs real smoke inference.

In another terminal:

~~~sh
curl http://127.0.0.1:8080/health
curl http://127.0.0.1:8080/ready
~~~

### 5. Start the chat

~~~sh
python3 apps/mica_playground.py \
  --mica-url http://127.0.0.1:8080 \
  --api-key-file "$HOME/models/mica-server/api-key" \
  --port 8090
~~~

Open <http://127.0.0.1:8090/chat>.

The local playground proxy keeps the API key out of browser JavaScript. Allow
microphone access when prompted. On macOS, <code>Command+Enter</code> sends and
<code>Option+R</code> starts or stops recording. On Linux/WSL, use
<code>Ctrl+Enter</code> and <code>Alt+R</code>.

## Engine recipes

### GGUF

~~~sh
./build/mica-server plan \
  --profile mica-assistant-gguf --ram-gib 8

./build/mica-server setup \
  --profile mica-assistant-gguf --ram-gib 8

./build/mica-server serve --port 8080
~~~

Setup compiles no more than two native jobs at once. The audio runtime includes
only the Granite and Audio8 families instead of its full model registry. The
saved hardware profile determines Metal, CUDA, HIP, SYCL, Vulkan, or CPU build
flags. This path does not install Python.

### vLLM

The current repository validates vLLM with the Qwen3 0.6B control profile. The
planned <code>mica-assistant-gptq</code> profile is listed in the remote catalog
but is deliberately non-installable until all four production modalities pass
native vLLM architecture, inference, batch, memory, and quality gates.

~~~sh
./build/mica-server plan \
  --profile vllm-control --ram-gib 8

./build/mica-server setup \
  --profile vllm-control --ram-gib 8 \
  --vllm-device auto

./build/mica-server serve --port 8080
~~~

Use <code>--vllm-device cpu|cuda|metal|rocm|xpu|tpu</code> to require a target
instead of falling back. Dedicated accelerators can receive
<code>--vram-gib N</code>. Apple Metal uses unified memory and remains governed
by <code>--ram-gib</code>.

### Switch profiles without deleting caches

~~~sh
./build/mica-server setup --profile mica-assistant-gguf --ram-gib 8
./build/mica-server serve
~~~

Stop the server before changing profiles. Setup reconciles the required engine
dependencies and records the selected profile; downloaded artifacts from older
profiles remain in the disk cache. A custom schema-2 profile may select more
than one engine, but every individual model remains bound to exactly one.

## Profiles and memory

Profiles define allowed models, context limits, concurrency, KV-cache policy,
priority, and the preferred warm working set. Built-ins live in
[config/profiles.lua](config/profiles.lua); installed JSON profiles live under
the runtime root and can come from the GitHub catalog or a local file.

| Profile | Engines/artifacts | Status and behavior |
| --- | --- | --- |
| <code>mica-assistant-mlx</code> | MLX only | Recommended Apple assistant: Spark pinned, ASR/TTS warm, vision on demand. |
| <code>mica-assistant-gguf</code> | GGUF only | Portable native equivalent; no Python dependency. |
| <code>mica-assistant-gptq</code> | GPTQ/vLLM | Visible roadmap profile; blocked from installation until all modalities are certified. |
| <code>quality-interactive</code>, <code>gguf-quality-interactive</code> | MLX or GGUF Q8 | Larger quality-oriented 16 GiB policies. |
| <code>text-batch</code>, <code>gguf-text-batch</code> | MLX or GGUF Q4 | Four bounded text sequences; utilities are ephemeral. |
| <code>low-memory</code>, <code>gguf-low-memory</code> | MLX or GGUF Q4 | At most one resident worker. |

At startup, required models are warmed first: LLM, then ASR. TTS and vision
follow while the budget permits. On demand, Mica evicts workers with no active
requests by expired TTL, oldest use, largest reservation, then model ID.

<code>--ram-gib</code> is a deterministic admission limit based on measured or
conservative reservations. It is not an operating-system cgroup or
<code>ulimit</code>; transient runtime allocations can exceed an estimate.
Inspect live decisions with:

~~~sh
curl http://127.0.0.1:8080/admin/models \
  -H "Authorization: Bearer $(< "$HOME/models/mica-server/api-key")"
~~~

The default idle TTL is five minutes. Adjust TTL, safety margin, timeouts, and
minimum free RAM in [config/policy.lua](config/policy.lua).

List, install, clone, validate, or edit profiles without modifying the binary:

~~~sh
./build/mica-server profile list
./build/mica-server profile list --remote
./build/mica-server profile install mica-assistant-mlx
./build/mica-server profile validate ./my-profile.json
./build/mica-server profile install-file ./my-profile.json
./build/mica-server profile create my-assistant --from mica-assistant-mlx
./build/mica-server profile edit my-assistant --editor vi
./build/mica-server setup --profile-file ./my-profile.json --ram-gib 8
~~~

A local profile may reference any compatible Hugging Face model, but must pin
an immutable revision, declare a commercial-use license, modality, concrete
engine, artifact path/format, and safe memory/context limits. Remote code is
never trusted. See [the profile guide](docs/profiles.md).

## Runtime layout

The default root is <code>~/models</code>:

~~~text
~/models/
├── cache/                       uv and Hugging Face caches
├── checkpoints/<backend>/      retained model artifacts
├── environment-tools/          download and publication tools
├── environment-mlx/            MLX workers, when installed
├── environment-vllm/           vLLM workers, when installed
├── runtime/                     native/runtime source and builds
├── staging/                     pinned full-precision sources
└── mica-server/
    ├── api-key                  generated local API key, mode 0600
    ├── hardware-profile.json
    ├── runtime.json
    ├── setup-acceptance.json
    ├── custom-models.json
    ├── profiles/                   installed schema-2 JSON profiles
    ├── logs/
    ├── workers/
    └── tmp/sessions/            chat JSON and session media
~~~

Disk cache and live residency are separate. Evicting a worker never deletes
the downloaded artifact.

## API usage

Load the generated key into the current shell:

~~~sh
export MICA_BASE_URL=http://127.0.0.1:8080
export MICA_API_KEY="$(< "$HOME/models/mica-server/api-key")"
~~~

### List models and quantizations

~~~sh
curl "$MICA_BASE_URL/v1/models" \
  -H "Authorization: Bearer $MICA_API_KEY"
~~~

When both Q4 and Q8 are configured, use an exact ID such as
<code>spark-x25-4b@mlx:q4</code>. An unsuffixed ID selects the configured
default.

### Chat completion

~~~sh
curl "$MICA_BASE_URL/v1/chat/completions" \
  -H "Authorization: Bearer $MICA_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "spark-x25-4b",
    "messages": [{"role": "user", "content": "Explain unified memory."}],
    "max_tokens": 256,
    "stream": false
  }'
~~~

Set <code>"stream": true</code> for server-sent text deltas.

### Speech recognition

~~~sh
curl "$MICA_BASE_URL/v1/audio/transcriptions" \
  -H "Authorization: Bearer $MICA_API_KEY" \
  -F model=granite-speech-5 \
  -F file=@sample.wav
~~~

### Text-to-speech

~~~sh
curl "$MICA_BASE_URL/v1/audio/speech" \
  -H "Authorization: Bearer $MICA_API_KEY" \
  -H "Content-Type: application/json" \
  -d '{
    "model": "audio8-tts-06b@mlx:q8",
    "input": "Mica is ready.",
    "response_format": "wav"
  }' \
  --output reply.wav
~~~

Audio8 also accepts a reference voice and its exact transcript. The chat UI is
the easiest place to record, transcribe, correct, preview, and reuse one.

### Agent chat

~~~sh
curl "$MICA_BASE_URL/v1/agent/chat" \
  -H "Authorization: Bearer $MICA_API_KEY" \
  -F session_id=my-session \
  -F text='Summarize the attachment.' \
  -F llm_model=spark-x25-4b \
  -F vlm_model=minicpm-v46-thinking \
  -F files=@document.pdf
~~~

Use <code>/v1/agent/chat/stream</code> for native ASR, tool, text, and audio
events. Voice-only input becomes the instruction; text plus voice treats the
transcript as supporting context. Images and video use the bounded VLM tool.
PDFs are rendered to page images before analysis.

### Routes

| Route | Purpose |
| --- | --- |
| <code>GET /health</code> | Process liveness |
| <code>GET /ready</code> | Warmup/readiness |
| <code>GET /v1/models</code> | Enabled models and quantized IDs |
| <code>GET /v1/catalog</code> | Registry filtered by modality, capability, or backend |
| <code>POST /v1/chat/completions</code> | Chat, including streaming |
| <code>POST /v1/completions</code> | Legacy completion adapter |
| <code>POST /v1/audio/transcriptions</code> | ASR |
| <code>POST /v1/audio/speech</code> | TTS |
| <code>POST /v1/agent/chat</code> | Complete multimodal agent turn |
| <code>POST /v1/agent/chat/stream</code> | Streaming agent turn |
| <code>GET /v1/agent/sessions/&lt;id&gt;</code> | Session JSON |
| <code>GET /v1/agent/sessions/&lt;id&gt;/media/...</code> | Authenticated media |
| <code>GET /admin/models</code> | Profile, budget, and resident workers |

The complete request/response and authentication reference is in
[docs/api.md](docs/api.md).

## Chat interface

The unified chat supports:

- multiple local sessions with history;
- text, recorded voice, images, video, PDFs, and text documents;
- compact media previews and playable voice bubbles;
- live ASR, transient agent state, Markdown, and TTS playback;
- one final saved audio item after streamed speech completes;
- built-in or recorded/uploaded TTS reference voices;
- Q4/Q8 selection for each modality;
- ZIP export/import with raw media.

The reference voice is stored in the browser's local IndexedDB. Audio8 requires
the clip and its exact transcript. See
[the agent chat state machine](docs/design/agent-chat-state-machine.md) for the
full event contract. Direct diagnostic pages remain available at
<code>/voice</code> and <code>/multimodal</code>.

## Add and quantize models

Mica accepts <code>tts</code>, <code>asr</code>,
<code>text-to-text</code>, and <code>img-text-to-text</code>:

~~~sh
./build/mica-server add-model \
  --url https://huggingface.co/owner/model \
  --modality text-to-text \
  --id my-model \
  --description "Short catalog description"
~~~

Registration rejects unknown and non-commercial licenses. The allowlist is
Apache-2.0, MIT, BSD-2-Clause, BSD-3-Clause, BSD, and ISC. Custom definitions
are stored in <code>~/models/mica-server/custom-models.json</code>.

Register and convert in one operation:

~~~sh
./build/mica-server add-model \
  --url https://huggingface.co/owner/model \
  --modality img-text-to-text \
  --backend mlx \
  --quant q4
~~~

For reproducible conversion, pin the immutable source revision:

~~~sh
./build/mica-server quantize \
  --model spark-x25-4b \
  --backend mlx \
  --quant q4 \
  --revision 0bcb35678590218655dff3765b9e61c83b35e9c4
~~~

- MLX dispatches to <code>mlx_lm.convert</code>,
  <code>mlx_vlm.convert</code>, or <code>mlx_audio.convert</code>.
- GGUF dispatches text/vision through <code>llama.cpp</code> and audio through
  <code>audiocpp_gguf</code>.
- vLLM creates gated compressed-tensors candidates: AutoRound W4A16 plus a GPTQ
  W4A16 control for Q4, and GPTQ W8A16 for Q8.

Full sources live under
<code>~/models/staging/&lt;model&gt;/&lt;revision&gt;/source</code> until
inference, publication, and clean re-download validation complete. Audio
profiles in
[config/mlx_audio_profiles.json](config/mlx_audio_profiles.json) record every
module retained at full precision and why. vLLM gates are documented in
[docs/vllm-quantization.md](docs/vllm-quantization.md).

## Configuration

| File | Responsibility |
| --- | --- |
| [config/models.lua](config/models.lua) | Model catalog, repositories, artifacts, reservations, and migration profiles |
| [config/policy.lua](config/policy.lua) | Idle TTL, safety margin, free-memory reserve, and timeouts |
| [config/tools.lua](config/tools.lua) | VLM model and upload/media bounds |
| [config/mlx_audio_profiles.json](config/mlx_audio_profiles.json) | Protected audio quantization layers |
| [config/vllm_quantization_profiles.json](config/vllm_quantization_profiles.json) | vLLM conversion and calibration |
| [config/huggingface_publish.json](config/huggingface_publish.json) | Publication destinations and gates |
| [config/profiles.lua](config/profiles.lua) | Active schema-2 execution and residency profiles |
| [profiles/catalog.json](profiles/catalog.json) | GitHub-distributed recommended profile catalog |

Architecture decisions and the current implementation boundary are maintained
in
[docs/design/architecture-decisions.md](docs/design/architecture-decisions.md).

## Publish validated models to Hugging Face

Publication creates one repository per logical model with MLX Q4/Q8, GGUF
artifacts, a model card, hashes, provenance, and validation evidence. vLLM
candidates remain excluded until they pass native-hardware and quality gates.

Authenticate interactively so the write token never enters a command, README,
or Git history:

~~~sh
"$HOME/models/environment-tools/bin/hf" auth login
~~~

Review, publish, create modality collections, and verify:

~~~sh
python3 scripts/publish_huggingface.py --dry-run
python3 scripts/publish_huggingface.py

"$HOME/models/environment-tools/bin/python" \
  scripts/publish_huggingface_collections.py

"$HOME/models/environment-tools/bin/python" \
  scripts/verify_huggingface_publish.py
~~~

For a temporary token, log out immediately afterward:

~~~sh
"$HOME/models/environment-tools/bin/hf" auth logout
~~~

The publisher refuses artifacts not marked validated in
[config/huggingface_publish.json](config/huggingface_publish.json), uploads one
artifact at a time, and never prints the token.

Current publication targets:

- <code>miguelamendez/mica-spark-x25-4b</code>
- <code>miguelamendez/mica-granite-speech-5</code>
- <code>miguelamendez/mica-audio8-tts-06b</code>
- <code>miguelamendez/mica-minicpm-v46-thinking</code>

## Validation and development

~~~sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure
~~~

Run a public-route acceptance test after starting a backend:

~~~sh
python3 scripts/mica_endpoint_acceptance.py \
  --api-key-file "$HOME/models/mica-server/api-key" \
  --text-model spark-x25-4b \
  --output /tmp/mica-endpoint-acceptance.json
~~~

The benchmark matrix covers reasoning, coding, knowledge, creativity, and
summarization across 512 to 16K input tokens, media, and online concurrency.

### Apple M4, 24 GiB snapshot

These are real measurements from the retained validation artifacts. Text values
are decode throughput. ASR/TTS use real-time factor (RTF); below 1 is faster
than real time.

| Model | Backend/quant | Workload | Speed | Worker memory |
| --- | --- | --- | ---: | ---: |
| Spark-X2.5-4B | MLX Q4 | Text | 36.8 tok/s | 2.40 GiB |
| Spark-X2.5-4B | MLX Q8 | Text | 21.4 tok/s | 4.45 GiB |
| Spark-X2.5-4B | GGUF Q4_K_M/Metal | 4K text | 31.7–32.3 tok/s | 3.09 GiB RSS |
| Spark-X2.5-4B | GGUF Q8_0/Metal | Text | 21.2–21.4 tok/s | 4.92 GiB RSS |
| Granite Speech 5 470M | MLX Q4/Q8 | 3.505 s ASR | 0.0074 RTF | 0.49/— GB peak |
| Granite Speech 5 470M | GGUF Q4_K/Metal | 3.505 s ASR | 0.0138–0.0207 RTF | 0.83 GiB RSS |
| Granite Speech 5 470M | GGUF Q8_0/Metal | 3.505 s ASR | 0.0136–0.0144 RTF | 1.27 GiB RSS |
| Audio8 TTS 0.6B | GGUF Q4_0/Metal | Plain / cloned | 1.083 / 1.217 RTF | 1.51 GiB RSS |
| Audio8 TTS 0.6B | GGUF Q8_0/Metal | Plain / cloned | 1.165 / 1.298 RTF | 1.81 GiB RSS |
| MiniCPM-V 4.6 Thinking | MLX Q4 | Image / video | 78.2 / 84.7 tok/s | 2.81 / 3.03 GB peak |
| MiniCPM-V 4.6 Thinking | MLX Q8 | Image / video | 70.2 / 77.7 tok/s | 2.97 / 3.18 GB peak |
| MiniCPM-V 4.6 Thinking | GGUF Q4_K_M/Metal | Image / video | 118.2 / 101.7 tok/s | 2.25 GiB RSS |
| MiniCPM-V 4.6 Thinking | GGUF Q8_0/Metal | Image / video | 90.3 / 84.8 tok/s | 2.28 GiB RSS |

Detailed evidence is in [docs/validation](docs/validation) and
<code>artifacts/benchmarks/</code>.

## Troubleshooting

### Readiness does not complete

Inspect <code>~/models/mica-server/logs/</code> and
<code>/admin/models</code>. First launch may still be downloading or
smoke-testing. A failed smoke test deliberately keeps the server unready.

### 401 Unauthorized

Use the key created by the same <code>--root</code> used for setup and serve.
The default is <code>~/models/mica-server/api-key</code>.

### Model or quantization unavailable

Check <code>/v1/models</code>. Only variants selected during setup and allowed
by the active profile/backend can be requested.

### Memory-budget error

Choose Q4, select a smaller profile, or increase <code>--ram-gib</code> and run
setup again. Mica never evicts an in-flight worker.

### Hugging Face download failure

Confirm the curated repository exists and is accessible. Authenticate with the
isolated <code>hf</code> executable for private repositories.

### Microphone shortcut does not start

Allow microphone access for <code>127.0.0.1</code>. On macOS recording is
<code>Option+R</code>, not <code>Command+R</code>; Command+R is browser refresh.

## Security

- Mica binds to loopback by default. Do not expose it publicly without TLS,
  network controls, and secret management.
- The generated API key is mode 0600 and is never printed by setup.
- The playground holds that key in its local Python proxy.
- Session-media routes use opaque indices and reject paths outside the session.
- Attachment contents are treated as untrusted data by the agent prompt.
- Never commit tokens, runtime state, model caches, or private conversations.

## Roadmap

- [ ] Complete Mica Harness, the workflow orchestration and validation layer.
- [ ] Certify and enable the full `mica-assistant-gptq` vLLM profile on native
  CUDA/ROCm/XPU hardware.
- [ ] Add image-generation engines, Q4/Q8 artifacts, routes, and profiles.
- [ ] Add audio/music-generation engines and quants for Stable Audio and
  DiffRhythm, with explicit license policy.
- [ ] Add a Qwen2.5-Coder-7B coding profile.
- [ ] Migrate the default runtime layout from `~/models` to `~/.mica`.

Stable Audio and DiffRhythm are documented future additions, not currently
installed or served. See [the execution and residency design](docs/design/model-execution-and-residency-profiles.md),
[the architecture decisions](docs/design/architecture-decisions.md),
[the API reference](docs/api.md), and [the profile guide](docs/profiles.md).
