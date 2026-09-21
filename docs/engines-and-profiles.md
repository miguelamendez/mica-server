# System, engine, model, and inference profiles

Mica's core is a deterministic resolver. It combines facts about one machine,
declarative engine installation recipes, immutable model artifacts, and a
task-specific inference policy into one auditable runtime plan.

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
| System profile | Native detection writes `state/hardware-profile.json`; saved profiles can drive plans and tests. | Versioned YAML generated from machine facts, with JSON retained only for runtime/API state if needed. |
| Engine manifest | Executable descriptors live in `config/profiles.lua`; native llama-compatible forks already use generic installer/launcher adapters. | One schema-validated YAML manifest per engine, loaded from the packaged registry or `~/.mica/config/engines.d/`. |
| Model/artifact registry | Lua model records describe Q4/Q8 and exact pre-packed artifacts; GGUF projectors are separate artifact fields. | Versioned model records whose artifacts contain a general list of required files and compatibility requirements. |
| Inference profile | Shareable schema-3 YAML is implemented and JSON profile files are rejected. | YAML continues to select model, artifact, engine, execution limits, placement, and residency without installation commands. |

The standalone system/engine/model YAML migration is accepted design, not yet
a released feature. Documentation and CLI output must continue to distinguish
the target contract from the current Lua/JSON-backed implementation.

## 1. Generated system profile

The system profile records detected facts only:

```yaml
schema: 1
system:
  os: macos
  architecture: arm64
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
User limits such as a 16-GiB build or inference budget belong to server config
or command-line policy rather than detected machine facts.

## 2. Engine manifests

An engine is a concrete executable or worker capable of serving compatible
artifacts. Examples include `mlx-lm`, `mlx-vlm`, `llama-cpp`,
`prism-llama-cpp`, and `vllm`.

Each manifest owns:

- source URL and immutable revision, or a verified binary distribution;
- installer and launcher adapter IDs;
- common build settings;
- composable platform, accelerator, architecture, and toolchain requirements;
- runtime directory or Python environment group;
- build targets and executable/module entry point;
- version and health checks;
- artifact formats and required runtime features;
- streaming, batching, offload, and unload capabilities.

It must not contain arbitrary shell fragments. Typed adapters and typed CMake
definitions prevent an engine manifest from becoming an unchecked code-
execution mechanism.

```yaml
schema: 1
id: prism-llama-cpp
status: candidate
engine_family: llama-server

source:
  type: git
  url: https://github.com/PrismML-Eng/llama.cpp.git
  revision: 9a9394a895b96003ca842a6041cb28ac49a108f7

compatibility:
  formats: [gguf]
  features:
    - prism-activation-transform
    - vision-projector

install:
  adapter: cmake
  runtime_directory: prism-llama.cpp
  targets: [llama-server]
  common:
    definitions:
      CMAKE_BUILD_TYPE: Release
      GGML_NATIVE: true
  accelerators:
    cpu:
      provides: [cpu]
      definitions: {}
    metal:
      requires: {accelerator_runtime: metal}
      provides: [cpu, metal]
      definitions: {GGML_METAL: true}
    cuda:
      requires: {accelerator_runtime: cuda, toolchain: nvcc}
      provides: [cpu, cuda]
      definitions: {GGML_CUDA: true}
    rocm:
      requires: {accelerator_runtime: rocm, toolchain: hipcc}
      provides: [cpu, rocm]
      definitions: {GGML_HIP: true}
  resource_estimate:
    minimum_memory_gib: 4
    estimated_memory_per_job_gib: 4

launch:
  adapter: llama-server
  executable: build-mica/bin/llama-server
  version_arguments: [--version]
```

The resolver composes common settings with independent platform and hardware
features. It does not duplicate `linux-cuda`, `windows-cuda`, and
`linux-rocm` cases unless the engine genuinely requires different behavior.
An explicit CPU/GPU profile request wins over automatic accelerator preference.

Python engines use the same contract with an `uv` adapter and an environment
group. `mlx-lm`, `mlx-vlm`, and `mlx-audio` may share `mlx` when their pinned
requirements resolve together. vLLM remains separate because its hardware-
specific PyTorch and plugin constraints can conflict. Download tooling remains
in a small `tools` environment. A GGUF-only profile installs no Python.

## 3. Models and multi-file artifacts

A model record describes logical identity, capability, license, upstream
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
different repository and immutable revision. This covers a separately
published `mmproj`, MTP head, or DFlash drafter without pretending that it is a
different logical model. Cross-repository drafters additionally require model,
architecture, tokenizer/vocabulary, and drafter-protocol compatibility
metadata, which the engine adapter validates before launch.

For example, Bonsai's `pq2_0` artifact contains both the language weights and
the BF16 vision projector:

```yaml
schema: 1
id: ternary-bonsai-2-27b
capabilities: [text-to-text, image-text-to-text]
license: apache-2.0

artifacts:
  pq2_0:
    repository: prism-ml/Ternary-Bonsai-2-27B-gguf
    revision: 6ed5e12bf84b7a63069882c91dd9e9218647d17b
    format: gguf
    quantization: PQ2_0
    files:
      - role: model
        path: Ternary-Bonsai-2-27B-PQ2_0.gguf
        size_bytes: 7206168928
        sha256: 3907dc1658db1f78a9826bf8d5bcb8dc65db0d466388937af57f2294fae62ec1
      - role: vision-projector
        path: Ternary-Bonsai-2-27B-mmproj-BF16.gguf
        size_bytes: 931145856
        sha256: e287342d92332fa3577ed1d42e921dac9370c08da58ba9337fa450f6cc76cfd7
    compatibility:
      engines: [prism-llama-cpp]
      required_features:
        - prism-activation-transform
        - vision-projector
    reservation_gib: 12
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

## 4. Inference profiles

An inference profile is the task-level composition and runtime policy. It
selects exact model, artifact, and engine IDs, then defines context, batching,
KV cache, placement, memory admission, warmup, priority, and eviction.

```yaml
schema: 3
id: mica-coder-bonsai-macos
description: Coding profile for a 24-GiB Apple Silicon host.
memory:
  maximum_ram_gib: 16
  maximum_vram_gib: 0
  safety_reserve_gib: 0.5
  maximum_resident_workers: 1
models:
  - id: ternary-bonsai-2-27b
    artifact: pq2_0
    engine: prism-llama-cpp
    placement: {mode: fixed, device: "metal:0", gpu_layers: 99}
    residency: warm
    priority: 100
    startup: true
  - id: spark-x25-4b
    artifact: mlx-q4
    engine: mlx-lm
    placement: {mode: fixed, device: "metal:0"}
    residency: on-demand
    priority: 80
    startup: false
  - id: minicpm-v46-thinking
    artifact: mlx-q8
    engine: mlx-vlm
    placement: {mode: fixed, device: "metal:0"}
    residency: on-demand
    priority: 90
    startup: false
```

This example is the target shape, not yet an installed catalog profile.
`maximum_vram_gib` is zero because Apple uses one unified RAM pool. The initial
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
9. Download every artifact bundle on demand and verify all required files.
10. Persist the exact resolution before launching workers.
11. Warm, load, and evict workers according to the inference profile.

The resolved state records the hardware fingerprint, engine source revision,
selected variant, rendered arguments, environment package lock, artifact
revision and hashes, and inference-profile digest. Setup is idempotent when
that state still matches. Updates create a new resolution and must not silently
follow an upstream `latest` branch.

## Registry and override rules

Packaged engine manifests form the trusted default registry. User manifests
may be added beneath `~/.mica/config/engines.d/`, but an ID collision must fail
unless the user explicitly authorizes an override. Remote profiles may select
known engines; they must not inject arbitrary installation commands.

The current `backend` field groups legacy MLX/GGUF/vLLM paths. It is not part of
the target user-facing design. Catalog and profile interfaces should migrate to
model ID, artifact ID, engine ID, and hardware target while retaining backend
only as temporary internal compatibility data.
