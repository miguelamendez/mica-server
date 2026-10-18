# Mica application normalization plan

Status: Planned. Application migration has not started.
Date: 2026-10-17

Normalize four independent applications using the Client–Server App Blueprint: Mica, the inference application, Assistant and Dictation. Each application has its own core, server implementing its selected protocol and fallback, SDK and thin clients. Mica installs and manages applications; consumers connect directly to inference and do not depend on the manager remaining open. All four applications target Android as well as macOS, Linux and Windows through WSL2.

Mica Inference is the proposed name for the inference application. Existing executable names remain unchanged until the naming and migration step.

## Application boundaries

| Application | Core and owned state | Application clients | Outbound dependency |
| --- | --- | --- | --- |
| Mica | Catalogues, package verification, installation receipts, app lifecycle, permissions and connection setup | CLI, TUI, desktop, web, Android, MCP | Package sources and applications' authorized management APIs |
| Mica Inference | Hardware discovery, machine policy, engines, artifacts, workloads, scheduling, request admission and worker management | CLI, TUI, desktop, web dashboard, Android, MCP | Inference engines and model sources |
| Assistant | Conversations, attachments, prompts, agent loop, tool permissions, import/export and speech orchestration | CLI, TUI, desktop, web, Android, MCP | Inference SDK |
| Dictation | Recording sessions, device permissions, shortcuts, transcription workflow, optional cleanup and clipboard delivery | CLI, TUI, desktop controls, web, Android, MCP | Inference SDK |

The inference application returns model outputs, including requested tool calls, without executing consumer tools or owning Assistant conversations. Its operational state and disposable prompt caches remain server-owned. Apps may use example workloads or compatible user workloads without changing the global server default.

```text
App clients
  CLI / TUI / desktop / web / Android / MCP
                    |
       App SDK or thin protocol adapter
                    |
      Server implementing the selected
             protocol and fallback
                    |
                  Core
                    |
       Another application's SDK if needed
```

## Platform and interface targets

The target platforms for all four complete applications are macOS, Linux, Windows through WSL2, and Android. Android support includes each application's core and server, not just a frontend controlling a server elsewhere. Browser clients can run on any compatible device and connect to an explicitly selected server. Native Windows services and iOS applications are outside this milestone.

| Target | Server and core | Client interfaces | Device integration |
| --- | --- | --- | --- |
| macOS | Rust core and supervised local service | CLI, TUI, Slint desktop, browser, MCP | Microphone, clipboard, shortcuts and permission prompts |
| Linux | Rust core and supervised local service | CLI, TUI, Slint desktop, browser, MCP | Explicit X11/Wayland and headless capability detection |
| Windows through WSL2 | Linux core and service in WSL2 | Linux CLI/TUI/MCP, Windows browser, Linux desktop through WSLg where available | Microphone, clipboard, shortcuts and GPU access validated separately |
| Android native | Complete Mica, Assistant and Dictation cores and local protocol servers in Android application lifecycles; native inference where an engine is supported | Kotlin/Compose UI through the Rust SDK, browser and automation adapters | Device-local state, permission-controlled capture and visible foreground service when required |
| Android through Termux and PRoot | Linux inference server and compatible engines in a PRoot userland; Linux builds of other app servers can also be tested here | CLI, TUI, MCP and browser; native Android clients connect through the same protocol | Detect the actual Android host and PRoot limitations; do not assume desktop service managers or GPU access |

All client families are in scope across the product suite: CLI, TUI, desktop GUI, web, Android and MCP. Android CLI/TUI hosting through a terminal environment is a separate compatibility test, not a prerequisite for its native application. MCP is an automation interface, not a mobile user interface.

Assistant and Dictation are full Android applications. Their own core, server, SDK, local state and device integration run on Android even when model inference runs remotely. Mica is also a complete Android application with platform-appropriate installation and management adapters, not merely a remote dashboard.

Local inference on Android is an explicit implementation and testing target through Termux/PRoot, alongside native engine integrations where supported. A phone can run the inference server locally, consume a remote inference server, or expose its local server to an authorized client. Start validation with a small CPU-capable model and test acceleration separately. Publish architecture, OS-version, engine and feature support from test results rather than treating all Linux-like environments as interchangeable.

PRoot-Distro can run Linux userland and individual long-running server processes without root. It does not supply a separate kernel or a normal systemd service environment. The Android/Termux host must own process lifecycle; detection and memory safeguards must account for restricted or substituted system statistics. [PRoot-Distro documentation](https://github.com/termux/proot-distro#limitations)

Android background work must follow foreground-service restrictions and permission rules; a desktop daemon cannot be copied unchanged onto Android. [Android foreground services](https://developer.android.com/develop/background-work/services/fgs)

WSLg provides Linux GUI application integration, not a complete Linux desktop or guaranteed global shortcut support. [Microsoft WSL GUI documentation](https://learn.microsoft.com/en-us/windows/wsl/tutorials/gui-apps)

## Protocols and SDKs

Define services and messages in versioned `.proto` packages. Each application exposes its own data and control services. ConnectRPC binary and JSON use the same service definitions; thin clients may use the wire protocol directly. Full clients share that application's Rust SDK. Android consumes the SDK through UniFFI.

| Pattern needed by an app | Main | Fallback |
| --- | --- | --- |
| Request and response | ConnectRPC | HTTPS with Connect JSON encoding |
| Progress, logs, tokens and application events | Connect server-streaming | WebSocket with the same logical event contract |
| Full-duplex live sessions when implemented | WebTransport streams | WebSocket |
| Loss-tolerant telemetry when implemented | WebTransport datagrams | WebSocket with an explicit stale-event policy |
| Media/P2P when implemented | WebRTC | WebRTC through TURN |

Select only patterns the application needs. Mica management primarily needs request/response and progress events. Assistant needs those plus streamed answers and optional live speech. Dictation needs recording controls, transcription events and optional full-duplex audio. Inference exposes the patterns supported by its operations. A normal file transcription does not require a live-media protocol.

Keep the existing OpenAI-compatible HTTP/SSE inference API as an adapter to the same inference core so external clients remain usable. MCP adapters call their own application's service/core; they do not introduce a second implementation of its workflows.

Local native connections use Unix sockets or a supported platform IPC transport; browsers use a loopback HTTP endpoint. Remote connections use authenticated TLS. Connect stream framing, timeouts and end-of-stream errors must be handled consistently by SDK and thin web adapters. [Connect protocol](https://connectrpc.com/docs/protocol/)

## Phase 1 Define contracts and baseline tests

- Inventory existing API routes, app features, storage, subprocess calls, credentials and tests.
- Define application IDs, proposed product/executable names, API packages and the capability vocabulary.
- Define versioned data/control services, errors, cancellation, deadlines, job events and capability discovery.
- Assign ownership of every existing configuration and state file before moving it.
- Record baseline Mac/Linux behavior and existing inference compatibility tests.

Completion: every existing feature has an owner and a migration test; four app boundaries and all interface/platform targets are documented.

## Phase 2 Build the shared foundation

- Establish Rust core/server/SDK crates and thin CLI, TUI, MCP, web, desktop and Android adapter boundaries for each app.
- Share transport, authentication, event and lifecycle primitives without merging app-specific state or responsibilities.
- Generate protocol bindings and add automatic schema compatibility checks. Use Kotlin/Compose plus UniFFI for Android and Slint for desktop.
- Define per-app config/data/cache/secrets locations and import existing installations without deleting models, conversations or connection settings.
- Extend package manifests with platform artifacts, integrity/provenance, interfaces, API requirements, permissions, service lifecycle and health/readiness declarations.

Completion: each app can answer a health/capability request through its SDK and thin clients; a fallback test reaches the same core operation.

## Phase 3 Normalize the inference application

- Keep hardware discovery, machine limits, engine/model/workload YAML and scheduling in the inference core.
- Expose management APIs to its own dashboard, CLI/TUI and MCP rather than performing management through a CLI subprocess.
- Preserve existing engine workers as external processes; keep Python only where an engine or transitional adapter requires it.
- Add lifecycle supervision, graceful drain, request cancellation and worker cleanup. OS supervision of the app does not prohibit launching engine workers.
- Add an Android Termux/PRoot deployment path: matching-architecture server build, compatible engine setup, host-aware detection and memory limits, and process supervision without assuming systemd. Validate CPU inference before claiming accelerator support.
- Retain OpenAI-compatible routes alongside the new contract; independently test inference and administrator credentials.

Completion: load/swap/unload and real inference work on Mac/Linux and an Android PRoot test target, in-flight requests survive unrelated app operations, and memory admission remains effective. Android certification requires an actual device run, not just a successful cross-build.

## Phase 4 Extract Assistant and normalize Dictation

- Move Assistant conversations, media archives, agent/tool loop, import/export and speech workflow out of inference into Assistant core.
- Route Assistant clients through Assistant API/SDK; let Assistant core call the inference SDK.
- Consolidate Dictation state and recording workflow behind Dictation API/SDK; keep platform capture, permissions, shortcuts and clipboard in device adapters.
- Distinguish stopped, ready, activated, recording, processing and error states. Opening a configuration screen never automatically enables the microphone.
- Support explicit background activation, stop/cancel, disconnect and reconnection without silently repeating tool actions or transcription submissions.
- Keep each app usable with local or remote inference and when the Mica manager is closed.
- Deliver complete Android Assistant and Dictation instances with their own local protocol servers, SDKs, state and native clients; remote inference replaces only the model execution location.

Completion: both apps pass headless workflow tests and expose the same operations through their UI and automation interfaces. Old sessions can be imported and speech permissions are respected.

## Phase 5 Normalize the Mica manager

- Rename/repackage the current client manager as Mica after separating its dependencies on the inference executable.
- Discover/install/update/bind/start/stop each app, including inference, through package and lifecycle contracts.
- Distinguish the device receiving an app installation from the machine providing inference.
- Detect local inference, reuse it if selected, or offer separately approved installation; selecting remote inference installs no local engines/models.
- Keep example workloads as app resources. Prepare a selected compatible workload through inference control API without changing its default or restarting it.
- Support single-app and multi-app sources and verified platform-specific upgrades with rollback.
- Implement Android package/environment management adapters with explicit OS installation approval, including the optional Termux/PRoot inference path.

Completion: installing Assistant or Dictation can select remote inference on a device without the inference executable; the app remains usable after Mica closes.

## Phase 6 Deliver every client family

- Implement the complete CLI and machine-readable output, then TUI and browser clients against stable APIs.
- Implement desktop clients and Android Kotlin/Compose clients using the shared SDKs.
- Expose permission-scoped MCP operations for each application.
- Keep web clients plain HTML/CSS/JavaScript; add Wasm only for a measured computation need.
- Display unavailable capabilities explicitly rather than pretending a headless host, browser, WSL environment or Android device has desktop recording/shortcut privileges.

Completion: equivalent operations succeed across client interfaces, with platform-specific input/permission controls tested on their target OS.

## Phase 7 Validate and release

- Test macOS and Linux locally/remotely, Windows browser to WSL2 service, and all four complete applications on Android. Test Android Assistant/Dictation with remote inference and with a local Android PRoot inference server. Use real Android hardware for inference, microphone, permissions and lifecycle acceptance.
- Test protocol fallback, cancellation, disconnect/reconnect, auth rotation, version mismatch and app/service restarts.
- Test concurrent Assistant/Dictation requests against different workloads, compatible worker reuse and memory-bound eviction after requests finish.
- Test installation/upgrade/uninstall and recovery without unrelated app/server restarts or loss of state.
- Measure binary/download sizes, startup/idle memory and latency per target; cap compilation from available hardware headroom, never beyond the existing 16 GiB ceiling.
- Update README, per-app help, SDK examples, manifests, platform support matrix and release packages together.

Completion: publish tested platforms and known limitations with reproducible acceptance results; do not label untested interfaces or engines as certified.

## First implementation slice

Begin with the four application contracts, the API/state ownership map and a Rust SDK/server skeleton for one inference management operation. Validate it from a CLI and plain web client before migrating existing routes. Keep running Mac/Linux installations unchanged until their replacement slice passes tests and configuration/state migration is verified.
