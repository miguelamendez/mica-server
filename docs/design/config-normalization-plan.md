# Configuration normalization plan

Status: Machine, packaged engine/model manifests, and 23 task profiles are wired into YAML. Synthetic per-device VRAM accounting is implemented. Legacy diagnostic profiles, strict memory enforcement, and real-hardware certification remain open.
Date: 2026-09-30

## Goal and naming

Mica resolves four conceptual layers in order: **machine → engine + model →
task**. An engine is a runtime; a model is a logical capability with one or
more concrete artifacts; a task is a set of model selections and operating
policies. The remote catalog is a distribution index, not a fifth profile
type. Server networking and authentication remain service configuration, not
machine hardware policy.

| Layer | Proposed canonical file | Owner | Contents |
| --- | --- | --- | --- |
| Machine facts | `~/.mica/state/hardware.yaml` | Detector | OS, CPU, installed toolchains, RAM, accelerators and their physical/unified memory. |
| Machine policy | `~/.mica/config/machine.yaml` | User | Devices Mica may use; global RAM/VRAM-per-device, CPU-thread, build-memory and parallel-job ceilings. |
| Engine | `config/engines/<id>.yaml` | Mica package | Source/revision, hardware features, typed install/build/launch adapters, supported artifact formats and runtime features. |
| Model | `config/model-manifests/<id>.yaml` | Mica package or trusted registry | Identity, provenance, license, capabilities, optional training/limit metadata, and pinned artifact–engine choices. |
| Task | `config/tasks/<id>.yaml` | Mica package or user | Model set, artifact/engine selection, placement, context, generation preset, batching, KV policy, warmup, priority, residency, eviction, and RAM/VRAM requirements. |

The first two files are **one machine layer with separate ownership**. A
detector must not overwrite user limits during rediscovery. If a single
`machine.yaml` is later required for UI display, generate a read-only resolved
view; do not make a detector rewrite an editable policy file.

## Current YAML inventory

- Generated machine state: `~/.mica/state/hardware.yaml` after setup;
  `~/.mica/state/system-profile.yaml` and `hardware-profile.json` are compatibility copies. No machine
  YAML is checked into the repository.
- Engine manifests (`config/engines/`): `audio-cpp.yaml`, `llama-cpp.yaml`,
  `mlx-audio.yaml`, `mlx-lm.yaml`, `mlx-vlm.yaml`, `prism-llama-cpp.yaml`,
  `vllm.yaml`, `xing-llama-cpp.yaml`.
- Model manifests (`config/model-manifests/`): eight YAML records, including
  Granite ASR, Audio8 TTS, Spark, MiniCPM, Bonsai, Ling, Xing, and the hidden
  vLLM control. Production model records no longer come from Lua.
- Built-in tasks (`config/tasks/`): 23 self-contained schema-4 YAML files.
- Installed user task files remain authoritative when their ID matches a
  packaged task. A preexisting schema-3 file can therefore shadow a new
  schema-4 built-in; `profile list` flags it for explicit migration instead
  of rewriting user data.
- Published profile bundle: `profiles/catalog.yaml`, currently containing
  `mica-assistant-mlx`, `mica-assistant-gguf`,
  `mica-assistant-gguf-cpu`, `mica-assistant-gguf-gpu`,
  `mica-assistant-gguf-mixed`, `bonsai-pq2-vision-cpu`,
  `bonsai-pq2-vision-gpu`, `mica-coder-bonsai-macos`,
  `mica-xing-q4-baseline-macos`, and `mica-assistant-gptq`.
- Test-only YAML: `tests/fixtures/profiles/custom-assistant.yaml`,
  `external-gguf.yaml`, and `unsafe-remote-code.yaml`.
- Schemas: `schemas/machine-policy-v1.schema.json`, `engine-v1.schema.json`,
  `model-v1.schema.json`, and `task-v4.schema.json`. The server still uses JSON
  for port/authentication settings. `config/models.lua` contains deprecated
  acceptance/test profiles only; `config/profiles.lua` has been removed.

## Proposed ownership and precedence

1. **Machine.** Physical facts are generated. User limits are explicit
   ceilings, not replacements for detected capacity. Record per-accelerator
   VRAM rather than one aggregate pool. Apple unified memory belongs to one
   RAM pool, not an independent VRAM allowance. Keep API keys and ports in
   server config.
2. **Engine.** Keep typed build and launch options in engine manifests. The
   resolver selects CPU, Metal, CUDA, ROCm, XPU, and other features using the
   detected hardware and machine policy. Engine installation is requested by
   a selected task, not by the mere presence of a manifest.
3. **Model.** A model can have many pinned artifacts, each with its own
   compatible engine list and required files (weights, projector, drafter,
   tokenizer, etc.). Optional metadata includes input/output modalities,
   task capabilities, tool-call format, model-specific reasoning mode values
   and budget controls, supported generation methods, and per-task sampling
   recommendations. Keep `supported_context_tokens`, disclosed
   `trained_context_tokens`, declared/supported output ceiling, and disclosed
   `trained_output_tokens` separate. Unknown values remain absent; provenance
   accompanies claims. Training lengths are quality hints, not hard runtime
   limits or guarantees.
4. **Task.** Select model IDs and artifacts, optionally pin an engine, and
   choose task-specific generation/sampling settings. When engine is `auto`,
   choose the first *compatible and certified artifact–engine pair* under a
   documented deterministic order, then save the exact resolution. The task
   declares RAM/VRAM requirements and may impose context, output, batching,
   KV, and residency rules. The machine allocation alone caps memory use.
   Agent tools/workflow belong here; the model
   only declares relevant capabilities.
5. **Requests.** Request overrides must remain inside the resolved task and
   model limits. Reject an unsupported context/output or unavailable device;
   warn when a chosen span exceeds a disclosed trained span. Validate that
   input plus reserved output fits the active context. For memory, the
   effective ceiling is the minimum of available physical capacity and
   machine/CLI policy; task requirements are eligibility checks. Existing
   admission reservations are **not** an
   OS-enforced hard memory cap; report that limitation until enforcement is
   implemented and measured.

## Migration sequence

1. **Freeze the baseline.** Record the current YAML/Lua inventory, active
   profile IDs, generated state paths, and existing Mac/Linux test results.
   Do not modify the user's `~/.mica` files or download models in this step.
2. **Specify schemas and resolver rules.** The ownership, names, precedence,
   warning, and failure rules are now drafted in the Step 1 contract. Next,
   define machine facts/policy,
   engine, model, and task schemas with optional-field and provenance rules.
   Publish one precedence table and a dry-run resolved-plan format. Version
   the breaking task schema rather than silently reinterpreting schema 3.
3. **Normalize the machine layer.** Emit generated `hardware.yaml`; create
   user-editable `machine.yaml` with conservative defaults and a one-time
   migration from the prior resolved runtime memory settings. Preserve user policy on
   every rediscovery. Add CPU, discrete GPU, multiple GPU, and unified-memory
   fixtures. Step 2 now provides `schemas/machine-policy-v1.schema.json`,
   a YAML parser, pure policy resolver, CUDA/Metal and CPU/multi-GPU tests,
   `plan`/`setup` integration, and create-only user policy persistence. A
   synthetic per-device scheduler checks are implemented; strict
   observed-memory enforcement remains later work.
4. **Complete engine and model manifests.** Migrate remaining curated Lua
   records; finish typed vLLM installation options. Validate exact artifact
   roles, revisions, hashes, licenses, engine features, and model-specific
   optional metadata. Never invent training lengths or tool-call support.
5. **Normalize tasks and catalog.** Move each bundled task to its own YAML,
   including assistant CPU/GPU/mixed variants. Keep a lightweight published
   catalog of task IDs, descriptions, availability, and source paths. Update
   download, list/filter, plan, setup, serve, profile hot-swap, and UI paths
   together. Retire Lua/old profile readers only after a one-time migration
   and explicit validation; do not maintain indefinite dual authorities.
6. **Prove behavior before release.** Unit-test schema failures, resolver
   precedence, engine auto-selection, context/output warnings and failures,
   memory accounting, and artifact bundles. Dry-run synthetic Mac/CPU/CUDA
   hardware, then perform bounded real cold setup, inference, batching,
   hot-swap, and memory measurements on Mac and the Linux host once reachable.
   Update README, CLI/API docs, examples, and migration instructions. Do not
   claim a hard memory ceiling until observed enforcement passes.

## Quick primary-source check

- [Hugging Face Transformers configuration](https://huggingface.co/docs/transformers/main/main_classes/configuration)
  and [generation configuration](https://huggingface.co/docs/transformers/main_classes/text_generation)
  separate model architecture from generation parameters; generation settings
  can be overridden per call. This supports model defaults plus task/request
  overrides, not one fixed sampler for every task.
- [Hugging Face model cards](https://huggingface.co/docs/hub/model-cards)
  use YAML metadata for discovery, task and license information. They do not
  make an undisclosed training context or output length knowable; Mica should
  store such claims only with provenance.
- [llama.cpp server options](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md)
  expose runtime context, GPU placement, concurrency, and KV-cache types.
  [vLLM serve options](https://docs.vllm.ai/en/latest/cli/serve/) likewise
  expose model length, batching, and memory controls. These are resolved
  execution policy, not intrinsic model identity.

## Step 2a verification

The machine policy parser/resolver was compiled with one build job. macOS
rejected an attempted 15 GiB `ulimit -v`, so this build was **not** protected
by a hard virtual-memory limit; read-only RSS checks during the build observed
the compiler below 1 GiB. Synthetic CPU, Metal, one- and two-GPU resolution,
invalid device/capacity, unified-memory, and explicit zero-GPU cases passed.
The complete CTest run passed 61/62 inside the managed sandbox; the API-auth
loopback test passed on its own with local network access, yielding 62/62
verified tests. No real model or remote host was used.

Step 2b added `--machine-file`, machine limits and device checks in plan/setup,
and a create-only `config/machine.yaml`. If that file is absent and a prior
`state/runtime.json` exists, Mica imports its last resolved RAM ceiling and,
only for one detected discrete GPU, its VRAM ceiling. Multiple-GPU legacy
VRAM totals remain ambiguous and are not imported. Without old state, the
minimal file inherits detected capacity. The CPU-thread limit is applied to llama.cpp generation
and prefill workers; other engines do not yet share one thread control. Real setup writes
`state/hardware.yaml` alongside the older snapshots. Synthetic CUDA and
CPU-only integration cases passed; all 65 tests passed when the one
loopback-restricted API-auth test was retried with local network access. No
real model or remote host was used. Subsequent normalization added separate
startup and live-worker reservation pools for each detected GPU, with a
synthetic two-GPU test. The global VRAM option is a per-device cap; machine
policy may narrow each GPU further. This still does not enforce observed
process memory or prove real concurrent multi-GPU execution.

## Local normalization verification

The normalized registry loads all eight engine manifests, eight model
manifests, and 23 schema-4 built-in task files. The installable published
catalog entries parse against the same registry; blocked entries remain
unavailable. The single-job macOS build and all 67 current CTest cases pass, including
synthetic CPU, Metal, CUDA, ROCm, XPU, one-/two-GPU placement, machine-policy
migration, startup admission, profile validation, local API authentication,
and an offline live task-activation fixture.
The API fixture now uses the pinned model revisions so it cannot accidentally
start a model download when checking authentication.

These are contract, planner, and local-control-plane tests. They do not certify
real model inference, model-quality metadata, vLLM installation on non-Mac
hardware, concurrent multi-GPU loading, or a hard observed-memory ceiling.
No model weights were intentionally downloaded for this normalization work;
an unintended API-test download attempt was stopped and its temporary process
cleaned up before the fixture was corrected.
