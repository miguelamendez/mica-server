# Adapting the current implementation to the Mica application suite

Migration plan, updated 2026-10-18. Working runtime baseline: `11aa488`; pre-migration design/contracts checkpoint: `7fa9095`. The [generic blueprint](01-generic-app-blueprint.md) and [five app designs](02-mica-app-designs.md) describe the target independently of current code. Implementation order is Infer first, Mica manager second, then consumer apps. Writing designs and setting up development tools do not rename or restart installed apps.

## Existing implementation and target ownership

| Current component | Target | Work required |
| --- | --- | --- |
| `src/client.cpp`, `client_main.cpp`, `apps/client-ui` | Mica | Replace server-binary/process coupling with shared lifecycle/core calls and app control SDKs; keep verified catalogue concepts |
| `src/server.cpp`, scheduler, setup, inventory, machine, model/engine/workload schemas | Infer | Extract transport-neutral services, then Rust implementation; retain engine processes and declarative manifests |
| `src/hardware.cpp`, build guard | Shared hardware library used by Mica and Infer | Preserve local host provenance, device topology, degraded detection and separate policy; port detectors incrementally |
| `apps/dashboard` | Infer dashboard | Consume Infer control SDK/wire services, not direct filesystem or CLI operations |
| Assistant agent/session routes inside `src/server.cpp`; `apps/mica_playground.py` | Assistant core/server | Move state, archives, tool loop, media and speech workflows out of inference; browser becomes thin client |
| `apps/assistant`, MCP bridge | Assistant package/interfaces | Replace transitional Python bridges after equivalent Rust protocol services pass tests |
| `apps/dictation/native`, dictation adapters | Dictation | Port workflow/connection core; retain native audio/clipboard knowledge behind platform interfaces; unify CLI/TUI/MCP behavior |
| No packaged Search app in current official catalogue | Search | New core, embedded storage and ingestion/retrieval tests; verify embedding candidate and modality support before model selection |
| `src/service.cpp`, detached app launch paths | Per-app lifecycle adapter | Move persistent services to OS supervision; Android/PRoot gets its own lifecycle path |
| `apps/apps.yaml`, app manifests | Suite app packages | Register five apps, platform/interface artifacts, trust/provenance and actual dependency requirements |

Current runtime uses C++/Lua, with Python in optional app/backend workflows. The Rust blueprint implies an incremental implementation migration, not just an executable rename. llama.cpp, MLX, vLLM and other runtime workers remain external engines. Do not rewrite their kernels or require Python for a native GGUF-only installation.

## Contract files

New draft contracts live under `proto/mica/{common,manager,infer,assistant,dictation,search}/v1`. They are design contracts, not claims that the current binaries serve Connect or these methods. Do not publish stable SDK guarantees until conformance and migration tests pass.

Common contracts cover request identity, errors, operations, jobs, cancellation, resources, connections, capabilities, health and hardware. Each app has its own data and control services. Compile descriptors and check field/API compatibility before generating SDKs. Keep existing JSON/YAML configuration schemas separate and map them explicitly to typed service messages.

## Migration order and gates

### Phase 1 Preserve the baseline

Inventory routes, serialized state, secrets, installed app receipts, engine/model caches and per-device configuration. Add independent tests for the current hardware detector, one real inference call, request-bound workloads, app opening, recording lifecycle and conversation export/import. Keep credentials out of fixtures and docs.

Gate: backup/restore procedures and API/state ownership map are complete. No production route or saved state has been removed.

### Phase 2 Establish protocol and shared components

Create a Rust workspace for each app's core/server/SDK and interface crates, with a small shared hardware/lifecycle/auth/transport library. No new language toolchain becomes a production runtime prerequisite. Add pinned protocol generation, Buf checks and a basic health/capability call exercised by CLI and plain browser.

Gate: Linux host build and descriptor validation pass; main/fallback produce equivalent results. The UI does not duplicate business logic or shell out to the CLI.

### Phase 3 Migrate Infer

Put inference control/data behavior behind the Infer core boundary. Preserve OpenAI-compatible HTTP/SSE routes as adapters. Make workload lookup, worker identity, hardware filtering and reservation logic directly testable.

Deliver Infer's CLI, TUI, dashboard, desktop, Android and MCP targets against the same core. Start Android with a native supported CPU engine and an optional Linux-runtime compatibility path; [runtime evaluation and real-device gates](04-android-runtime-options.md) remain separate from cross-compilation. Do not remove the current Assistant integration while its independent app is pending.

Gate: existing clients still complete text/media/tool workflows, thinking on/off and request-bound workloads work, and the new Infer interfaces exercise one core. Validate real inference and installation rollback before promoting a platform package.

### Phase 4 Migrate Mica manager and packaging

Separate Mica's installer/manager from the inference executable. Define independently released app/interface artifacts, digests, compatibility and dependency requirements. Mica can install those artifacts from this monorepo or verified external sources; it does not require all applications to be installed together.

Use shared hardware code locally in Mica and Infer; keep inference memory policy on Infer. Distinguish local package compatibility from remote model/workload compatibility. Add local Infer discovery/reuse, approved installation and remote-node configuration to consumer setup.

Gate: discover/install/open/stop/update apps independently; closing Mica leaves apps usable. Remote-only apps install without local models or an inference binary. Routine package/engine installation does not restart unrelated services. Validate manager clients and Android installation approval on their own platforms.

### Phase 5 Migrate consumer applications

Move conversations, uploaded media, archives, agent tools and speech sequencing into Assistant. Bindings use Infer node/connection and workload IDs. Import existing sessions with a rollback copy; do not automatically delete original data.

Port Dictation workflow logic behind its local protocol server/SDK, keeping device capture/clipboard integrations explicit. Test manual controls before background shortcuts. Add model-supported diarization separately from basic ASR.

Build Search with embedded storage, a small representative ingestion set and exact retrieval before optimizing vector search. Test query/document formatting against the chosen embedding model, source locators, incremental update/delete and no mixing of embedding versions.

Gate: all three consumer apps use local/remote Infer through its SDK. Restarting Infer loses no conversation or search index. Source paths and microphone permissions remain within the app's authorized device scope; Infer never executes consumer filesystem tools.

### Phase 6 Complete interfaces and operating systems

Build Linux and Android releases on Linux; macOS releases on Mac. Deliver CLI, TUI, desktop, web, Android and MCP clients for each app. Android has local cores/servers for all five apps; local Infer through Termux/PRoot is explicitly tested rather than inferred from a cross-build.

Gate: platform support is demonstrated per interface, with real Android recording/lifecycle and local inference tests, WSL device access tests and macOS permissions tests. Mark pending targets honestly until hardware is available.

### Phase 7 Upgrade installations

Release signed/verified platform packages with a schema/version handshake. Migrate explicit listeners and bindings deliberately: do not silently replace the existing Linux `8092` endpoint with `55000`. Preserve shared caches and user memory ceilings. Upgrade one app at a time with readiness checks and rollback.

Gate: existing clients have a compatibility path or a clear update requirement, app state remains intact, and real multi-app requests obey memory/residency rules.

## Development host setup

Set up the Linux build host after these designs/contracts are saved. Use user-owned toolchain directories where possible. This setup does not upgrade Ubuntu, alter NVIDIA drivers, install models, or restart the current inference service.

| Prerequisite | Linux/Android role | Installation policy |
| --- | --- | --- |
| Rust stable/rustup, rustfmt, Clippy | Core/server/SDK, Linux clients | Minimal rustup profile; record exact resolved version |
| Android Rust targets | Native Android SDK/core libraries | ARM64 phone target plus selected emulator/secondary ABIs |
| Protobuf compiler and Buf | Descriptor generation/lint/compatibility | Versioned tools; installed generators match Connect runtime |
| Connect/message generators | Typed SDK/server stubs | Prefer verified prebuilt releases; bounded compilation otherwise |
| JDK 17 or newer compatible with pinned Android plugin | Gradle and Android command-line tools | Side-by-side user-owned JDK; do not change other apps' Java default |
| Android command-line tools, platform/build-tools, NDK | APK/AAB and native libraries | Headless SDK, pinned stable package revisions, explicit license handling |
| CMake, Ninja, pkg-config, compiler and UI/SSL headers | Linux GUI and native dependencies | Reuse installed packages; sudo only for missing system prerequisites |
| Project Gradle wrapper and Kotlin/Compose/UniFFI dependencies | Android UI and Rust bindings | Pinned project build dependencies, not a global desktop IDE |
| Optional emulator and test image | Android integration checks | Separate opt-in download; real hardware still required for device certification |

The initial pre-setup probe reported Java 8 and missing tools on PATH; that is historical, not the current inventory. User-local development tools have since been prepared on Linux. Inspect the existing `mica-dev` environment and verify Android SDK/NDK packages and licenses before installing duplicates or claiming Android build readiness.

Install a separate developer environment file rather than overwriting global shell files, existing toolchains or server settings. Record versions and locations in a setup report. A distro package installation needing sudo is an explicit interactive step; never read a stored password or change drivers as a side effect.

Keep one compiler job by default. Derive available headroom before starting and cap the entire build process tree to at most 8 GiB for initial tests, never over the user's 16 GiB ceiling. Cap Gradle heap/workers as well; do not bypass safeguards because physical RAM is large. [Rust installation](https://rust-lang.org/tools/install/), [Android JDK guidance](https://developer.android.com/build/jdks), [NDK downloads](https://developer.android.com/ndk/downloads)

## Verification and delivery

Validate the complete Protobuf descriptor set, generated contract bindings and links between the three docs. Smoke-test host Rust and an NDK-linked Android native library. If Android licenses, sudo, network access or a platform device blocks a step, record the exact pending action rather than claiming the setup is complete.

Keep the drafts/contracts in the repository and synchronize them to the Linux development checkout. Runtime migration, production restarts and public releases require their own acceptance gates. The documentation checkpoint does not install or migrate any application.
