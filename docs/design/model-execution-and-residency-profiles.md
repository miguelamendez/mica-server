# Model execution and residency profiles

Status: accepted design; profile catalog proposed; runtime loader not implemented  
Last updated: 2026-09-17

The proposed machine-readable catalog is `config/profiles.lua`. It is not loaded
by the current legacy profile parser. Every execution profile is `candidate` or
`experimental` until its exact context, cache, batch, quality, and peak-memory
combination passes on real hardware; unimplemented additions remain `planned`.

## Four separate concepts

| Concept | Meaning | Example |
|---|---|---|
| Capability | What the model does | `asr`, `tts`, `text-to-music` |
| Artifact | Stored format and precision | MLX Q4, GGUF Q8_0, TFLite |
| Engine | Concrete executable/runtime | `mlx-vlm`, `llama-cpp`, `sa3-mlx` |
| Profile | Certified engine configuration and scheduling policy | context, batching, memory, residency |

An **execution profile** configures one model worker: a required concrete
engine, artifact format and precision, input/output/total context, thinking
budget, cache precision, batch slots, media limits, and measured memory. The
engine is singular: a certified profile must not silently fall back to another
engine because its memory, speed, and quality evidence would no longer apply.

A **residency profile** configures the collection: startup order, load priority,
which workers stay pinned, which remain warm, and when idle workers are
offloaded. Keeping these layers separate lets the same Spark worker definition
participate in an interactive collection, a text-only batch collection, or a
realtime voice pipeline.

The user hard limit is outside both profile layers and always wins. A profile
whose pinned group cannot fit is rejected before workers start.

An **execution set** is resolved once during setup from the hardware profile
and use case. It selects one concrete execution profile, persists that choice,
and gives the installer the exact engine dependency closure. This permits a
music-only installation to add `sa3-mlx` without installing DiffRhythm,
TensorRT, or unrelated Python packages. Engines may share an environment only
when a tested lockfile proves their dependencies compatible; otherwise each
selected engine gets its own minimal environment under `~/.mica`.

The existing `backend` CLI remains a compatibility interface until schema 2 is
implemented. It currently groups serving stacks and artifact families. New
code must not use that overloaded term where it means a concrete runtime.

## Proposed residency profiles

| Profile | Behavior | Primary trade-off |
|---|---|---|
| `interactive` | Pins balanced Spark, warms ASR then TTS, loads vision on demand | Default responsiveness inside an 8-GiB-class budget |
| `quality-interactive` | Uses Q8 text/TTS/vision choices and allows warm workers to move | Higher fidelity, more reloads and memory |
| `balanced-all` | Tries to warm all four models in priority order | Best for 12-GiB-class or larger limits |
| `text-batch` | Pins Spark Q4 with Q4 KV and four bounded sequences | Aggregate text throughput; other modalities are ephemeral |
| `long-context` | Gives one Spark worker most of the budget with Q4 KV | Larger single request; no parallel model residency guarantee |
| `realtime-voice` | Atomically pins ASR + Spark + TTS | Low pipeline latency; requires their certified peaks to fit together |
| `vision-quality` | Pins MiniCPM Q8/Q8 and loads other models on demand | Image/video quality and predictable vision latency |
| `low-memory` | Allows only one ephemeral worker at a time | Minimum residency; every model switch may reload |

The 4/8/12/16-GiB hardware tiers select among these policies. They do not assert
that a model fits: the solver still checks the selected engine's certified
memory envelope and the user's actual hard limit.

## Planned creative-audio additions

These entries are design/catalog candidates only; they are not installed,
served, or validated by mica-server yet.

| Use case | Default model | Planned engines | Selection intent |
|---|---|---|---|
| Portable text-to-music | Stable Audio 3 Small-Music | `audio-cpp`, `sa3-mlx`, `sa3-tflite`, `sa3-tensorrt` | Default: up to 120 seconds; native GGUF or official optimized engine |
| Quality text-to-music | Stable Audio 3 Medium | `audio-cpp`, `sa3-tensorrt` | Optional higher-quality/longer-output profile, up to 380 seconds |
| Timed lyrics-to-song | DiffRhythm 1.2 Full | `diffrhythm-pytorch` | Optional CUDA specialist, up to 285 seconds |

Stable Audio 3 Small-Music supersedes Stable Audio Open Small for the proposed
default: Open Small's short-output niche does not justify another engine and
artifact path. Current audio.cpp supports the Stable Audio 3 Small/Medium
family as GGUF 16/Q8, so Mica can offer a no-Python native profile without
conflating that engine with the official MLX/TFLite/TensorRT paths. The pinned
audio.cpp build does not include that family yet; it remains planned work.

Stable Audio weights use the Stability AI Community License (and their text
encoder carries its own terms), so setup must show the exact terms and require
explicit acceptance. DiffRhythm code and DiT weights are Apache-2.0, while the
complete dependency bundle must still be audited component by component before
Mica calls it permissive or redistributes it.

Upstream references:

- [Stable Audio 3 runtimes and model matrix](https://github.com/Stability-AI/stable-audio-3)
- [audio.cpp Stable Audio route](https://github.com/0xShug0/audio.cpp/blob/main/docs/music_generation.md#stable-audio)
- [DiffRhythm 1.2 Full checkpoint](https://huggingface.co/ASLP-lab/DiffRhythm-1_2-full)

## Precision choices

Weight and KV precision are independent:

- Q8 weights + Q8 KV: quality-first single-request work.
- Q4 weights + Q8 KV: balanced default for text and vision.
- Q4 weights + Q4 KV: batch or long-context capacity.
- Q8 weights + Q4 KV: valid future quality/long-context option after testing.
- Native KV: required as a quality/speed baseline, not the low-memory default.

MLX-VLM supports `kv_bits`, key/value split precision, group size,
`quantized_kv_start`, `max_kv_size`, and `max_num_seqs`. A profile with Q4/Q8
KV explicitly sets `quantize_after_tokens`; otherwise MLX's current 5,000-token
default can leave a large native-precision prefix. The proxy additionally
enforces aggregate batch tokens, which the worker flag alone does not express.

ASR has no autoregressive text KV cache. Audio8 has DualAR/runtime-specific
caches and must not inherit MLX-VLM text-cache flags. Those profiles use
`not-applicable` or `runtime-managed` and are certified separately.

## Admission and offloading

Before admitting a request, the proxy reserves weights, fixed runtime
workspace, media/codec workspace, maximum request KV, concurrent slots, and a
safety margin. If the envelope does not fit, it evicts only idle eligible
workers in this order:

1. ephemeral;
2. on-demand;
3. warm;
4. lowest numeric priority;
5. oldest last use;
6. shortest reload cost as final tie-breaker.

Active workers are queued behind, not killed. Pinned workers are never
automatically evicted. The proxy unloads selected workers, confirms memory has
fallen below the load watermark, and only then launches replacements. Disk
artifacts are never deleted by memory offloading.

## Context and reasoning validation

For autoregressive models:

```text
effective input = system + conversation + user + tools + media tokens
effective input + max output <= profile max total <= architecture/engine max
max reasoning + minimum visible answer <= max output
sum(active request totals) <= max batch tokens
```

Exceeding an architectural/engine limit is always an error. Exceeding a
published training context, upstream recommendation, or Mica-tested context is
a warning in experimental mode and an error in certified mode. Unknown training
limits produce an explicit "cannot verify training-context safety" warning.

For ASR and TTS, validators use modality-specific units: audio/reference
duration, packed text/audio positions, frames, and codec workspace.

## Work still required

1. Replace the legacy `all/core/quality` parser with schema-2 loading.
2. Validate model-card metadata and profile references at startup.
3. Implement the engine registry, execution-set resolver, minimal per-engine
   installer, and compatibility mapping from the current `backend` CLI.
4. Map generic fields to MLX, llama.cpp/audio.cpp, vLLM, and future creative
   audio engine flags.
5. Add request token/media estimation and aggregate batch admission.
6. Add observed worker-memory watermarks and exception-safe leases.
7. Certify each candidate with real Q4/Q8, Q4/Q8-KV, context, concurrency, and
   quality tests; write measured idle/peak memory into the profile.
8. Define and test proxy routes for `text-to-music` and `lyrics-to-song`.
9. Enable deterministic hardware recommendations only for certified profiles.
