# Machine, engine, model, and workload configuration

Mica's core is a deterministic resolver. It combines facts about one machine,
declarative engine installation recipes, immutable model artifacts, and a
workload-specific inference policy into one auditable runtime plan.

The dependency graph is one-way:

```text
                         generated system profile
                                   │
                                   ▼
inference profile ───────► engine manifest
        │                          │
        ▼                          ▼
model registry ──────────► compatible engine variant
        │                          │
        ▼                          ▼
multi-file artifact          installed runtime
             └──────────┬──────────┘
                        ▼
                  model worker
```

These layers must remain separate. A model is not an engine, a file format is
not a runtime, and an inference profile is not an installation script.

## Implementation status

| Layer | Current alpha | Target contract |
| --- | --- | --- |
| Machine facts and policy | Setup writes `state/hardware.yaml`, retains older hardware snapshots for migration, and creates `config/machine.yaml` only when absent. `plan` reads existing policy and synthetic YAML/JSON hardware fixtures. | Validate per-device reservations on real multi-GPU hardware and retire older hardware snapshots after migration. |
| Engine manifest | Packaged schema-2 `config/engines/*.yaml` records drive installation and declare the adapter endpoints Mica can actually call. | User engine directories and all installer options resolved from manifests. |
| Model/artifact registry | Curated schema-2 `config/model-manifests/*.yaml` records declare modality flows, abilities, operations, pinned artifacts, and compatible engines. GGUF bundle downloads stage and verify files before the completion marker. | Add image/audio-generation adapters and validate drafter launch. |
| Inference profile | Twenty-five built-in self-contained schema-5 YAML workloads contain model collections and intended-use descriptions. Optional engine/artifact selection resolves installed and declared variants; request defaults use operations and inputs. | Retire legacy Lua diagnostics and validate real multi-GPU behavior. |

The packaged YAML path is implemented for all built-in workloads. User engine
directories, speculative MTP/DFlash launch, cross-repository drafter
compatibility proofs, and observed-memory hard caps remain future work.

An engine manifest may track `latest` or pin an immutable Git commit. Setup
records the concrete installed commit; `latest` does not update an existing
working runtime except on explicit `setup --refresh`. Native model artifacts
may declare `minimum_engine_commit` for architecture support. Setup checks Git
ancestry against the installed runtime and rejects an older or unrelated
commit before accepting the profile. This is a structural compatibility gate,
not an inference certification: exact-load and smoke tests remain necessary.
Ling 3.0 Tiny requires the upstream llama.cpp BailingMoE3 merge commit
`373336672029b12e09f272bc027cc801345a3fd6`.

## 1. Generated system profile

The generated hardware file records detected facts only:

```yaml
schema: 1
system:
  os: macos
  arch: arm64
  apple_silicon: true
cpu:
  physical_cores: 10
  logical_cores: 10
memory:
  system_ram_gib: 24
  unified_memory_gib: 24
accelerators:
  - id: "0"
    runtime: metal
    memory_gib: 24
    unified_memory: true
toolchains:
  cmake: true
  cxx: true
  metal: true
  cuda: false
```

It does not contain llama.cpp CMake flags, Python package names, preferred
models, or task policy. Engine-specific data belongs to engine manifests.
User limits such as a 16-GiB build or inference budget belong to the separate
user-owned `config/machine.yaml` policy. CLI memory arguments may override that
global allocation; workload profiles declare the minimum RAM/VRAM required but do
not create a second ceiling. Server config continues to own networking and
credentials. `plan` accepts `--machine-file PATH` to validate an alternate
machine policy without writing user files. Schema-4 startup and live worker
admission track each discrete GPU separately; synthetic two-GPU tests pass.
Observed-memory enforcement and real multi-GPU validation remain future work.

## 2. Engine manifests

An engine is a concrete executable or worker capable of serving compatible
artifacts. Examples include `mlx-lm`, `mlx-vlm`, `llama-cpp`,
`prism-llama-cpp`, and `vllm`.

The active manifests declare:

- source URL and immutable revision, or a verified binary distribution;
- installer and launcher adapter IDs;
- common build settings;
- supported hardware classes and composable CMake options by accelerator;
- runtime directory or Python environment group;
- build targets and executable/module entry point;
- version-check arguments;
- artifact formats and compatibility tags for loading artifacts;
- actual adapter endpoint contracts, including operation and input/output modalities.

It must not contain arbitrary shell fragments. Typed adapters and typed CMake
definitions prevent an engine manifest from becoming an unchecked code-
execution mechanism.

```yaml
schema: 2
id: prism-llama-cpp
status: candidate
backend: gguf
installer: cmake-llama
launcher: llama-server
device_target: gguf
hardware: [cpu, metal, cuda, rocm, sycl, vulkan]
artifact_formats: [gguf]
compatibility_tags: [prism-activation-transform, vision-projector]
endpoint_contracts:
  - {id: chat, operation: chat.generate, transport: http, adapter_call: openai_chat, method: POST, path: /v1/chat/completions, required_inputs: [text], optional_inputs: [image], outputs: [text], streaming: true}
source:
  url: https://github.com/PrismML-Eng/llama.cpp.git
  revision: 9a9394a895b96003ca842a6041cb28ac49a108f7
install:
  runtime_directory: prism-llama.cpp
  server_executable: build-mica/bin/llama-server
  build_targets: [llama-server]
  version_arguments: [--version]
  cmake_definitions:
    common: {CMAKE_BUILD_TYPE: Release, GGML_NATIVE: ON}
    cpu: {}
    metal: {GGML_METAL: ON}
    cuda: {GGML_CUDA: ON}
    rocm: {GGML_HIP: ON}
    sycl: {GGML_SYCL: ON}
    vulkan: {GGML_VULKAN: ON}
```

The resolver composes common settings with independent platform and hardware
features. It does not duplicate `linux-cuda`, `windows-cuda`, and
`linux-rocm` cases unless the engine genuinely requires different behavior.
An explicit CPU/GPU profile request wins over automatic accelerator preference.

Python engine manifests specify an environment group and packages. `mlx-lm`,
`mlx-vlm`, and `mlx-audio` share `mlx`; setup installs only the packages used by
the selected engines. vLLM remains separate because its hardware-specific
PyTorch/plugin constraints can conflict; its device-specific installation
logic still lives in C++. Download tooling uses a small `tools` environment.
A GGUF-only profile installs no Python.

## 3. Models and multi-file artifacts

A model record describes logical identity, modality flows, abilities, license, upstream
provenance, architectural limits, and its available artifacts. An artifact is
one concrete downloadable weight package for one precision and format.

An artifact may contain one file or an ordered bundle of required files. File
roles include:

- primary model weights;
- vision projector (`mmproj`);
- MTP, DFlash, or classic speculative drafter;
- tokenizer and processor data;
- adapter or codec weights;
- engine-specific metadata.

Files do not need to share a repository. The artifact-level repository and
revision are defaults; any file can provide a `source` override containing a
different repository, immutable revision, and commercial-use license. This covers a separately
published `mmproj`, MTP head, or DFlash drafter without pretending that it is a
different logical model. Cross-repository drafters additionally require model,
architecture, tokenizer/vocabulary, and drafter-protocol compatibility
metadata, which the engine adapter validates before launch.

For example, the active
[`ternary-bonsai-2-27b.yaml`](../config/model-manifests/ternary-bonsai-2-27b.yaml)
record includes both the language weights and the BF16 vision projector:

```yaml
schema: 2
id: ternary-bonsai-2-27b
description: Ternary multimodal coding model with a vision projector.
source_repository: prism-ml/Ternary-Bonsai-2-27B-gguf
input_modalities: [text, image]
output_modalities: [text]
abilities: [text_generation, instruction_following, reasoning, image_understanding]
supported_interactions:
  - {operation: chat.generate, required_inputs: [text], outputs: [text]}
  - {operation: chat.generate, required_inputs: [text, image], outputs: [text]}
license: apache-2.0

artifacts:
  - id: pq2_0
    repository: prism-ml/Ternary-Bonsai-2-27B-gguf
    revision: 6ed5e12bf84b7a63069882c91dd9e9218647d17b
    format: gguf
    quantization_type: PQ2_0
    compatible_engines: [prism-llama-cpp]
    required_compatibility: [prism-activation-transform, vision-projector]
    reservation_gib: 12
    files:
      - role: model
        path: Ternary-Bonsai-2-27B-PQ2_0.gguf
        size_bytes: 7206168928
        sha256: 3907dc1658db1f78a9826bf8d5bcb8dc65db0d466388937af57f2294fae62ec1
      - role: vision-projector
        path: Ternary-Bonsai-2-27B-mmproj-BF16.gguf
        size_bytes: 931145856
        sha256: e287342d92332fa3577ed1d42e921dac9370c08da58ba9337fa450f6cc76cfd7
```

All required files form one atomic artifact. Mica must download them into a
staging directory, validate the pinned revision, filenames, sizes, and SHA-256
values, and only then publish the artifact as ready. A partial model without
its required projector or drafter must never become loadable.

Optional acceleration is modeled as another artifact variant rather than a
half-installed required component. For example, `q8` can contain only the
primary model while `q8_with_mtp` contains the same pinned model plus a pinned
`mtp-drafter` from another repository. The latter requires an engine feature
such as `mtp-speculative-decoding`; a DFlash variant similarly requires the
corresponding DFlash feature.

Artifact compatibility is more than file format. Both stock and Prism
llama.cpp read GGUF, but only the Prism fork implements the activation
transform required by these rotated ternary weights. A format match is
necessary, not sufficient.

## 4. Workload profiles

An inference profile is the workload-level composition and runtime policy. It
selects model IDs, optionally pins artifact and engine IDs, then defines context, batching,
KV cache, placement, memory admission, warmup, priority, and eviction.

```yaml
schema: 5
id: mica-coder-bonsai-macos
description: Coding profile for a 24-GiB Apple Silicon host.
selection:
  engine_policy: explicit-only
  defaults:
    - {operation: chat.generate, required_inputs: [text], model: ternary-bonsai-2-27b}
memory:
  required_ram_gib: 16
  required_vram_gib: 0
  safety_reserve_gib: 0.5
  maximum_resident_workers: 1
models:
  - id: ternary-bonsai-2-27b
    artifact: {id: pq2_0, format: gguf}
    engine: prism-llama-cpp
    placement: {mode: fixed, device: "metal:0", gpu_layers: 99}
    residency: warm
    priority: 100
    startup: true
  - id: spark-x25-4b
    artifact: {id: q4, format: mlx}
    engine: mlx-lm
    placement: {mode: fixed, device: "metal:0"}
    residency: on-demand
    priority: 80
    startup: false
  - id: minicpm-v46-thinking
    artifact: {id: q4, format: mlx}
    engine: mlx-vlm
    placement: {mode: fixed, device: "metal:0"}
    residency: on-demand
    priority: 70
    startup: false
```

The installed [coding profile](../config/workloads/mica-coder-bonsai-macos.yaml)
also specifies per-model context, batching, KV cache, and eviction timers; the
excerpt above shows only the selection and residency fields.
`required_vram_gib` is zero because Apple uses one unified RAM pool. The initial
worker limit is one until measured Bonsai Metal memory proves that a second
worker fits inside the reservation. Admission is currently reservation-based;
observed-process enforcement remains required before calling 16 GiB a hard
operating-system limit.

## Resolution and installation

Activating an inference profile performs these steps:

1. Load or generate the system profile.
2. Resolve every model and artifact from the model registry.
3. Resolve every engine ID from the engine registry.
4. Validate artifact format, required features, engine revision, and hardware.
5. Select the most specific compatible engine installation variant.
6. Combine detected hardware with user build limits to derive build jobs and
   memory budget.
7. Install only missing native runtimes and Python environment groups.
8. Verify engine versions and health checks.
9. Download selected artifacts at first server start and verify all required
   GGUF bundle files.
10. Persist the exact resolution before launching workers.
11. Warm, load, and evict workers according to the inference profile.

The runtime state records selected engines, model variants, hardware, and
installed package snapshots. A complete immutable resolution lock and drift
check for every engine/package remains future work; the stock llama.cpp and
audio.cpp manifests still use their existing `latest` revision policy.

## Registry and override rules

Packaged engine manifests under `config/engines/` form the trusted registry.
Another pinned CMake/llama-server fork can be added there without C++ source
changes by selecting the existing typed adapter. Loading user engine manifests
from `~/.mica/config/engines.d/` is not implemented yet. Remote inference
profiles may select known engines but cannot inject installation commands.

The current `backend` field groups legacy MLX/GGUF/vLLM paths. It is not part of
the target user-facing design. Catalog and profile interfaces should migrate to
model ID, artifact ID, engine ID, and hardware target while retaining backend
only as temporary internal compatibility data.
