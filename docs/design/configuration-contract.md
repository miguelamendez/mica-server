# Mica configuration contract

Status: Machine policy, schema-2 engine/model manifests, schema-5 workload YAML, automatic engine/artifact selection, and synthetic per-device reservation accounting are implemented. Strict process-memory enforcement and Linux vLLM/multi-GPU certification remain open.
Updated: 2026-10-09

This contract defines **four layers**, not four interchangeable meanings of
"profile": machine, engine, model, and workload. The catalog is an index of workload
definitions. Server network settings and credentials are a separate service
configuration.

## Names and ownership

| Layer | Canonical path | Writer | Version |
| --- | --- | --- | --- |
| Detected machine facts | `~/.mica/state/hardware.yaml` | Detector only | `schema: 1` |
| Machine policy | `~/.mica/config/machine.yaml` | User/setup defaults only | `schema: 1` |
| Engine manifest | `config/engines/<id>.yaml` | Mica package/trusted registry | `schema: 2` |
| Model manifest | `config/model-manifests/<id>.yaml` | Mica package/trusted registry | `schema: 2` |
| Workload profile | `config/workloads/<id>.yaml`, or a user-selected YAML file | Mica/user | `schema: 5` |
| Server settings | `~/.mica/config/server.json` | User/TUI/CLI | `schema: 1` (old unversioned files readable) |

See the [configuration schema index](../configuration-schemas.md) for the exact
schema files, native validation and version rules for all six contracts.

The machine layer has two physical files because rediscovery may rewrite facts
but **must never rewrite user policy**. The resolver combines them into one
effective machine view. `~/.mica/config/server.json` continues to own host,
port, UI, and API-key configuration; secrets never belong in a profile.

## Machine contract

`hardware.yaml` contains only observations: OS/architecture, CPU, physical
RAM, accelerators, each accelerator's memory and unified/dedicated status,
runtime/driver, and installed toolchains. It must not contain a requested
model, budget, build-job count, or preferred device. A CUDA driver failure is
reported as a detection/health finding, not as proof the PCI device is absent.

`machine.yaml` contains policy ceilings and permissions, for example:

```yaml
schema: 1
allowed_devices: [cpu, "cuda:0"]
limits:
  inference:
    ram_gib: 16
    dedicated_memory_gib: {"cuda:0": 12}
    cpu_threads: 8
  build:
    ram_gib: 16
    parallel_jobs: 2
```

All limits are optional. Omitted means inherit a safe detected/default value;
an explicit zero for a dedicated-memory device means **do not use that device
for model memory**, rather than "unlimited." Invalid or unavailable device IDs
are errors. Apple Metal's unified allocation is charged to system RAM only;
it cannot also consume a separate `dedicated_memory_gib` allowance. Do not
call an admission estimate a kernel-enforced hard memory cap.

## Engine contract

An engine manifest defines a concrete installer and launcher, source revision
policy, supported hardware and artifact formats, typed hardware-specific build
options, runtime features, executable/module path, and version/health checks.
It contains neither model weights nor user memory policy. Mica installs an
engine only when a selected task resolves to it. `latest` must resolve to and
record a concrete revision; an installed engine changes only on explicit
refresh. A manifest cannot supply arbitrary shell commands.

## Model contract

A model manifest describes one logical model and any number of concrete
artifact–engine choices. Each artifact records immutable source and revision,
format/quantization, required files and roles (including projector or
drafter), integrity checks, required engine features, and an ordered list of
compatible engines. The first listed engine is only a preference: selection
also requires compatible hardware, feature support, and certification.

The following metadata is optional, never fabricated, and accompanied by a
source when the value is not directly measured by Mica:

- identity, source model, license and commercial-use terms;
- input and output modalities and task capabilities (text, image, video,
  audio, ASR, TTS, image/audio generation, etc.); a model may have several;
- supported context ceiling, disclosed training context, supported output
  ceiling, and disclosed training output span as **four distinct values**;
- exact model-specific reasoning modes, budget controls, tool-call format,
  and supported generation methods;
- task-tagged *recommended* sampling presets, each with provenance or local
  validation evidence.

Training spans are hints about the model's training exposure, not claims of
quality at every point in the span. Missing values remain absent. Agent
workflows and tool availability are task concerns; a model records only its
ability to emit/consume the corresponding format. Model presets provide
defaults, not an unchangeable sampler.

The current model parser records modalities, thinking modes, optional
provenance-backed token claims, and tool-call format identifiers. It validates
supported output limits and warns in `plan` when disclosed useful/trained spans
are exceeded. No training span is inferred from a tokenizer or architecture
window; generation presets are not yet implemented.

## Workload contract

A workload profile chooses a collection of models, optional operation/input
defaults, artifact variants or an allowed selection rule, optional engine
pins, placement, context/output limits, batching, KV precision, warmup,
priority, residency and eviction. It declares RAM and dedicated-memory
requirements, not execution steps or a RAM/VRAM usage cap: the global
machine allocation is the ceiling. If that allocation cannot satisfy the
declared requirements and the selected model reservations, setup rejects the
workload without installing or loading it.

Schema-5 workloads may omit engine and artifact for registered models.
`selection.engine_policy` defaults to `prefer-installed`: choose an installed
compatible engine before model-manifest order. `manifest-order` follows that
order; `explicit-only` requires both pins. Missing artifacts prefer Q4, then
Q8, then the first remaining artifact ID in sorted order. Omit `engine` for
automatic selection rather than declaring an engine ID named `auto`.
Generation/sampling presets and execution-workflow definitions remain future
work, not accepted workload fields.

Resolution checks compatible hardware and artifact–engine pairs. The exact
chosen artifact, engine revision, device and effective settings are written
to runtime state. A format match alone is not sufficient (for example, a
specialized GGUF may require a fork-specific feature). Engine minimum-revision
checks occur during setup; compatibility resolution alone is not inference
certification.

## Resolution and validation order

1. Parse and validate every file independently; do not allow unknown fields
   to silently change meaning. Refuse unsupported schema versions with a
   migration message.
2. Detect/load machine facts; merge user machine policy without modifying
   either source file. Reject a policy that requests an unavailable device.
3. Resolve task model IDs, artifact files, and engine candidates; verify
   license, integrity metadata, feature and revision requirements.
4. Resolve placement and budgets. Effective resources are bounded by current
   physical availability, machine ceilings, and any narrower CLI override;
   the task contributes minimum requirements, not another ceiling. CPU RAM
   and each discrete accelerator's memory are
   independent pools; unified memory is one RAM pool.
5. Validate generation: active `input + reserved output` must fit the
   native context and any verified `max_output_tokens` limit. A disclosed
   `recommended_context_tokens` total produces a warning when exceeded,
   not a hard error. Unknown values produce no invented threshold. Training
   disclosures and output recommendations are preserved in model references,
   not conflated with maximum supported values. Task generation
   settings override model recommendations; permitted request settings may
   narrow or override task defaults without escaping hard constraints.
6. Persist a resolved snapshot with selected revisions and settings before
   installing or launching workers. Replans and hot-swaps compare against
   that snapshot; an implicit `latest` update is not a silent runtime change.

Global machine limits are **admission ceilings** in the current alpha. A
strict process-memory ceiling is a separate implementation/testing milestone;
until then, plans and the UI must not promise that RSS can never exceed the
configured number.

## Current-to-target mapping

The prior implementation exposed an ownership inconsistency: `HardwareInfo`
stored build-memory and job-count limits, and the legacy JSON hardware
snapshot serialized them as `build_policy`, while generated YAML omitted that
policy. New hardware snapshots no longer serialize `build_policy`; old JSON
input may still contain it for migration. Setup must use machine policy for
build limits without changing measured hardware facts.

| Current | Target | Migration note |
| --- | --- | --- |
| `state/system-profile.yaml` and `state/hardware-profile.json` | `state/hardware.yaml` | Keep JSON only for a one-time migration/fixture path; remove build policy from detected facts. |
| CLI/server memory options and per-profile memory fields | `config/machine.yaml` global ceilings plus `config/workloads/*.yaml` workload requirements | CLI may narrow a run but must not silently raise machine policy; workload requirements are not extra usage ceilings. |
| `config/engines/*.yaml` | Same path | Extend typed fields only where current manifests still defer to code. |
| Curated model definitions in Lua and YAML | `config/model-manifests/*.yaml` | All curated model records now load from YAML; the Lua file retains diagnostic profile definitions only. |
| `config/inference-profiles/*.yaml` and `config/profiles.lua` | `config/workloads/*.yaml` | Twenty-five built-in workload definitions are standalone schema-5 YAML; the old execution-template file was removed. |
| `schemas/profile-v3.schema.json` | Machine-policy, engine-v2, model-v2, and workload-v5 schemas | Legacy schema-3 remains only for reading older user/remote documents during migration. |

## Step 1 acceptance boundary

This document establishes ownership, names, precedence, missing-value
semantics, and failure/warning rules. The implementation now includes a
machine-policy resolver, authoritative packaged engine/model YAML, and
schema-5 workload YAML with synthetic-hardware tests.
Setup creates `config/machine.yaml` only if absent and writes the new
`state/hardware.yaml` alongside migration snapshots. Existing user policy
is not overwritten. Schema-4 startup and live-worker admission now track
discrete memory per GPU in synthetic tests; real multi-GPU behavior and a
hard observed-memory ceiling are not certified. No live user config, model
cache, or remote machine was changed during these local tests.
