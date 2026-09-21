# GitHub source-install and NVIDIA validation plan

Status: active; Phase A complete, awaiting clean-host validation
Created: 2026-09-20
Target: user-provided Linux/NVIDIA validation host
Installation source: a clean clone of the public Mica GitHub repository

## Objective

Validate the experience a new Linux/NVIDIA user receives from the repository:
clone Mica, follow only the published instructions, detect the machine, build
the control plane, install the profile-selected engines, download models on
demand, and run real inference through the public API. Local build products or
model directories must not be copied to the target.

The run also validates two extension boundaries:

1. explicit CPU-only, GPU-only, and mixed CPU/GPU model placement; and
2. adding a model that requires a new engine rather than an existing backend
   executable.

Every validation step updates the execution record and findings ledger in this
document before the next step begins. A command succeeding does not make the
step pass if the required action was missing, surprising, or undocumented.

## Safety and access rules

- Ask the user before every command executed on the remote host, including
  SSH, SCP, and rsync invocations. Do not request a reusable approval rule.
- Never print, inspect, log, or commit the temporary SSH password.
- Use a clean validation root such as `~/.mica-validation`; do not alter an
  existing `~/.mica` installation.
- Cap project and engine compilation at two parallel jobs and 16 GiB of RAM.
- Inspect free disk space before downloading weights. Stop before a download
  if the expected artifact plus temporary-file allowance cannot fit.
- Do not use destructive cleanup commands without separate approval. Record
  exactly which temporary paths are eligible for later removal.
- Do not create a release during implementation or source-install validation.
  Push only the tested candidate, and create `v0.2.0-alpha.1` only after the
  clean-host run passes and the user explicitly approves the release step.

## Pass criteria

The validation passes only when all of the following are demonstrated from a
fresh GitHub clone:

- Mica detects the actual Linux distribution, CPU, RAM, NVIDIA GPU, VRAM,
  driver, and CUDA toolchain correctly.
- The README or linked getting-started guide contains every prerequisite and
  command needed on the target.
- YAML is the only accepted profile interchange format; JSON profiles fail
  with a useful error.
- Setup installs only the engines required by the selected profile and chooses
  build flags from the detected hardware profile.
- Model artifacts, engines, environments, configuration, secrets, state, and
  logs remain under the selected Mica root.
- CPU, GPU, and mixed profiles respect independent RAM and VRAM limits and the
  configured per-model placement.
- Health, model discovery, administration, chat, streaming chat, image input,
  ASR, TTS, and batch/concurrent calls perform real inference where the active
  profile exposes that modality.
- vLLM passes CUDA control-model inference and batching. This does not certify
  the blocked four-modality GPTQ profile.
- A new `prism-llama-cpp` engine and Ternary Bonsai model can be registered,
  installed, downloaded, loaded, called, and removed from memory without
  modifying unrelated engine behavior.
- Observed RAM/VRAM, latency, throughput, and failures are recorded.

## Profiles under test

| Profile | Intended placement | Initial limits | Purpose |
| --- | --- | ---: | --- |
| `mica-assistant-gguf-cpu` | All four GGUF models on CPU | 16 GiB RAM, 0 GiB VRAM | Prove GPU presence does not override fixed CPU placement. |
| `mica-assistant-gguf-gpu` | All four GGUF models on accelerator 0 | 8 GiB RAM, 16 GiB VRAM | Prove full GPU offload and VRAM admission. |
| `mica-assistant-gguf-mixed` | Spark/MiniCPM on GPU; Granite/Audio8 on CPU | 8 GiB RAM, 12 GiB VRAM | Prove simultaneous, independent resource pools. |
| `vllm-control` | CUDA | Hardware-derived | Validate vLLM installation, serving, streaming, and batching only. |
| `bonsai-pq2-vision` | Configurable CPU or accelerator 0 | Derived after measurement | Exercise third-party engine extensibility and multimodal inference. |

Limits are scheduler reservations, not claims about measured peak memory. The
run must compare reservations with process RSS and `nvidia-smi` observations,
then correct the profile values when they are not conservative.

## Ternary Bonsai extension case

Register the model as a new vision-capable model and the fork as a distinct
engine. It must not be presented as stock `llama-cpp`.

| Field | Required value |
| --- | --- |
| Model ID | `ternary-bonsai-2-27b` |
| Modality | `img-text-to-text` |
| License | Apache-2.0 |
| Hugging Face repository | `prism-ml/Ternary-Bonsai-2-27B-gguf` |
| Pinned model revision | `6ed5e12bf84b7a63069882c91dd9e9218647d17b` |
| Language artifact | `Ternary-Bonsai-2-27B-PQ2_0.gguf` |
| Vision projector | `Ternary-Bonsai-2-27B-mmproj-BF16.gguf` |
| Quantization/packing | `PQ2_0`, preserved as its own identifier |
| Engine ID | `prism-llama-cpp` |
| Engine source | `https://github.com/PrismML-Eng/llama.cpp` |
| Runtime directory | `<mica-root>/runtimes/prism-llama.cpp` |

The current Prism documentation says Bonsai 2 requires the Prism fork because
its rotated weights need the fork's activation transform. Stock llama.cpp must
not be used even if it appears to recognize the architecture. The current
Bonsai demo pins release `prism-b10709-9a9394a`; before implementation, resolve
that release to a full immutable commit and record it in Mica's engine registry.
Never mix this fork's `ggml-*` libraries with the stock llama.cpp runtime.

The PQ2 language artifact is approximately 7.21 GB and the BF16 projector is
approximately 0.93 GB. Reserve additional space for partial downloads, engine
builds, logs, and runtime workspace. First validate text inference, then image
grounding through the BF16 projector, followed by CPU and full-GPU-offload
measurements. Use a conservative context during initial certification instead
of immediately exercising the advertised 262K ceiling.

Upstream references:

- [Ternary Bonsai 2 model and files](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf)
- [Pinned model revision](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf/commit/6ed5e12bf84b7a63069882c91dd9e9218647d17b)
- [Prism llama.cpp fork](https://github.com/PrismML-Eng/llama.cpp)
- [Bonsai demo source of truth](https://github.com/PrismML-Eng/Bonsai-demo)

### Extension questions this case must answer

1. Can an engine be declared in data, or does adding one require scattered C++
   conditionals?
2. Can setup associate its own repository, revision, build flags, executable,
   health route, and supported artifact types with that engine?
3. Can Mica represent `PQ2_0` without pretending it is Q4, Q8, or native?
4. Can the model declare a required projector and download both artifacts
   atomically?
5. Can profiles choose CPU, GPU, or mixed placement for the new engine?
6. Can runtime isolation prevent the Prism and stock llama.cpp libraries from
   being combined?
7. Can model-list and admin endpoints describe the engine, packing, processor,
   artifact sizes, and actual residency accurately?
8. Are errors actionable when the engine revision, PQ2 artifact, projector,
   CUDA capability, memory, or disk space is unsuitable?

## Step-by-step execution plan

### Phase A — local release candidate

- [x] Define and enforce profile schema 3 in a standalone JSON Schema document.
- [x] Finish YAML-only profile parsing, emission, catalog installation, editing,
      and `.json` rejection.
- [x] Finish per-model device selection and independent RAM/VRAM scheduling.
- [x] Add CPU, GPU, and mixed profiles with deterministic placement.
- [x] Add engine metadata capable of representing `prism-llama-cpp`.
- [x] Make quantization/packing identifiers extensible enough for `PQ2_0`.
- [x] Register Ternary Bonsai, its exact model revision, both files, license,
      context ceiling, initial conservative context, and provisional memory.
- [x] Add automated positive and negative tests for all preceding behavior.
- [x] Update README, installation, profile, model, engine, and API docs.
- [x] Build with two jobs and pass the complete local suite.
- [x] Commit and push the exact candidate to GitHub.

### Phase B — clean GitHub installation

- [ ] Record target disk availability and basic OS/hardware information using a
      read-only remote command.
- [ ] Clone the public GitHub repository into a new directory.
- [ ] Follow only README/linked documentation to install prerequisites.
- [ ] Configure and build Mica with two parallel jobs.
- [ ] Run the repository test suite.
- [ ] Run `mica-server detect` and save the generated hardware profile beneath
      `~/.mica-validation/state/`.
- [ ] Compare detected values with OS tools and document every mismatch.

### Phase C — setup and engine installation

- [ ] Dry-run each selected profile before making changes.
- [ ] Verify stock llama.cpp/audio.cpp CUDA flags and the 16-GiB/two-job guard.
- [ ] Run GGUF setup and inspect the resulting runtime state.
- [ ] Run vLLM setup and verify the CUDA-specific environment.
- [ ] Run Bonsai setup and verify the separately pinned Prism engine directory.
- [ ] Confirm setup is idempotent and does not reinstall unchanged engines.
- [ ] Confirm changing profiles installs only newly required engines.

### Phase D — model download and inference

- [ ] Start from an empty validation model cache.
- [ ] Trigger first-use downloads through Mica rather than manual placement.
- [ ] Verify checksums/sizes, completion markers, partial-download recovery, and
      exact repository revisions.
- [ ] Run CPU-only profile endpoint and memory tests.
- [ ] Hot-swap and run GPU-only profile endpoint and memory tests.
- [ ] Hot-swap and run mixed profile endpoint and concurrent-load tests.
- [ ] Force safe memory pressure and validate eviction priority and pinning.
- [ ] Run vLLM control inference, streaming, and batch/concurrent tests.
- [ ] Run Bonsai PQ2 text inference on CPU and GPU.
- [ ] Run Bonsai image grounding with the BF16 projector.
- [ ] Confirm stock llama.cpp is never selected for the Bonsai artifacts.

### Phase E — usability and recovery

- [ ] Repeat installation using only the final revised documentation.
- [ ] Test missing compiler, missing CUDA toolkit, insufficient disk, bad API
      key, unavailable model, interrupted download, port collision, and invalid
      YAML/profile placement errors.
- [ ] Restart Mica and confirm state/cache reuse.
- [ ] Validate profile installation from GitHub and from a local YAML file.
- [ ] Verify API-key enforcement and profile hot-swap endpoints.
- [ ] Record remaining platform-specific assumptions and unsupported paths.

### Phase F — closeout

- [ ] Add measured latency, throughput, RAM, VRAM, startup, and download results.
- [ ] Resolve every blocking or correctness finding and rerun affected steps.
- [ ] Classify unresolved limitations explicitly; do not silently mark them as
      supported.
- [ ] Push fixes and final evidence to GitHub.
- [ ] With explicit approval, create `v0.2.0-alpha.1` binaries and reinstall
      from that prerelease on the validation host.
- [ ] With approval, remove only the recorded validation directories and delete
      the temporary local password file.

## Execution record

Append one row immediately after every step, including failed commands.

| Time (UTC) | Step | Git commit | Command/action | Expected | Observed | Result | Finding IDs |
| --- | --- | --- | --- | --- | --- | --- | --- |
| — | Planning | working tree | Defined validation protocol | Reproducible plan | Plan created before remote execution | Pass | F-001–F-004 |
| 2026-09-20 21:43 | A1 | working tree | Build and focused schema-3/YAML CLI round-trip | YAML validates and JSON/schema 2 fail | Built with two jobs; built-in and external YAML validated; generated YAML revalidated; JSON rejected by extension | Pass | F-001, F-002 |
| 2026-09-20 21:43 | A1 | working tree | Install into fresh temporary prefix | Binary, YAML catalog, and formal schema are packaged | All three installed beneath the prefix | Pass | F-001 |
| 2026-09-20 21:43 | A1 | working tree | Complete local CTest suite | 51 tests pass | 50 passed in sandbox; loopback API-auth test could not retain its listener | Environmental retry | F-005 |
| 2026-09-20 21:43 | A1 | working tree | Retry API-auth test with local loopback access | Authentication integration passes | Passed in 0.24 seconds; aggregate result 51/51 | Pass | F-005 |
| 2026-09-20 21:43 | A1 | working tree | Retry generated-YAML order and validation | Contract identifiers appear first and output remains valid | Output begins with `schema: 3`, then `id`; generated profile revalidated | Pass | F-006 |
| 2026-09-20 22:16 | A2 | working tree | Audit placement and resource-pool implementation | CPU, unified, and discrete-GPU accounting are distinct | Found and fixed unified-memory auto-budgeting, native runtime translation, and irrelevant-pool eviction defects | Pass | F-007–F-009 |
| 2026-09-20 22:16 | A2 | working tree | Run CPU, GPU, mixed, CPU-host rejection, and explicit-zero-VRAM tests | Deterministic device and RAM/VRAM plans | All 6 focused tests passed; CPU startup reserves 7.3/0 GiB, GPU 1.5/7.3 GiB, mixed 3.0/4.8 GiB | Pass | F-007–F-009 |
| 2026-09-20 22:16 | A2 | working tree | Run complete local CTest suite | 56 tests pass | 55 passed in sandbox; API-auth alone failed because its loopback listener could not remain active | Environmental retry | F-005 |
| 2026-09-20 22:16 | A2 | working tree | Retry API-auth with local loopback access | Authentication integration passes | Passed in 0.24 seconds; aggregate result 56/56 | Pass | F-005 |
| 2026-09-20 23:36 | A3 | working tree | Resolve and register the Prism engine and Bonsai PQ2 artifacts | Immutable engine/model revisions and exact artifact metadata are represented without stock llama.cpp fallback | Prism is pinned to `9a9394a895b96003ca842a6041cb28ac49a108f7`; model revision, both filenames, sizes, SHA-256 values, packing, and license are registered | Pass | F-003, F-004 |
| 2026-09-20 23:36 | A3 | working tree | Build with two jobs and run focused Bonsai/engine tests | Dynamic packing, CPU/CUDA setup plans, registry data, and engine isolation pass | All 5 focused tests passed; CPU and CUDA setup plans build only the pinned Prism `llama-server` target | Pass | F-003, F-004 |
| 2026-09-20 23:36 | A3 | working tree | Run complete local CTest suite | 60 tests pass | 59 passed in the sandbox; API-auth alone could not retain its loopback listener | Environmental retry | F-005 |
| 2026-09-20 23:36 | A3 | working tree | Retry API-auth with local loopback access | Authentication integration passes | Passed in 0.24 seconds; aggregate result 60/60 | Pass | F-005 |
| 2026-09-20 23:36 | Repository attribution | `c4f6dd5` | Inspect Git history and GitHub's public contributors API | Only the intended GitHub account is attributed | Reachable history and the REST contributor list contain only `miguelamendez`, but the repository homepage still displays stale pre-rewrite `miguel-flowstate` attribution | Pending cache refresh | F-011 |
| 2026-09-20 23:42 | Repository attribution | `c4f6dd5` | Trace stale contributor identity | Identify whether an active ref still contains the old author | No branch or tag contains it; unreachable pre-rewrite commits used `miguel@flowstatehq.com`, which explains the cached account association | Pass | F-011 |
| 2026-09-21 17:51 | A4 | `d8f3fb1` | First candidate push | Push with the intended repository owner | macOS's cached Git credential attempted the push as `miguel-flowstate`; GitHub rejected it with HTTP 403 | Fail, corrected | F-012 |
| 2026-09-21 17:51 | A4 | `d8f3fb1` | Authenticate `miguelamendez` alongside the existing account and retry with a command-scoped credential helper | Push without removing or globally replacing the other account | Push succeeded; both keyring accounts remain registered | Pass | F-012 |
| 2026-09-21 17:51 | A4 | `d8f3fb1` | Verify public `main` through Git and GitHub REST | Remote SHA and attribution match the tested candidate | Public `main` resolves to `d8f3fb1c6cde918a4d228791360994681fb261ec`, authored and committed by `miguelamendez` | Pass | — |

## Findings ledger

Use these categories:

- `DOC`: documentation missing, misleading, or out of order;
- `BUG`: implemented behavior is incorrect;
- `IMPL`: required behavior is absent or structurally hard-coded;
- `UX`: behavior works but is unnecessarily difficult or surprising;
- `ENV`: target-machine or third-party limitation;
- `EVIDENCE`: behavior lacks enough measurement to certify.

Severity is `blocking`, `high`, `medium`, or `low`. A finding remains open until
the fix is committed and the relevant step has been rerun successfully.

| ID | Category | Severity | Status | Step | Observation | Required correction | Fix commit | Retest evidence |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| F-001 | IMPL | high | Closed | A1 | Profile structure was enforced in several C++/Lua locations but had no standalone authoritative schema. | Schema 3 document, strict native validation, packaging, and round-trip tests are present. | `d8f3fb1` | Focused schema tests pass. |
| F-002 | DOC | high | Closed | A1 | README and profile documentation described shareable profiles as JSON. | Documentation now uses schema-3 YAML and states that JSON profile files are rejected. | `d8f3fb1` | Stale-reference scan is clean except an intentional runtime-state migration comment. |
| F-003 | IMPL | high | Closed | A | Quantization was a closed Q4/Q8/native enum, which could not honestly represent PQ2_0. | Artifact variants now use validated open identifiers; production quantization remains explicitly limited to Q4/Q8 while exact pre-packed variants such as `pq2_0` are downloaded. | `d8f3fb1` | Dynamic-variant unit test and exact Bonsai registry test pass. |
| F-004 | IMPL | high | Closed | A | Engine selection contained hard-coded mappings for known engines. | Engine descriptors now carry source, revision, installer, launcher, runtime directory, build targets, formats, and hardware support; Bonsai requires the isolated Prism descriptor. | `d8f3fb1` | CPU/CUDA setup plans select the pinned Prism runtime only; mismatched engine/artifact policies are rejected. |
| F-005 | ENV | low | Closed | A1 | The managed local sandbox did not permit the API-auth test's loopback listener to remain active. | Retry the same test with explicit local loopback permission; no product change required. | N/A | Passed outside the network sandbox in 0.24 seconds. |
| F-006 | UX | low | Closed | A1 | Generated YAML placed `schema: 3` after the models because the intermediate document map sorts keys. | Emit top-level profile keys in contract order while retaining deterministic ordering elsewhere. | `d8f3fb1` | Retry begins with `schema: 3`, then `id`, and revalidates successfully. |
| F-007 | BUG | high | Closed | A2 | Auto placement treated Apple unified memory as a dedicated VRAM pool when deriving setup limits. | Resolve the physical device first; charge Metal/shared accelerators only to RAM and discrete accelerators to RAM plus VRAM. | `d8f3fb1` | Unit coverage passes for Metal, CUDA, CPU, and mixed reservations. |
| F-008 | BUG | high | Closed | A2 | VRAM pressure could evict a CPU-only worker even though doing so releases no VRAM. | Filter eviction candidates to workers that release a constrained pool while retaining TTL, residency, priority, and LRU ordering. | `d8f3fb1` | Scheduling code now excludes zero-relief candidates; real pressure test remains in Phase D. |
| F-009 | BUG | medium | Closed | A2 | Logical accelerator placement did not consistently translate ROCm/XPU hardware into native-engine HIP/SYCL/Vulkan targets or their visibility variables. | Centralize placement resolution and map physical runtime to the selected engine target. | `d8f3fb1` | Unit assertions pass for CUDA, ROCm/HIP, XPU/SYCL, XPU/Vulkan, and Metal. |
| F-010 | EVIDENCE | medium | Open | A2 | Admission currently has one aggregate discrete-VRAM pool even when hardware reports multiple GPUs. | Track reservations and limits per physical device before claiming multi-GPU hard-limit enforcement. | — | Single-device `accelerator:0` behavior is covered; multi-GPU target evidence is pending. |
| F-011 | ENV | low | Open | A3 | GitHub's homepage contributor display still lists `miguel-flowstate` after the old author email was removed by a history rewrite. | Wait 24 hours from the force-push and recheck; if the UI remains stale, the repository owner must ask GitHub Support to refresh contributor data. | N/A | Main, the only tag, the commits API, and REST contributor list are clean; homepage verification is pending. |
| F-012 | ENV | low | Closed | A4 | The first HTTPS push used a stale macOS credential for `miguel-flowstate` even though commit attribution and the remote owner were correct. | Authenticate `miguelamendez` without removing the other account and use the intended identity through a command-scoped credential helper. | N/A | Push and public SHA/author verification passed; both accounts remain in the keyring. |

## Measurements

| Profile/model | Engine | Processor | Context | Batch | Cold start | Prompt tok/s | Decode tok/s or realtime factor | Peak RAM | Peak VRAM | Result |
| --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| Pending | — | — | — | — | — | — | — | — | — | Not run |

## Final decision

Not yet evaluated. Passing local simulations is insufficient: support is
declared only after a clean GitHub install and real inference on the target.
