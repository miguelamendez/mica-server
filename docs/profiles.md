# Mica profiles

A Mica profile is a complete task environment. It selects the models needed for
the task and tells Mica exactly how each model is installed, loaded, retained,
and evicted.

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
[`config/tasks/mica-coder-bonsai-macos.yaml`](../config/tasks/mica-coder-bonsai-macos.yaml).
All 23 built-in task profiles are self-contained schema-4 YAML in
[`config/tasks/`](../config/tasks/). Shareable and user-created profiles use
the same format. Installed files are kept in
`<root>/config/profiles/`; the default catalog is
[`profiles/catalog.yaml`](../profiles/catalog.yaml) in this GitHub repository.
The authoritative machine-readable contract is
[`schemas/task-v4.schema.json`](../schemas/task-v4.schema.json). JSON
remains the runtime/API state format, but it is not accepted for profiles.
Older `mica.profile` entries in [`config/models.lua`](../config/models.lua)
remain only for legacy acceptance/test reproduction; they are not the
authoritative task definitions.

An already-installed YAML profile under `<root>/config/profiles/` takes
precedence over a packaged task with the same ID. `profile list` marks an
installed schema-3 profile with `migration_recommended: true`; Mica does not
silently replace user-edited files. To prepare reviewed schema-4 copies in a
new directory, use `profile export-all --output DIRECTORY`, then validate and
install the desired files individually.

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
cannot override a schema-4 profile. Change the profile itself so its behavior
remains reproducible.

Setup loads detected hardware or a saved YAML/JSON profile supplied through
`--hardware-profile`. It writes detector-owned facts to `state/hardware.yaml`
and temporarily keeps the older `state/system-profile.yaml` and JSON copy.
Separately, `config/machine.yaml` is user-owned policy: `plan` reads it,
`setup` creates a minimal version only when absent, and neither command
overwrites an existing policy. Use `--machine-file PATH` to plan or set up
against another YAML policy. The global machine allocation is the memory
ceiling; a task profile declares the RAM/VRAM it requires. Setup rejects the
task when the allocation is smaller. Schema-4 tasks reserve discrete memory
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

The first two contain the same logical assistant set: Spark text, Granite ASR,
Audio8 TTS, and MiniCPM vision/video. The GPTQ profile remains unavailable while
that multimodal set is uncertified; internal validation fixtures are never
advertised as supported models.

For the 24-GiB Apple Silicon coding profile, inspect and set up with the
explicit 16-GiB reservation budget:

```sh
./build/mica-server profile show mica-coder-bonsai-macos
./build/mica-server plan --profile mica-coder-bonsai-macos --ram-gib 16
./build/mica-server setup --profile mica-coder-bonsai-macos --ram-gib 16
./build/mica-server serve
```

Bonsai is the default chat model and warms first; Ling Q4 and Spark Q4 are
on-demand text alternatives, while MiniCPM serves document/image analysis.
Ling Q8 is registered in the model manifest but is not selected by this profile;
choose it in a copied YAML profile for a quality comparison. Ling's MLX Q4
community conversion requires a separate `rapid-mlx` implementation, so it is
not registered against Mica's current `mlx-lm` engine. The profile allows one
resident worker, so a request for another model evicts the warm worker rather
than trying to keep all four loaded. This is reservation-based admission, not
an operating-system-enforced 16-GiB process limit. Setup and the Bonsai Metal
inference path still need real-machine validation before treating the profile
as certified.

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
schema-4 definition with the setup snapshot and refuses to start if engines,
artifacts, context, batching, residency, or memory policy changed.

## Built-in profiles

| Profile | Backend | Declared RAM requirement | Behavior |
| --- | --- | ---: | --- |
| `interactive` | MLX | 8 GiB | Pins text, warms ASR/TTS, loads vision on demand. |
| `quality-interactive` | MLX | 16 GiB | Uses Q8 for text, TTS, and vision where configured. |
| `balanced-all` | MLX | 12 GiB | Attempts to warm all four capabilities. |
| `text-batch` | MLX | 8 GiB | Four text sequences; utility models are ephemeral. |
| `long-context` | MLX | 12 GiB | Single text request with the certified local context ceiling. |
| `realtime-voice` | MLX | 8 GiB | Pins ASR, text, and TTS for a voice pipeline. |
| `vision-quality` | MLX | 8 GiB | Pins Q8 vision and loads other utilities on demand. |
| `low-memory` | MLX | 4 GiB | At most one resident worker; models are loaded per request. |
| `gguf-interactive` | GGUF | 8 GiB | Native equivalent of the interactive workflow. |
| `gguf-quality-interactive` | GGUF | 16 GiB | Native Q8-oriented workflow. |
| `gguf-text-batch` | GGUF | 8 GiB | Native batched text workflow. |
| `gguf-long-context` | GGUF | 12 GiB | Native single-request context workflow. |
| `gguf-low-memory` | GGUF | 6 GiB | At most one native worker; no Python environment. |

The machine/command-line RAM and VRAM values are the admission ceilings.
Task requirements are eligibility checks, not smaller usage caps. Omitting
`--vram-gib` (or passing `auto`) derives the discrete-device capacity; an
explicit `--vram-gib 0` disables discrete VRAM. Apple Metal and other detected
unified-memory devices charge complete model reservations to the RAM pool.

## Profile structure

Schema 4 puts the task's model selection and runtime policy in one YAML file.
There is no separate execution-template lookup:

```yaml
schema: 4
id: my-assistant
description: Compact local text assistant
mode: interactive
default_chat_model: spark-x25-4b
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
or be defined as an external model in the task. Its selected artifact must
name the same engine and a compatible format. The parser rejects unknown
fields, duplicate model IDs, impossible context/output limits, and unsupported
artifact–engine pairs.

## External Hugging Face models

A YAML profile may introduce a model that is not in the built-in registry. It
must provide an exact compatibility contract; a Hugging Face URL or model card
alone is not treated as proof that an engine can load the architecture.

```yaml
schema: 4
id: my-external-assistant
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

Each schema-4 model entry pins one concrete engine and one compatible
artifact. A profile does not set one implicit engine for the whole server.
Setup installs the union of engines required by its model entries, and the
proxy routes each model to its selected engine behind the same public URL.

The recommended assistant profiles intentionally keep every model in one
runtime family: MLX artifacts use `mlx-lm`, `mlx-vlm`, or `mlx-audio`; the
assistant GGUF artifacts use `llama-cpp` or `audio-cpp`; and the planned GPTQ
artifacts use `vllm`. Bonsai deliberately uses a separate
`prism-llama-cpp` runtime because stock llama.cpp cannot execute its rotated
weights. This keeps disk use, dependencies, and validation boundaries clear.

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

Mica rejects a task when its model/backend/quantization is not in
the registry, its token limits are inconsistent, or its total context exceeds
the model's declared local ceiling. A request that asks for more output tokens
than the active task permits is also rejected.

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
| `required_ram_gib` | RAM allocation needed to activate this task; it does not cap use. |
| `required_vram_gib` | Dedicated-VRAM allocation needed by the task; zero for CPU or unified-memory tasks. It does not cap use. |
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

An installed schema-4 task can be activated through
`POST /admin/profile/activate` with `{"profile":"task-id"}`. The proxy keeps
resident workers whose model, artifact, engine, context, batching, KV-cache,
placement, and memory reservations match the target task. It drains active
requests before unloading incompatible workers, then warms missing startup
workers. The target task's RAM/VRAM requirements must fit the global machine
allocation. A missing backend, changed machine allocation, or edited engine
installation still requires `setup` and a server restart. See the
[API reference](api.md#health-and-discovery).
