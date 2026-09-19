# Mica profiles

A Mica profile is a complete task environment. It selects the models needed for
the task and tells Mica exactly how each model is installed, loaded, retained,
and evicted.

Built-in profiles are written in Lua in
[`config/profiles.lua`](../config/profiles.lua). Shareable and user-created
profiles use schema-2 JSON. Installed files are kept in
`<root>/config/profiles/`; the default catalog is
[`profiles/catalog.json`](../profiles/catalog.json) in this GitHub repository.
Older `mica.profile` entries in [`config/models.lua`](../config/models.lua)
remain available for migration and test reproduction.

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
by the profile, and records each concrete model/backend/quantization selection.
Model artifacts are downloaded on the first server start. Startup models are
then warmed in priority order; on-demand models remain stopped until requested.

The default profile is `auto`: it resolves to `mica-assistant-mlx` on Apple
Silicon and `mica-assistant-gguf` elsewhere. Supplying `--backend` or `--quant`
cannot override a schema-2 profile. Change the profile itself so its behavior
remains reproducible.

Setup loads either detected hardware or the exact JSON supplied through
`--hardware-profile`. That same resolved profile controls dependency choice,
vLLM device selection, native CMake flags, and the two-job/16-GiB compilation
guard. It is persisted beside runtime state for auditability.

## Recommended assistant profiles

| Profile | Artifact/engine boundary | Status |
| --- | --- | --- |
| `mica-assistant-mlx` | MLX only: `mlx-lm`, `mlx-audio`, `mlx-vlm` | Runnable on Apple Silicon. |
| `mica-assistant-gguf` | GGUF only: `llama-cpp`, `audio-cpp` | Runnable on macOS, Linux, and WSL; no Python. |
| `mica-assistant-gptq` | GPTQ through vLLM | Listed but blocked until all four modalities pass native vLLM certification. |

The first two contain the same logical assistant set: Spark text, Granite ASR,
Audio8 TTS, and MiniCPM vision/video. The GPTQ profile remains unavailable while
that multimodal set is uncertified; internal validation fixtures are never
advertised as supported models.

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
./build/mica-server profile validate ./my-assistant.json
./build/mica-server profile install-file ./my-assistant.json
./build/mica-server profile edit my-assistant --editor vi

# A file can also be selected directly. Setup installs it under the runtime root.
./build/mica-server setup --profile-file ./my-assistant.json --ram-gib 8
```

`profile edit` uses a temporary draft and replaces the installed profile only
after validation succeeds. The editor must be one executable path; interactive
terminal editors such as `vi` and `nano` are the reliable choices.
Run `setup --profile <id>` after any edit. The server compares the active
schema-2 definition with the setup snapshot and refuses to start if engines,
artifacts, context, batching, residency, or memory policy changed.

## Built-in profiles

| Profile | Backend | Budget cap | Behavior |
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

The command-line RAM/VRAM values are hard upper bounds. A profile may impose a
smaller cap, but it can never increase the user's limit.

## Profile structure

Schema 2 separates execution from residency:

```lua
return {
  schema = 2,

  execution_profiles = {
    ["coder-q4"] = {
      model = "my-coder",
      engine = "llama-cpp",
      artifact = {format = "gguf", quantization = "q4"},
      context = {
        max_input_tokens = 7168,
        max_output_tokens = 1024,
        max_total_tokens = 8192,
      },
      batching = {
        max_concurrent_requests = 1,
        max_batch_tokens = 8192,
        max_queued_requests = 16,
      },
      kv_cache = {precision = "q8", max_tokens = 8192},
    },
  },

  residency_profiles = {
    coding = {
      mode = "interactive",
      maximum_ram_gib = 8,
      memory_safety_reserve_gib = 0.5,
      models = {
        {
          id = "my-coder",
          execution = "coder-q4",
          residency = "pinned",
          priority = 100,
          startup = true,
        },
      },
    },
  },
}
```

The model must first exist in [`config/models.lua`](../config/models.lua), with
a supported artifact and measured memory reservation for the selected backend
and quantization.

JSON profiles use the same separation but refer to a built-in execution by ID:

```json
{
  "schema": 2,
  "id": "my-assistant",
  "mode": "interactive",
  "memory": {
    "maximum_ram_gib": 8,
    "safety_reserve_gib": 0.5,
    "maximum_resident_workers": 2
  },
  "models": [
    {
      "id": "spark-x25-4b",
      "execution": "spark-balanced-single",
      "residency": "pinned",
      "priority": 100,
      "startup": true
    }
  ]
}
```

## External Hugging Face models

A JSON profile may introduce a model that is not in the built-in registry. It
must provide an exact compatibility contract; a Hugging Face URL or model card
alone is not treated as proof that an engine can load the architecture.

```json
{
  "id": "my-text-model",
  "modality": "text-to-text",
  "engine": "llama-cpp",
  "declared_context_tokens": 2304,
  "source": {
    "repository": "owner/original-model",
    "revision": "0123456789abcdef0123456789abcdef01234567",
    "license": "apache-2.0",
    "trust_remote_code": false
  },
  "artifact": {
    "repository": "owner/quantized-model",
    "revision": "89abcdef0123456789abcdef0123456789abcdef",
    "format": "gguf",
    "quantization": "q4",
    "path": "model-q4-k-m.gguf",
    "size_gib": 2.0,
    "reservation_gib": 3.3
  },
  "context": {
    "max_input_tokens": 1792,
    "max_output_tokens": 256,
    "max_total_tokens": 2048
  },
  "residency": "on-demand",
  "idle_seconds": 60
}
```

Both source and artifact revisions must be immutable commit hashes. The
declared license must be on Mica's commercial-use allowlist, but the person
creating the profile remains responsible for verifying that declaration and
all dependency licenses. `trust_remote_code` is rejected. GGUF files require
`.gguf`; vision GGUF also requires a projector; audio.cpp requires a supported
family adapter. MLX and vLLM entries likewise require the matching artifact
format and an explicit engine.

When optional values are omitted, Mica deliberately assumes conservative
limits: 2,048 input tokens, 256 output tokens, one concurrent request, Q8 KV,
and reservations of 4 GiB for Q4, 8 GiB for Q8, or 12 GiB for native weights
(or `size_gib × 1.4 + 0.5`). These defaults prevent optimistic scheduling; they
do not certify compatibility or quality.

## Per-model engine selection

Each schema-2 model entry pins one concrete engine and one compatible
artifact. A profile does not set one implicit engine for the whole server.
Setup installs the union of engines required by its model entries, and the
proxy routes each model to its selected engine behind the same public URL.

The recommended assistant profiles intentionally keep every model in one
runtime family: MLX artifacts use `mlx-lm`, `mlx-vlm`, or `mlx-audio`; GGUF
artifacts use `llama-cpp` or `audio-cpp`; and the planned GPTQ artifacts use
`vllm`. This keeps disk use, dependencies, and validation boundaries clear.

Engine IDs are strings rather than a closed profile-schema enum. Future image
and music generation can add engine adapters, artifact validators, installers,
and endpoint contracts without changing existing profile documents or the
proxy URL. The `backend` field and `--backend` flag remain only as compatibility
grouping for the currently implemented runtime families.

## Execution fields

| Field | Meaning |
| --- | --- |
| `model` | Stable model ID from the model registry. |
| `engine` | Concrete runtime: `mlx-lm`, `mlx-vlm`, `mlx-audio`, `llama-cpp`, `audio-cpp`, or `vllm`. |
| `artifact.format` | Artifact family consumed by the engine. |
| `artifact.quantization` | Exact `q4`, `q8`, or `native` variant. |
| `context` | Input, output, and combined token ceilings. |
| `batching` | Concurrent requests and batched-token limits. |
| `kv_cache.precision` | `q4`, `q8`, `auto`, `runtime-managed`, or `not-applicable`. |

Mica rejects an execution profile when its model/backend/quantization is not in
the registry, its token limits are inconsistent, or its total context exceeds
the model's declared local ceiling. A request that asks for more output tokens
than the active execution profile permits is also rejected.

Engine-to-backend mapping is deterministic:

| Engine | Backend | Python required |
| --- | --- | --- |
| `mlx-lm`, `mlx-vlm`, `mlx-audio` | MLX | Yes |
| `llama-cpp`, `audio-cpp` | GGUF | No |
| `vllm` | vLLM | Yes |

For a GGUF-only profile, setup uses the native C++ hardware detector, compiles
only the referenced native engines, and downloads model files with `curl`.
Neither a Python interpreter nor a virtual environment is part of that path.

## Residency fields

| Field | Meaning |
| --- | --- |
| `maximum_ram_gib` | Profile RAM cap, bounded by `--ram-gib`. |
| `maximum_vram_gib` | Optional discrete-GPU cap, bounded by `--vram-gib`. |
| `memory_safety_reserve_gib` | Memory held outside model admission. |
| `maximum_resident_workers` | Optional worker-count ceiling; zero means memory-only. |
| `execution` | Execution profile selected for this model. |
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

Mica never evicts an in-flight worker. When it needs room, it considers expired
workers first, then ephemeral/on-demand before warm workers, then lower
priority, older use, and larger reservation. Pinned workers are excluded.

## Validate a new profile

1. Add or select registry artifacts with measured reservations.
2. Keep context within the model card's declared local limit.
3. Run `mica-server plan` against each supported hardware profile.
4. Run `mica-server setup --dry-run` and confirm only intended engines appear.
5. Start the server and inspect `GET /admin/models`.
6. Run the endpoint acceptance and batch suites.
7. Record measured memory, latency, and quality before marking a profile
   production-ready.

Profiles are applied during setup and server start; live profile switching is
not implemented yet. Stop the server, rerun setup with the new profile, and
restart it.
