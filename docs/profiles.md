# Mica workload profiles

A Mica workload profile is a model collection with loading and residency
policies, not an ordered execution workflow. It selects the models needed for
an intended use and tells Mica how each model is installed, loaded, retained,
and evicted. The CLI keeps the concise `profile` command and `--profile` flag;
the packaged definitions live in `config/workloads/`.

Every schema-5 workload profile needs a nonempty `description` explaining its
intended use. Its `models` array is a collection, not a set of fixed roles or
modality groups. Optional `selection.defaults` map an operation and input
modalities to a model ID. A request can name another eligible model in the
active profile. With no explicit model or matching default, Mica chooses the
highest-priority eligible model, breaking ties by model ID.

An inference profile is one layer of Mica's resolver, not an installation
script. It composes:

- a generated system profile containing detected machine facts;
- engine manifests containing typed installation and launch recipes;
- model records containing immutable, possibly multi-file artifacts; and
- runtime policy containing context, batching, placement, memory, priority,
  warmup, and eviction.

The complete dependency graph, packaged YAML engine manifests, and migration
status are documented in [System, engine, model, and inference
profiles](engines-and-profiles.md).

The coding profile is an active built-in YAML file at
[`config/workloads/mica-coder-bonsai-macos.yaml`](../config/workloads/mica-coder-bonsai-macos.yaml).
All built-in workload profiles are self-contained schema-5 YAML in
[`config/workloads/`](../config/workloads/). Shareable and user-created profiles use
the same format. Installed files are kept in
`<root>/config/profiles/`; the default catalog is
[`profiles/catalog.yaml`](../profiles/catalog.yaml) in this GitHub repository.
The authoritative machine-readable contract is
[`schemas/workload-v5.schema.json`](../schemas/workload-v5.schema.json). JSON
remains the runtime/API state format, but it is not accepted for profiles.
Older `mica.profile` entries in [`config/models.lua`](../config/models.lua)
remain only for legacy acceptance/test reproduction; they are not the
authoritative workload definitions.

An already-installed YAML profile under `<root>/config/profiles/` takes
precedence over a packaged workload with the same ID. `profile list` marks an
installed schema-3 profile with `migration_recommended: true`; Mica does not
silently replace user-edited files. `profile export-all --output DIRECTORY`
can provide a starting point, but exports of older Lua profiles remain
schema-4 until reviewed and given a schema-5 description and selection policy.
Validate and install the desired files individually.

## Apply a profile

Inspect the plan before changing the runtime:

```sh
./build/mica-server plan --profile interactive --ram-gib 8
```

Then reconcile the machine to it:

```sh
./build/mica-server setup --profile interactive --ram-gib 8
./build/mica-server serve
```

`setup` detects the hardware, installs or compiles every missing engine named
by the profile, and records each concrete model/artifact/engine selection.
Model artifact bundles are downloaded on the first server start. Startup models
are then warmed in priority order; on-demand models remain stopped until
requested.

The default profile is `auto`: it resolves to `mica-assistant-mlx` on Apple
Silicon and `mica-assistant-gguf` elsewhere. Supplying `--backend` or `--quant`
cannot override a schema-5 profile. Change the profile itself so its behavior
remains reproducible.

Setup loads detected hardware or a saved YAML/JSON profile supplied through
`--hardware-profile`. It writes detector-owned facts to `state/hardware.yaml`
and temporarily keeps the older `state/system-profile.yaml` and JSON copy.
Separately, `config/machine.yaml` is user-owned policy: `plan` reads it,
`setup` creates a minimal version only when absent, and neither command
overwrites an existing policy. Use `--machine-file PATH` to plan or set up
against another YAML policy. The global machine allocation is the memory
ceiling; a workload profile declares the RAM/VRAM it requires. Setup rejects the
workload when the allocation is smaller. Schema-4 and schema-5 workloads reserve discrete memory
independently for each detected GPU. This is reservation accounting, not an
OS-enforced limit, and concurrent multi-GPU inference still needs hardware
validation. Native
compilation is limited to two jobs, but the 16 GiB build-memory budget is not
an OS-enforced process cap.

## Recommended assistant profiles

| Profile | Artifact/engine boundary | Status |
| --- | --- | --- |
| `mica-assistant-mlx` | MLX only: `mlx-lm`, `mlx-audio`, `mlx-vlm` | Runnable on Apple Silicon. |
| `mica-assistant-gguf` | GGUF only: `llama-cpp`, `audio-cpp` | Runnable on macOS, Linux, and WSL; no Python. |
| `mica-assistant-gptq` | GPTQ through vLLM | Listed but blocked until all four modalities pass native vLLM certification. |
| `bonsai-pq2-vision-cpu` | PQ2_0 through isolated `prism-llama-cpp` on CPU | Experimental; inference certification pending. |
| `bonsai-pq2-vision-gpu` | PQ2_0 through isolated `prism-llama-cpp` on accelerator 0 | Experimental; inference certification pending. |
| `mica-coder-bonsai-macos` | Bonsai PQ2_0 on Prism Metal, Ling Q4 on llama.cpp Metal, Spark Q4 on MLX, MiniCPM Q4 on MLX | The previous three-model profile passed text, image, video, streaming, and one-worker swap on Apple M4; Ling is a new candidate pending direct and proxied inference. [Validation](validation/bonsai-macos-metal.md) and [task benchmarks](validation/coder-profile-benchmark-macos.md). |
| `mica-coder-qwen-gguf` | Stock llama.cpp only: Spark Q4, Qwen3.8 27B GSQ-RCO IQ3_XXS, Qwen3.5 9B Q4 + BF16 projector | One resident worker; 64K input / 16K output. Linux CUDA certification is tracked in [the installation report](validation/linux-coder-install.md). |

The first two contain the same logical assistant set: Spark text, Granite ASR,
Audio8 TTS, and MiniCPM vision/video. The GPTQ profile remains unavailable while
that multimodal set is uncertified; internal validation fixtures are never
advertised as supported models.

For the 24-GiB Apple Silicon coding profile, inspect and set up with the
explicit 16-GiB reservation budget (single resident worker):

```sh
./build/mica-server profile show mica-coder-bonsai-macos
./build/mica-server plan --profile mica-coder-bonsai-macos --ram-gib 16
./build/mica-server setup --profile mica-coder-bonsai-macos --ram-gib 16
./build/mica-server serve
```

Spark is the default chat model and warms first; Bonsai PQ2_0 and Ling Q4 are
on-demand coding alternatives, while MiniCPM serves document/image analysis.
Each of the four models allows 65,536 input tokens and up to 16,384 generated
tokens, with an 81,920-token total context and one concurrent request. The
input allowance includes chat history, templates, tool messages, and visual
tokens, not just the newest user message. Reasoning may consume part of the
generation allowance; this does not guarantee 16,384 visible answer tokens.
These are workload ceilings, not a requirement to generate that many tokens
on every request or a claim of measured long-context quality. Smaller output
budgets can be requested per call.

The current conservative KV estimate gives Bonsai a 14.8125-GiB model/cache
reservation at this context size, plus the 0.5-GiB workload safety margin. The
workload therefore requires a 16-GiB machine allocation; a 14-GiB allocation
must reject it. `--ram-gib 16` cannot override a lower existing machine-policy
ceiling. Review `~/.mica/config/machine.yaml` explicitly before setup; Mica
does not raise that user-owned limit automatically. Context declarations in
the upstream configs are not evidence of trained context or usable quality
at the maximum. Full-size inference and memory validation remain pending.
Ling Q8 is registered in the model manifest but is not selected by this profile;
choose it in a copied YAML profile for a quality comparison. Ling's MLX Q4
community conversion requires a separate `rapid-mlx` implementation, so it is
not registered against Mica's current `mlx-lm` engine. The profile allows one
resident worker, so a request for another model evicts the warm worker rather
than trying to keep all four loaded. This is reservation-based admission, not
an operating-system-enforced 16-GiB process limit. Setup and the Bonsai Metal
inference path still need real-machine validation before treating the profile
as certified.

### Experimental context-ceiling override

`plan`, `setup`, and `serve` accept `--ignore-context-limit` to bypass a
model manifest's declared total context ceiling and enable partial-workload
startup. Setup records the choices in
`runtime.json`, and serving that setup retains it; run setup again without
the flag to remove the persisted override. A warning is printed whenever
the override is enabled. This does not change model weights, RoPE settings,
or an engine's capabilities and does not certify quality beyond the declared
context. Engine startup may still reject an unsupported configuration.

Workload input/output/total ceilings, a known supported output ceiling, and
per-request RAM/VRAM admission remain enforced. With this override the coding
server can start under 14 GiB and warm Spark, but Bonsai's 81,920-token
reservation is still too large and its requests return an admission error.
The API being ready does not certify that every listed model fits.
`--allow-partial-workload` selects this startup behavior without overriding
declared model context ceilings. Pinned workers must still fit; machine
device restrictions and physical-memory validation are never bypassed.

```sh
mica-server setup --profile mica-coder-bonsai-macos --ram-gib 14 --ignore-context-limit
mica-server serve
```

`mica-coder-gguf` contains the same four coding models and context ceilings,
but uses `llama-cpp` for Spark/Ling/MiniCPM and `prism-llama-cpp` for Bonsai.
Placements follow the hardware profile (CUDA on compatible NVIDIA Linux,
Metal on Apple Silicon, or CPU). Its default and startup model is Spark.

### Model metadata audit

The read-only developer tool `scripts/audit_model_limits.py` checks all model
manifests against upstream architecture and generation configurations without
loading weights. Run it with `uv run --with pyyaml`; it is not a dependency
of the native server. The dated result is in
`artifacts/model-context-audit-2026-10-05.json`.
Missing supported/trained output claims remain unknown rather than treating
`max_new_tokens` or a sampling default as a trained maximum. ASR, TTS, and
diarization are not assigned a chat-style 64K context from unrelated encoder
or decoder configuration fields. GGUF-only releases without `config.json`
require model-card or artifact-metadata verification instead.

## Catalog and local files

```sh
# Inspect built-in and installed profiles.
./build/mica-server profile list
./build/mica-server profile show mica-assistant-mlx

# Inspect/install the GitHub-hosted catalog.
./build/mica-server profile list --remote
./build/mica-server profile install mica-assistant-mlx

# Create, validate, install, and edit local profiles.
./build/mica-server profile create my-assistant --from mica-assistant-mlx
./build/mica-server profile validate ./my-assistant.yaml
./build/mica-server profile install-file ./my-assistant.yaml
./build/mica-server profile edit my-assistant --editor vi

# A file can also be selected directly. Setup installs it under the runtime root.
./build/mica-server setup --profile-file ./my-assistant.yaml --ram-gib 8
```

`profile edit` uses a temporary draft and replaces the installed profile only
after validation succeeds. The editor must be one executable path; interactive
terminal editors such as `vi` and `nano` are the reliable choices.
Run `setup --profile <id>` after any edit. The server compares the active
schema-5 definition with the setup snapshot and refuses to start if engines,
artifacts, context, batching, residency, or memory policy changed.

## Built-in profiles

| Profile | Backend | Declared RAM requirement | Behavior |
| --- | --- | ---: | --- |
| `interactive` | MLX | 8 GiB | Pins text, warms ASR/TTS, loads vision on demand. |
| `quality-interactive` | MLX | 16 GiB | Uses Q8 for text, TTS, and vision where configured. |
| `balanced-all` | MLX | 12 GiB | Attempts to warm all four capabilities. |
| `text-batch` | MLX | 8 GiB | Four text sequences; utility models are ephemeral. |
| `long-context` | MLX | 12 GiB | Single text request with the certified local context ceiling. |
| `realtime-voice` | MLX | 12 GiB | Pins ASR, text, and TTS, with room for on-demand vision. |
| `diarization` | Auto | 2 GiB | Nemotron Q8 speaker turns; selects MLX or native audio.cpp. |
| `vision-quality` | MLX | 8 GiB | Pins Q8 vision and loads other utilities on demand. |
| `low-memory` | MLX | 4 GiB | At most one resident worker; models are loaded per request. |
| `gguf-interactive` | GGUF | 8 GiB | Native equivalent of the interactive workflow. |
| `gguf-quality-interactive` | GGUF | 16 GiB | Native Q8-oriented workflow. |
| `gguf-text-batch` | GGUF | 8 GiB | Native batched text workflow. |
| `gguf-long-context` | GGUF | 12 GiB | Native single-request context workflow. |
| `gguf-low-memory` | GGUF | 6 GiB | At most one native worker; no Python environment. |

The machine/command-line RAM and VRAM values are the admission ceilings.
Workload requirements are eligibility checks, not smaller usage caps. Omitting
`--vram-gib` (or passing `auto`) derives the discrete-device capacity; an
explicit `--vram-gib 0` disables discrete VRAM. Apple Metal and other detected
unified-memory devices charge complete model reservations to the RAM pool.

## Profile structure

Schema 5 puts the workload's model selection and runtime policy in one YAML file.
There is no separate execution-template lookup:

```yaml
schema: 5
id: my-assistant
description: Compact local text assistant
mode: interactive
selection:
  engine_policy: explicit-only
  defaults:
    - {operation: chat.generate, required_inputs: [text], model: spark-x25-4b}
memory:
  required_ram_gib: 8
  required_vram_gib: 0
  safety_reserve_gib: 0.5
  maximum_resident_workers: 1
models:
  - id: spark-x25-4b
    engine: llama-cpp
    artifact: {id: q4, format: gguf}
    context: {max_input_tokens: 7168, max_output_tokens: 1024, max_total_tokens: 8192}
    batching: {max_concurrent_requests: 1}
    kv_cache: {precision: q8}
    placement: {mode: fixed, device: cpu, gpu_layers: 0}
    residency: pinned
    priority: 100
    startup: true
    idle_seconds: 0
```

The model must exist in [`config/model-manifests/`](../config/model-manifests/)
or be defined as an external model in the workload. Its selected artifact must
list the chosen engine and have a compatible format. The parser rejects unknown
fields, duplicate model IDs, impossible context/output limits, and unsupported
artifact–engine pairs.

Known models may omit `engine` and `artifact`. The default selection policy,
`prefer-installed`, chooses an installed compatible engine before falling back
to model-manifest order. `manifest-order` always follows manifest order;
`explicit-only` requires both fields. An omitted artifact prefers Q4, then Q8,
then the first remaining artifact ID in sorted order. External models still require a
complete engine, source, and artifact declaration. Explicit settings always win.

## External Hugging Face models

A YAML profile may introduce a model that is not in the built-in registry. It
must provide an exact compatibility contract; a Hugging Face URL or model card
alone is not treated as proof that an engine can load the architecture.

```yaml
schema: 5
id: my-external-assistant
description: Private GGUF assistant using one externally sourced text model.
mode: interactive
memory:
  required_ram_gib: 4
  required_vram_gib: 0
  safety_reserve_gib: 0.5
models:
  - id: my-text-model
    modality: text-to-text
    engine: llama-cpp
    declared_context_tokens: 2304
    source:
      repository: owner/original-model
      revision: 0123456789abcdef0123456789abcdef01234567
      license: apache-2.0
      trust_remote_code: false
    artifact:
      repository: owner/quantized-model
      revision: 89abcdef0123456789abcdef0123456789abcdef
      format: gguf
      quantization: q4
      path: model-q4-k-m.gguf
      size_gib: 2.0
      reservation_gib: 3.3
    context:
      max_input_tokens: 1792
      max_output_tokens: 256
      max_total_tokens: 2048
    placement:
      mode: auto
      device: auto
    residency: on-demand
    idle_seconds: 60
```

Both source and artifact revisions must be immutable commit hashes. The
declared license must be on Mica's commercial-use allowlist, but the person
creating the profile remains responsible for verifying that declaration and
all dependency licenses. `trust_remote_code` is rejected. GGUF files require
`.gguf`; vision GGUF also requires a projector; audio.cpp requires a supported
family adapter. MLX and vLLM entries likewise require the matching artifact
format and an explicit engine.

The inline external-model form currently represents the primary file as `artifact.path` and an
optional vision file as `artifact.projector`. The accepted successor contract
generalizes this into `artifact.files`, where every file has a role, path, size,
and SHA-256 value. Weights, `mmproj`, MTP or DFlash drafters, tokenizers,
processors, codecs, and adapters then form one atomic artifact bundle. A file
may override the artifact-level repository and revision when it is published
separately. Mica must verify every required file and validate cross-repository
drafter/model compatibility before making that artifact loadable. Optional
speculative acceleration should be a separate artifact variant so the base
model does not require the drafter.

When optional values are omitted, Mica deliberately assumes conservative
limits: 2,048 input tokens, 256 output tokens, one concurrent request, Q8 KV,
and reservations of 4 GiB for Q4, 8 GiB for Q8, or 12 GiB for native weights
(or `size_gib × 1.4 + 0.5`). These defaults prevent optimistic scheduling; they
do not certify compatibility or quality.

## Per-model engine selection

Each schema-5 model entry resolves to one concrete engine and one compatible
artifact, either explicitly or by the selection policy. A profile does not set
one implicit engine for the whole server.
Setup installs the union of engines required by its model entries, and the
proxy routes each model to its selected engine behind the same public URL.
Reusing a Python environment still reconciles the requested packages. Native
audio.cpp setup checks its compiled model-family set and adds missing families
without removing previously installed ones.

The recommended assistant profiles intentionally keep every model in one
runtime family: MLX artifacts use `mlx-lm`, `mlx-vlm`, or `mlx-audio`; the
assistant GGUF artifacts use `llama-cpp` or `audio-cpp`; and the planned GPTQ
artifacts use `vllm`. Bonsai deliberately uses a separate
`prism-llama-cpp` runtime because stock llama.cpp cannot execute its rotated
weights. This keeps disk use, dependencies, and validation boundaries clear.

### Context and memory admission

Reservations include a floor based on artifact bytes, workspace headroom,
and a KV estimate scaled by `max_total_tokens × max_concurrent_requests`.
Artifacts may set `kv_bytes_per_token_f16` from architecture dimensions; when
unknown, text/vision variants use a conservative planning fallback, not a
measured guarantee. Q4/Q8 cache estimates include scale overhead. MLX text and
vision workers pass cache precision and concurrency to mlx-vlm and validate
actual tokenized input, including projected media. Native text prompts are
checked with the engine's own template and tokenizer; projected native media
is additionally bounded by the worker's total context, not an exact separate
media-input quota. Unknown cache settings and excessive outputs are rejected.

MLX workers cap device allocator/wired/cache limits to their reservation.
vLLM uses each worker's GPU allocation rather than the full server allocation;
CPU workers receive an explicit KV-cache byte allocation from context and
concurrency instead of the upstream automatic pool. CPU/vLLM execution still
requires certification on the target Linux hardware.
Admission estimates are not an OS-enforced process-RSS ceiling, and native/GPU
workspace peaks can differ from estimates. Do not treat the admission budget
as a guarantee that total machine memory can never briefly exceed that value.

Engine IDs are strings rather than a closed profile-schema enum. Future image
and music generation can add engine adapters, artifact validators, installers,
and endpoint contracts without changing existing profile documents or the
proxy URL. The `backend` field and `--backend` flag remain only as compatibility
grouping for the currently implemented runtime families.

Active engine manifests live in [`config/engines/`](../config/engines/) and
their typed CMake definitions drive native builds. A new pinned
llama-compatible fork can be
declared in packaged YAML with the existing installer/launcher adapters;
loading arbitrary user engine manifests from `~/.mica/config/engines.d/` is
future work. Profiles reference engine IDs and never contain installation
commands or arbitrary shell fragments.

## Execution fields

| Field | Meaning |
| --- | --- |
| `id` | Stable model ID from the model registry. |
| `engine` | Concrete runtime ID from the engine registry, such as `mlx-lm`, `llama-cpp`, `prism-llama-cpp`, `audio-cpp`, or `vllm`. |
| `artifact.format` | Artifact container/layout consumed by the engine; a format match alone does not prove architecture compatibility. |
| `artifact.id` | Extensible lowercase artifact variant ID, such as `q4`, `q8`, `native`, or `pq2_0`. |
| `artifact: {id, format}` | Selects a known model's registry artifact directly; an external model can instead define `source` and an inline `artifact.path`. |
| `context` | Input, output, and combined token ceilings. |
| `batching.max_concurrent_requests` | Per-model concurrency; engine launch derives its batch-token envelope from context × concurrency. |
| `kv_cache.precision` | `q4`, `q8`, `auto`, `runtime-managed`, or `not-applicable`. |

Mica rejects a workload when its model/backend/quantization is not in
the registry, its token limits are inconsistent, or its total context exceeds
the model's declared local ceiling. A request that asks for more output tokens
than the active workload permits is also rejected.

Engine-to-backend mapping is deterministic:

| Engine | Backend | Python required |
| --- | --- | --- |
| `mlx-lm`, `mlx-vlm`, `mlx-audio` | MLX | Yes |
| `llama-cpp`, `audio-cpp` | GGUF | No |
| `prism-llama-cpp` | GGUF | No |
| `vllm` | vLLM | Yes |

For a GGUF-only profile, setup uses the native C++ hardware detector, compiles
only the referenced native engines, and downloads model files with `curl`.
Neither a Python interpreter nor a virtual environment is part of that path.
Native engine descriptors pin the source URL, revision, isolated runtime
directory, build targets, server executable, launcher adapter, supported
artifact formats, and hardware classes. Adding another llama.cpp-compatible
fork uses the `cmake-llama` installer and `llama-server` launcher without a new
engine-name conditional in setup or serving.

## Residency fields

| Field | Meaning |
| --- | --- |
| `required_ram_gib` | RAM allocation needed to activate this workload; it does not cap use. |
| `required_vram_gib` | Dedicated-VRAM allocation needed by the workload; zero for CPU or unified-memory workloads. It does not cap use. |
| `memory_safety_reserve_gib` | Memory held outside model admission. |
| `maximum_resident_workers` | Optional worker-count ceiling; zero means memory-only. |
| `engine` and `artifact.id` | Concrete engine and artifact chosen for this model. |
| `residency` | `pinned`, `warm`, `on-demand`, or `ephemeral`. |
| `priority` | Higher values are warmed first and evicted later. |
| `startup` | Whether warmup should try to load the model. |
| `idle_seconds` | Idle TTL for non-pinned workers. |

Residency modes behave as follows:

- `pinned`: loaded at startup and never evicted. Setup fails if the pinned
  baseline cannot fit.
- `warm`: normally loaded at startup; it may be evicted for a higher-priority
  request and expires after its TTL.
- `on-demand`: starts stopped and is retained only for its configured TTL.
- `ephemeral`: unloads as soon as its last in-flight request completes.

Mica never evicts an in-flight worker. It first excludes workers that cannot
release memory from a constrained pool—for example, a CPU worker cannot solve
VRAM pressure. Among relevant workers it considers expired workers first, then
ephemeral/on-demand before warm workers, then lower priority, useful memory
relief, older use, and larger reservation. Pinned workers are excluded.

## Validate a new profile

1. Add or select registry artifacts with measured reservations.
2. Keep context within the model card's declared local limit.
3. Run `mica-server plan` against each supported hardware profile.
4. Run `mica-server setup --dry-run` and confirm only intended engines appear.
5. Start the server and inspect `GET /admin/models`.
6. Run the endpoint acceptance and batch suites.
7. Record measured memory, latency, and quality before marking a profile
   production-ready.

An installed schema-5 workload can be activated through
`POST /admin/profile/activate` with `{"profile":"workload-id"}`. The proxy keeps
resident workers whose model, artifact, engine, context, batching, KV-cache,
placement, and memory reservations match the target workload. It drains active
requests before unloading incompatible workers, then warms missing startup
workers. The target workload's RAM/VRAM requirements must fit the global machine
allocation. A missing backend, changed machine allocation, or edited engine
installation still requires `setup` and a server restart. See the
[API reference](api.md#health-and-discovery).
