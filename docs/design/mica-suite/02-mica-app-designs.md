# Mica applications designed from first principles

Design baseline, 2026-10-17. These are five independent server–client applications, not frontend modes of one inference daemon. Their common structure and dependency choices follow the [generic blueprint](01-generic-app-blueprint.md). Product names below are working names; no existing executable is renamed by this document.

| App | ID | Owns | Depends on |
| --- | --- | --- | --- |
| Mica | `mica` | App discovery, trusted sources, installation, updates, connections and lifecycle management | App package sources and authorized app control services |
| Infer | `infer` | Models, engines, workloads, hardware policy, scheduling and inference | Engine workers and artifact repositories |
| Assistant | `assistant` | Conversations, attachments, tools, agent loop and speech orchestration | Infer SDK |
| Dictation | `dictation` | Recording sessions, transcription workflow, cleanup and clipboard delivery | Infer SDK |
| Search | `search` | Local collections, ingestion, chunks, embeddings and retrieval | Infer SDK and embedded vector storage |

Infer is the proposed one-word name for the AI server. All five apps target macOS, Linux, Windows through WSL2, and Android. Each owns its core, protocol server, SDK, state and appropriate client interfaces. Mica may manage an installation, but closing Mica never prevents another app from communicating directly with Infer.

## Protocol and client choices

Each app implements Connect request/response with a JSON compatibility mode and Connect event streaming with WebSocket fallback. SDKs encapsulate both. Assistant and Dictation can add WebTransport with WebSocket fallback for explicitly supported live audio sessions; ordinary recorded-file transcription remains available. WebRTC is not required merely because audio is present.

Every app has a CLI, TUI, web interface, desktop interface, Android interface and MCP adapter in the target release matrix. Client functions are appropriate to the app: Infer's desktop/web UI is its dashboard; Dictation's UI configures recording and presents transcripts; Search's UI manages sources and results. Linux and Mac release packages contain the selected native interfaces; Android uses Kotlin/Compose over its Rust SDK.

```text
Mica core ── controls installation/lifecycle, never proxies model requests

Assistant clients → Assistant SDK → Assistant server/core ─┐
Dictation clients → Dictation SDK → Dictation server/core ─┼→ Infer SDK
Search clients    → Search SDK    → Search server/core ────┘       |
                                                          Infer server/core
                                                                 |
                                                            Engine workers
```

The servers in the diagram implement their app's selected protocol and fallback. CLI/web thin clients may use the wire contract directly. That does not move application state into those clients.

## Hardware discovery and compatibility

Implement hardware discovery as a shared Rust library, not a sixth app or a required call to the running Mica manager. Mica and Infer independently use it on their own host. Other apps can use the library for local device needs or query an authenticated node through its SDK.

Detected facts include node identity, timestamp, OS/architecture, execution environment (native/WSL/PRoot), CPU, physical memory, devices and memory topology, driver/runtime availability, toolchains and permissions relevant to the operation. Cache durable facts with provenance; refresh volatile memory/headroom and device availability before admission. Missing permission or a failed driver probe is an unknown/degraded capability, not proof that hardware does not exist.

Mica uses its local facts to select installable binaries, interfaces and optional dependencies. Infer uses its local facts to select compatible engine builds and artifacts, filter workloads and validate requests. A phone connected to a Linux GPU server must not filter that server's models using the phone's GPU.

Keep user policy separate from detection: allowed devices, RAM/unified-memory and per-device VRAM ceilings, build limits and administrative permissions are not hardware facts. Infer is authoritative for its own scheduling/admission. A client-supplied hardware profile cannot raise those limits.

Engine compatibility starts with supported OS/architecture, runtime and engine features. A model is usable if at least one selected artifact/component set has a compatible engine and sufficient resource allowance. A workload is usable if its required models/operations have a valid plan under Infer's policy. Return installed, available, unsupported, unknown and unprepared status with reasons. Default lists filter usable entries; Show all reveals the rest. Hardware-specific engine/model/workload constraints are explicit, and compatibility is derived across them rather than copied inconsistently into every manifest.

## Installation and inference connections

After installing Assistant, Dictation or Search, always configure a connection to an Infer app. This is the same dependency whether Infer is local or remote; consumer apps never load engines directly.

1. Discover a local Infer installation and live node using package receipts, protected local discovery endpoints and explicit health/capability checks.
2. Offer to reuse the discovered node, install Infer locally with approval, or configure a remote Infer node. Do not install models silently.
3. Validate node identity, credentials, protocol version and the selected workload's required/optional operations and media capabilities.
4. Save a named connection and workload binding. Reuse it on later launches; prompt again when no valid binding exists or the user changes it.

Store secrets by protected reference, not in app manifests. Multiple named connections are allowed. Choose one connection explicitly for a job/session; failover is permitted only when policy and capability checks allow it. Local dependency installation remains separately approved, including Android Termux/PRoot setup. Remote selection downloads no local models or engines.

Mica can discover/install apps from a source containing one manifest or a verified multi-app catalogue, including GitHub/Hugging Face sources. Validate immutable revisions, hashes and publisher trust. Hashes from an untrusted catalogue do not prove authenticity. Platform package installation follows OS rules; Mica cannot silently install Android APKs or bypass recording permissions.

## Infer

Infer's data plane provides chat/generation, embeddings, transcription, speech synthesis and other model-supported operations. Image/audio generation or diarization are advertised only when a model plus engine actually supplies the operation. The core resolves model/artifact/components and engine support; it does not execute an assistant's tools or own its conversation history.

Its control plane manages detected hardware, machine policy, engine/model/workload registries, preparation jobs, worker lifecycle, metrics and credentials. The dashboard is Infer's own client. Workloads group model configurations and priorities. Per-request workload selection does not require changing a global default. A global residency strategy decides what stays loaded, shares compatible workers and waits for in-flight work before eviction.

Memory admission includes weights, selected projector/drafter components, KV configuration, context/output reservation, batching and workspace estimates. Estimates and measured peaks remain distinct. An administrative budget is not automatically an OS hard cap. Exact enforcement and unsupported-device conditions are visible in capabilities and status.

User-authored machine, engine, model and workload manifests remain YAML. Their schemas validate configuration; `.proto` defines callable services and messages. Engines stay isolated external workers. Installations expose hardware-compatible models and workloads by default, with explicit reasons under Show all.

## Assistant

Assistant owns sessions, prompts, attachments, agent/tool permissions, media preparation and export/import. Every UI and MCP operation calls the same core. It uses Infer's SDK for chat, visual analysis, ASR and TTS according to the bound workload.

Text becomes the user instruction. A voice note alone becomes the instruction after transcription; with accompanying text, it becomes additional context. Files are uploaded or ingested into Assistant-owned storage, prepared as needed and passed to an appropriate inference operation. Local paths are never interpreted on another device without an explicit upload or authorized host operation.

Tools run where Assistant has permissions, not automatically on Infer's host. A model can request a tool, but Assistant authorizes and executes it. The inference server can discard caches without losing a conversation. Stream text and optional speech through one session event contract, retain a final assembled voice reply and allow cancellation/import/export.

## Dictation

Dictation owns microphone capture, recordings, transcript sessions, cleanup preferences, shortcuts and clipboard output on its installed device. Its local server is the recording/workflow authority. Its clients configure or control that service through Dictation's SDK. MCP file transcription is distinct from microphone activation and requires explicit file access.

Opening configuration leaves the app ready, not recording. Explicit activation enables the approved shortcut/background service; an explicit record action captures audio. Stop submits the clip or closes a live stream; cancel releases capture without submitting. Deactivation removes capture capability. Show recording/processing/error state and release the microphone on failure.

Infer performs ASR, optional model-supported diarization and optional LLM cleanup. Dictation aligns and presents results only where supported; do not claim speaker attribution from an ASR-only engine. Preserve the original transcript separately from cleaned text. Auto-paste is opt-in and requires OS permissions; clipboard delivery remains available when typing is unavailable.

## Search

Search is an independent local-first semantic search app, not a database inside Infer. Its own server controls source folders, ingestion jobs, indexing, querying and file opening. CLI, TUI, web, desktop, Android and MCP operate on that same core.

Use embedded SQLite for collection metadata, provenance, jobs and compact vectors initially; no separate vector database daemon. Start with exact cosine/dot-product search over normalized vectors and an indexed metadata filter. An optional SQLite vector extension can replace the scan when corpus measurements justify it. The storage interface keeps backend choice out of clients. This choice favors a small footprint over approximate-search scale. [SQLite deployment guidance](https://www.sqlite.org/whentouse.html)

An index records model/artifact revision, dimensions, normalization, distance metric, query/document task formatting and chunking version. An incompatible embedding configuration requires a new index or explicit rebuild; never mix vector spaces. Query and document formatting follow the selected model's verified card/engine contract. The previously discussed EmbeddingGemma candidate is a candidate, not assumed proof of image/audio/video support. Advertise only tested embedding modalities and model-specific task prefixes.

Users select a root, recursion, file types and exclusions. Canonicalize paths and handle symlinks explicitly so indexing cannot escape authorized roots. Track hashes/mtime and source identity for incremental updates, cancellation and deletion.

| Input | Unit stored | Result locator |
| --- | --- | --- |
| PDF | Page text and/or rendered page image as supported | File, page, optional bounding region |
| Text/document | Bounded semantic chunks with stable offsets; page where present | File, section/page and text offsets |
| Image | Image or supported crops with provenance | File and optional region |
| Video | Timestamped clips/frames, with transcript/caption fallback where needed | File, start/end time and frame identity |
| Audio | Timestamped segments and transcript if available | File, start/end time and speaker only when supported |

Query accepts text and model-supported media, top-N and filters. Results contain score, source file ID, exact locator, snippet/thumbnail reference and embedding provenance. A local app opens its local file; a remote browser uses an authorized served preview/download, not an unresolvable filesystem link. Android uses user-granted document URIs rather than assuming unrestricted desktop paths.

## Ports and hosting

These are Mica suite conventions, not IANA registrations. Retain an existing explicitly configured listener until a deliberate migration. Never kill an unrelated process occupying a preferred port.

| Port/range | New installation convention | Exposure |
| --- | --- | --- |
| 55000 | Infer protocol services, compatibility endpoints and `/dashboard` | Loopback by default; authenticated TLS for remote use |
| 55001 | Mica manager services and web interface | Local by default; remote control only by explicit policy |
| 55002 | Assistant standalone services and web interface | Local by default |
| 55003 | Dictation standalone services and configuration web interface | Local by default; remote recording control disabled by default |
| 55004 | Search standalone services and web interface | Local by default |
| 55005–55199 | Future explicitly assigned app listeners | Unallocated, not prestarted |
| 55200–55399 | Reusable private HTTP app services, 200 simultaneous leases | Loopback only |
| 55400–55899 | Unallocated future use | No automatic spillover |
| 55900–55999 | Reusable private inference workers, at most 100 leases | Loopback only |

An app's data and control services can share a listener with distinct authorization. UDS/in-process deployment needs no TCP port for every component. Web UIs can be reached directly on the owning app or through a managed `/apps/APP_ID/` reverse proxy on Mica. The proxy is an optional hosting convenience; closing Mica must not stop an independent app or remove its direct service endpoint.

Allocate service/worker leases only on launch and return them after the process exits. Installed apps are not limited to 200; concurrently running private HTTP services are. Prefer native/static hosting without another HTTP process when possible. Multi-instance installations select another explicit free listener and record it. SDK connections use discovered/configured endpoints, not hard-coded assumptions about port numbers.

## Build and platform coverage

The Mac builds macOS packages. The Linux host builds Linux packages and Android APK/AAB/native libraries. Release targets include x86_64 and aarch64 where the selected dependencies support them; emulator ABIs are separate from phone ARM64 releases. Every interface has a release target, and Mica selects packages by platform, architecture, capability and user choice rather than omitting unbuildable interfaces globally.

Android Assistant, Dictation, Search and Mica are full local applications, even when they connect to remote Infer. Infer can run locally on Android through a tested Termux/PRoot deployment or a native supported engine integration. PRoot needs host-managed process lifecycle and honest hardware detection; GPU support is separately tested. Hardware access, permissions, Android background policies and WSL integrations are per-platform acceptance requirements.

The accompanying draft `.proto` files define common lifecycle, hardware and connection contracts plus each app's services. [Adaptation plan](03-implementation-adaptation.md) concerns existing code only; it does not constrain this fresh design.
