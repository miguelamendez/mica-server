# mica-server architecture decisions

Status: living design record  
Last updated: 2026-09-18

This document is the authoritative record for mica-server architecture and
implementation status. The README explains how to use released behavior;
validation reports record experimental evidence. When either conflicts with
this document, resolve the conflict before claiming the feature complete.

Status labels:

- **Implemented**: present in the code and covered by an automated or real
  runtime test.
- **Partial**: some implementation or validation exists, but an acceptance
  condition remains.
- **Planned**: accepted design that is not implemented.

## ADR-001: native control plane with optional Python workers

Decision: **accepted; partial**.

- The CLI, hardware detection, setup orchestration, proxy, model scheduler,
  process lifecycle, downloads, state, and authentication belong in C++.
- Lua owns declarative model catalog and scheduling policy configuration.
- Python is allowed only inside a selected engine that requires it (currently
  MLX or vLLM), and in an explicitly requested conversion process that has no
  native equivalent. Planned creative-audio engines follow the same rule and
  are installed only when their use case is selected.
- A GGUF-only installation must not require Python or create a Python virtual
  environment.

Current gap: the hardware-profile prototype is Python and setup always creates
an `environment-tools` Python environment for Hugging Face downloads. Both must
be replaced by native C++ before the bootstrap path is complete.

Rationale: the proxy and GGUF path must remain small, deterministic, easy to
distribute as one binary, and independent of Python package resolution.

## ADR-002: application home and filesystem layout

Decision: **accepted; planned migration**.

The default application home is `~/.mica`. `MICA_HOME` overrides the default,
and an explicit `--root PATH` has highest precedence. All paths must derive from
the resolved application home.

```text
~/.mica/
├── config/
│   ├── config.lua
│   ├── models.lua
│   ├── policy.lua
│   └── profiles/
├── state/
│   ├── runtime.json
│   ├── hardware-profile.json
│   └── downloads.json
├── models/
│   ├── mlx/
│   ├── gguf/
│   └── vllm/
├── environments/
│   ├── mlx/
│   └── vllm/
├── runtimes/
│   ├── llama.cpp/
│   └── audio.cpp/
├── cache/
│   ├── downloads/
│   ├── huggingface/
│   └── uv/
├── staging/
├── logs/
├── run/
└── secrets/
    └── api-key
```

State, configuration, models, environments, runtime source/builds, caches,
temporary staging, logs, process files, and secrets remain separate. Cleaning
an environment must never delete models; evicting a model from memory must
never delete its disk cache. The API key is mode `0600`. Authentication tokens
must not be written into ordinary config or logs.

Current gap: the implementation still defaults to `~/models` and uses the old
directory names.

## ADR-003: isolated, demand-created Python environments

Decision: **accepted; partial**.

- GGUF: zero Python environments.
- MLX only: one persistent minimal MLX serving environment.
- vLLM only: one device-specific vLLM environment.
- MLX and vLLM installed: two environments, because their PyTorch,
  Transformers, NumPy, Triton, tokenizer, and hardware package constraints can
  conflict.
- Conversion-only dependencies use an isolated, temporary `uv` execution
  environment and are removed after successful conversion when practical.
- All environments share `~/.mica/cache/uv` so isolation does not require
  duplicate package downloads and can reuse hard-linked package data.
- `HF_HOME` is scoped to `~/.mica/cache/huggingface` when a Python backend uses
  Hugging Face libraries.

Current gap: MLX setup installs `mlx-audio[all,server]`, MLX-LM, MLX-VLM,
Hugging Face tooling, and PyTorch together. PyTorch is required for specific
conversions such as Audio8 auxiliary `.pth` weights, not for every MLX server.
The serving environment must be reduced and conversion dependencies moved out.

## ADR-004: one versioned hardware profile drives all backends

Decision: **accepted; partial**.

Before installation or compilation, native C++ detection produces
`state/hardware-profile.json` with schema version 1. The profile records:

- OS, OS version, architecture, and WSL status;
- CPU vendor, model, physical cores, and logical cores;
- system RAM;
- every accelerator rather than one collapsed `GPU` value;
- accelerator vendor, name, architecture, driver, APIs, runtime, and stable ID;
- dedicated memory per device when reported by the vendor runtime;
- Apple unified memory as the shared CPU/GPU memory pool;
- whether device memory is known, dedicated, unified, or unavailable;
- available compiler/runtime tools such as Metal/Xcode, CUDA, ROCm/HIP, SYCL,
  Vulkan, CMake, and a C++ compiler;
- selected device target for MLX, llama.cpp, audio.cpp, and vLLM;
- build resource policy.

`--hardware-profile PATH` loads a saved or synthetic profile for reproducible
planning and CI. Detection occurs again on setup unless an explicit profile is
supplied, preventing stale hardware assumptions.

Target policy:

| Hardware | MLX | llama.cpp | audio.cpp | vLLM |
| --- | --- | --- | --- | --- |
| Apple Silicon | Metal | Metal | Metal | vLLM-Metal |
| NVIDIA | unsupported | CUDA | CUDA | CUDA |
| AMD | unsupported | HIP/ROCm | HIP/ROCm | ROCm |
| Intel GPU | unsupported | SYCL, then Vulkan | Vulkan | XPU |
| Google TPU | unsupported | CPU | CPU | TPU |
| No accelerator | unsupported | CPU | CPU | CPU |

An accelerator without its required native compiler falls back to Vulkan when
available, otherwise CPU. TPU detection must never select a nonexistent
llama.cpp or audio.cpp TPU path.

Current gap: profile schema, C++ loading, compile selection, and synthetic tests
exist, but the primary detector is currently `scripts/detect_hardware.py`.
Vendor parsing must move into `src/hardware.cpp`, after which the Python
detector will be deleted.

## ADR-005: bounded native compilation

Decision: **accepted; implemented as a conservative operational guard**.

- Native runtime builds use no more than two concurrent compiler jobs.
- The declared compilation memory budget is at most 16 GiB.
- Native GGUF compilation is refused below 8 GiB system RAM.
- audio.cpp compiles only the catalog families currently required:
  `granite5asr` and `audio8_tts`, not its full model registry.
- Metal, CUDA, HIP, SYCL, or Vulkan flags come from the hardware profile, not
  merely from a user-provided VRAM number.

The two-job limit plus restricted audio.cpp composite is the current practical
memory control. It is not a kernel-enforced aggregate RSS cgroup on macOS; do
not describe it as a hard operating-system memory limit. Build memory must be
observed during the first real build on every hardware family.

Regression tests assert `--parallel 2`, the audio.cpp custom composite, and the
expected per-profile CMake flags.

## ADR-006: current backend selection and future engine dispatch

Decision: **accepted; implemented for selection, partial end to end**.

Setup may install MLX, GGUF, vLLM, or any combination. A server process serves
exactly one backend, selected explicitly when multiple backends are installed.
Installing MLX and GGUF therefore downloads both artifact formats, while Q4 and
Q8 selection downloads both precisions.

The public proxy keeps one base URL and OpenAI-compatible route set. It lists
available models, lazily starts workers, warms priority models, admits work
within the memory budget, and unloads eligible workers deterministically.

Current gaps include true proxy streaming, observed-memory enforcement,
exception-safe worker leases, request queueing, and complete real proxy-to-
worker integration tests.

This is the current compatibility behavior, not the final abstraction. The
schema-2 design in ADR-015 permits one public proxy to dispatch different
models to different concrete engines while retaining one authenticated base
URL. That migration is planned and does not make multi-engine serving an
implemented feature today.

## ADR-007: memory admission and deterministic eviction

Decision: **accepted; partial**.

- `max_ram_gib` limits resident model workers, not the disk cache.
- Dedicated accelerators also use `max_vram_gib`; zero means use detected
  device capacity where the backend supports that interpretation.
- Apple Silicon uses the system/unified-memory RAM budget rather than pretending
  that VRAM is a separate pool.
- A safety reserve remains available to the OS and proxy.
- Warmup priority is text model, ASR, TTS, then vision.
- On demand, unload only zero-inflight workers. Rank eviction by expired idle
  TTL, oldest last use, largest reservation, then stable model ID.
- An idle scale-down timer unloads unused optional models.
- If the required text model plus its accepted drafter exceeds the configured
  budget, setup must report the required minimum rather than silently overcommit.

Current implementation uses catalog reservations. It does not yet enforce
observed process memory or all configured queue/safety fields.

## ADR-008: model acquisition and quantization

Decision: **accepted; partial**.

For every model and backend precision:

1. Resolve and record the immutable original Hugging Face revision.
2. Verify the license. The default catalog permits commercial use and prefers
   Apache-2.0 or MIT. Conditional/community licenses require an explicit
   opt-in license policy, exact terms and thresholds in the model card, and
   interactive acceptance; they are never presented as permissive. Artifacts
   whose terms prohibit the configured use remain excluded.
3. Search for an existing Q4/Q8 artifact with adequate provenance.
4. Adopt and validate that artifact when it matches the pinned source and
   runtime requirements; otherwise download the full source checkpoint.
5. Convert or quantize with the backend's supported tools.
6. Run actual inference, not only load/import tests.
7. Record quality, speed, peak memory, exact protected layers, commands,
   revisions, and output evidence.
8. Publish only after local inference and clean re-download validation pass.
9. Remove full source weights only after every required artifact is verified and
   published; retain immutable provenance metadata.

MLX produces Q4 and Q8 MLX artifacts. Text/vision GGUF produces Q4_K_M and
Q8_0. Audio GGUF precision is selected per architecture: Granite uses Q4_K and
Q8_0, while Audio8 uses Q4_0 and Q8_0 because Q4_K's 256-element blocks leave
too many codec matrices unquantized and produce a larger artifact and resident
set than Q8_0. vLLM is a serving backend with a curated candidate-conversion
path, not a universal converter. The portable matrix uses compressed-tensors:
AutoRound W4A16 group-128 as the primary Q4 candidate, GPTQ W4A16 group-128 as
the Q4 control, and GPTQ W8A16 group-128 as the quality-first Q8 candidate.
Blackwell-only NVFP4/MXFP4 is excluded. Candidate creation never implies
serving eligibility; promotion requires real inference and quality evidence on
the target runtime.

Audio and multimodal quantization must explicitly protect sensitive embeddings,
input/output projections, codebook boundaries, codecs, projectors, or other
quality-critical layers when evidence requires it. Model cards must record
those exclusions and their reasoning.

For Audio8, the 32-element Q4_0 blocks cover the codec matrix shapes that do
not satisfy Q4_K alignment. The accepted Q4_0 artifact passed seeded plain TTS,
zero-shot cloning, and exact Granite ASR round-trip. Blanket Q4_K and mixed
Q4_K-generator/Q8_0-codec candidates are retained as rejected evidence.

## ADR-009: speculative decoding requires evidence

Decision: **accepted; partial**.

A drafter is enabled only when target and draft vocabularies and tokenization
are compatible, output correctness passes, and a representative prompt suite
shows a useful end-to-end speed improvement. Tests cover input sizes 512, 1024,
2048, 4096, 8192, and 16384 tokens where model context allows, longer outputs,
and reasoning, coding, creativity, general knowledge, and summarization tasks.

Spark-X2.5 smaller-sibling classic speculation did not pass the MLX correctness
gate and is disabled. MiniCPM-V 4.6 Thinking has no validated published smaller
sibling or extractable MTP checkpoint in the audited source. A configuration
field alone is not treated as MTP weights.

## ADR-010: repository and collection publishing

Decision: **accepted; implemented for the initial catalog**.

Use one Hugging Face repository per logical model, not one repository per
backend. Each repository contains all validated variants under stable paths,
for example:

```text
model-repository/
├── source-provenance.json
├── README.md
├── mlx/q4/
├── mlx/q8/
├── gguf/model-Q4_K_M.gguf
├── gguf/model-Q8_0.gguf
├── gguf/mmproj.gguf
└── vllm/<supported-format>/
```

The model card links the original source and any adopted upstream quantization,
states the license, modalities, supported engines and artifacts, protected
layers, inference commands, quality results, throughput, and peak memory.
Collections group logical model repositories by capability: ASR, TTS,
text-to-text, image/video-text-to-text, and—when implemented—text-to-image,
text-to-music, and lyrics-to-song. Tags and short descriptions are
machine-readable for the setup UI.

The initial four-model catalog now resolves every backend variant from one
logical repository per model. The repositories and capability collections are
public, and the clean-room verifier checks remote paths, sizes, and LFS hashes
against the publication manifest. vLLM candidates remain intentionally absent
until their production gates pass.

## ADR-011: API, live audio, and smoke validation

Decision: **accepted; partial**.

The proxy exposes one authenticated localhost endpoint with `/v1/models`, chat
and completion routes, ASR transcription, TTS speech, readiness, health, and
administrative model status. Streaming must remain streaming across the proxy;
buffering a worker response is not acceptable.

Live ASR consumes bounded audio chunks and emits incremental transcripts. LLM
generation streams tokens. Live TTS begins synthesizing stable text segments
without waiting for the full answer. End-to-end Python demo clients are allowed
because they are optional examples, not a server dependency.

Every backend smoke test performs real inference. Audio tests preserve generated
files and prompts for listening validation; voice-cloning tests use the same
reference sample when comparing precisions.

## ADR-012: validation matrix and current evidence

Decision: **accepted; active**.

Automated synthetic-profile tests currently cover:

- Apple Metal for llama.cpp and audio.cpp;
- NVIDIA CUDA;
- AMD HIP/ROCm;
- Intel SYCL for llama.cpp and Vulkan for audio.cpp;
- CPU fallback;
- vLLM CUDA, ROCm, XPU, TPU, and CPU package-selection plans;
- two-job native build and restricted audio.cpp model composite.

These tests compile mica-server itself and validate generated commands. They do
not constitute physical CUDA, ROCm, XPU, or TPU compilation. Real hardware CI
or hosts are required before those platform paths can be called validated.

Current real native-runtime evidence on the Apple M4 host:

- llama.cpp Metal: compiled.
- audio.cpp Metal: the earlier unrestricted full-registry build was stopped
  after excessive resource use. The replacement two-job
  `granite5asr,audio8_tts` converter and server build completed successfully;
  observed system-wide free memory remained at least 84% during recorded
  checks. Granite Q4/Q8 ASR and Audio8 Q4/Q8 TTS plus voice cloning have now
  passed real Metal inference; exact results are in the GGUF validation record.
- vLLM-Metal 0.29.0: installed from compatible prebuilt wheels. A supported
  Qwen3 0.6B MLX control passed real single, concurrent, and explicit batch
  inference with a 30% unified-memory fraction. All four catalog targets failed
  before inference at documented architecture/integration boundaries and remain
  disabled. See `docs/validation/vllm-metal-macos.md`.
- vLLM portable candidates: Spark, MiniCPM, Granite Speech, and Audio8 Q4/Q8
  compressed-tensors structural artifacts all passed real Transformers reload
  and batch-2 modality inference under the 16-GiB guard. This does not override
  the native-engine failures or satisfy the 512-record quality gate.
- Project regression suite: 38/38 tests passed after modality calibration and
  batch-smoke plan coverage was added.

No model or backend is publishable solely because a synthetic profile test
passes.

## ADR-013: separate execution and residency profiles

Decision: **accepted; planned**.

Model execution profiles own a concrete engine, artifact format and precision,
context, generated output, reasoning budget, cache policy, batch limits, media
limits, and certified memory. Residency profiles own collection startup order,
priority, pinned/warm/on-demand/ephemeral state, idle timers, and atomic
pipeline groups. The global user memory limit is independent and cannot be
overridden by either profile layer.

Model-card metadata separately records architectural context, disclosed
training context, maximum generated output, reasoning controls, and Mica-tested
limits. Profiles fail above architectural/engine limits and warn (or fail in
certified mode) above training, upstream-recommended, or Mica-tested limits.

The proposed schema and initial profile catalog are documented in
`docs/design/model-execution-and-residency-profiles.md` and
`config/profiles.lua`. The legacy `all/core/quality` runtime profiles remain in
force until the schema-2 loader, validator, engine mappings, and memory
enforcement are implemented and tested.

## ADR-014: vLLM memory limits and model eligibility

Decision: **accepted; partial**.

vLLM consumes one absolute user budget through a backend-specific adapter. On
Apple Silicon, `max_ram_gib / unified_memory_gib` becomes vLLM-Metal's GPU
memory utilization. On discrete CUDA, ROCm, or XPU devices,
`max_vram_gib / device_memory_gib` becomes the utilization. The value is
clamped to 0.01–0.95. CPU and TPU workers do not receive this device-fraction
flag. The selected vLLM allocator must budget weights, runtime overhead, and KV
cache together; artifact file size alone never establishes admission.

A working vLLM installation does not make every catalog model eligible. Each
model and device plugin must pass architecture loading, actual modality-specific
inference, and memory measurement. The Apple M4 control passed, but Spark,
Granite Speech, Audio8, and MiniCPM-V 4.6 did not; their catalog flags remain
disabled. Current evidence is in `docs/validation/vllm-metal-macos.md`.

The utilization flag bounds the backend allocator. Small, medium, and long
profiles now map context and concurrency into vLLM's model-length, sequence,
and batched-token controls; equivalent GGUF profiles map context, parallel
slots, and KV-cache precision into llama.cpp. Observed process-memory hard
enforcement remains a separate future gate—the scheduler currently performs
reservation-based admission and engine allocator limits.

## ADR-015: capability, artifact, engine, and profile are independent

Decision: **accepted; planned**.

Mica keeps four concepts separate:

- **capability** describes the request/result contract, such as ASR, TTS,
  text-to-music, or lyrics-to-song;
- **artifact** describes stored weights and precision, such as MLX Q4, GGUF
  Q8_0, compressed-tensors, TFLite, or a TensorRT engine;
- **engine** is the concrete runtime executable or worker, such as `mlx-lm`,
  `mlx-vlm`, `mlx-audio`, `llama-cpp`, `audio-cpp`, `vllm`, `sa3-mlx`, or
  `diffrhythm-pytorch`;
- **profile** selects a model, exactly one engine, one compatible artifact, and
  certified execution and residency limits.

An execution profile pins exactly one engine. Fallback between engines is not
allowed inside a certified profile because latency, memory, output quality,
batch behavior, and dependencies differ. Instead, an execution set resolves a
hardware/use-case preference to one concrete profile during setup. The chosen
profile is persisted, and setup installs only the transitive engine and artifact
dependencies it names. A music-only Apple installation therefore need not
install CUDA, TensorRT, DiffRhythm, vLLM, or llama.cpp.

The public proxy remains one authenticated URL and routes by capability/model
to engine adapters. Multiple engines behind that proxy are a planned extension;
the current server's one-backend-at-a-time rule remains until dispatch,
lifecycle, streaming, admission, and failure-isolation tests pass.

Accepted future catalog candidates are:

| Capability | Model | Planned engine profiles | Notes |
|---|---|---|---|
| Text-to-music/audio | Stable Audio 3 Small-Music | audio.cpp GGUF Q8, MLX on Apple Silicon, TFLite/XNNPACK on CPU, TensorRT on NVIDIA | Proposed portable default; up to 120-second output |
| Text-to-music/audio | Stable Audio 3 Medium | audio.cpp GGUF Q8 or TensorRT/NVIDIA | Optional quality profile; up to 380-second output |
| Timed lyrics-to-song | DiffRhythm 1.2 Full | PyTorch/CUDA | Optional specialist rather than default music engine |

These models and engines are not implemented or locally validated. Their
complete dependency licenses must pass ADR-008: Stable Audio weights are under
the Stability AI Community License and their text encoder has separate terms;
DiffRhythm code and DiT weights are Apache-2.0 but its full dependency bundle
still requires a component audit. Stable Audio Open Small is not a planned
default because Stable Audio 3 Small-Music covers the portable role with
materially longer output.

audio.cpp now exposes Stable Audio 3 Small/Medium through its `stable_audio`
family, but Mica's pinned build currently compiles only `granite5asr` and
`audio8_tts`. Selecting a future native music profile will add
`stable_audio` to the use-case-specific build; it must not expand every
audio.cpp installation by default. Official optimized MLX, TFLite, and
TensorRT profiles remain separate engines with independent evidence.

`config/profiles.lua` schema 2 documents the proposed engine registry,
engine-pinned profiles, and hardware-resolved execution sets. The existing
`--backend` CLI remains a compatibility surface until the schema-2 loader and
engine-aware installer are implemented.

## ADR-016: one agent chat with ASR, VLM, and TTS utilities

Decision: **accepted; implemented prototype on MLX**.

The user-facing chat has one main LLM and one conversation history. ASR turns a
voice note into either the instruction (voice-only) or supporting context
(text plus voice). The VLM is a bounded native tool invoked by the main LLM for
images, videos, and rendered document pages. TTS is a response utility enabled
when the input includes voice. File paths are session-scoped handles and must
pass an exact upload allowlist before a tool can open them.

The non-stream endpoint returns one final JSON response. The stream endpoint
preserves Granite transcript deltas, Spark answer deltas, tool activity, and
Audio8 WAV chunks as typed server-sent events. Audio8 `arktts` does not yet
decode frames incrementally, so its progressive mode uses bounded sentence
calls to the native speech endpoint; it must not be described as frame-level
codec streaming. Browser microphone capture currently uploads after recording
stops, so token streaming after upload is not continuous microphone ingestion.

Sessions persist temporary JSON plus uploads, rendered pages, and generated
audio beneath the Mica runtime root. The implemented prototype still uses one
global agent-session mutex and supports native document rendering only for PDF.
Per-session locking, idle cleanup, broader document conversion, and a realtime
microphone transport remain open.

The complete state and event contract is documented in
`docs/design/agent-chat-state-machine.md`.

## Change discipline

Every material architecture change must update this document in the same
change. Experimental results belong in `docs/validation/`; accepted decisions
and their implementation status belong here. A feature moves to **Implemented**
only when its stated acceptance evidence exists.
