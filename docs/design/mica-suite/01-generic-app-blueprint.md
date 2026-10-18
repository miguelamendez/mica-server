# Generic client server application blueprint

Design baseline, 2026-10-17. This blueprint describes a new application, independently of any existing Mica implementation. Choose the communication patterns an application actually needs. Target broad device compatibility first, then performance and a small installation footprint.

## Architecture and contracts

```text
                     Versioned .proto contracts
                     generate messages and services

CLI   TUI   Desktop   Web   Android   MCP   Other apps
                    |
          Application SDK or thin wire client
                    |
    Server implementing selected protocol and fallback
                    |
                  Core
          business logic and owned state
                    |
       Another application's SDK when required
```

The server is the implementation of the protocol-facing services, not a separate API layer before or after the protocol. The SDK encapsulates the selected protocol, fallback, authentication, deadlines and reconnect behavior. The core does not depend on a UI or transport.

The CLI, TUI, GUI, browser and MCP interfaces are sibling clients. A running server never invokes its own CLI to perform business operations. Executing a declared external worker or invoking an OS service manager is a separate lifecycle operation, not business logic implemented through a CLI.

Define messages and services once in `.proto` packages such as `example.notes.v1`. Keep data services and control services separate: data services perform the app's work; control services manage configuration, permissions, jobs and metrics. Local control is the default. Every exposed remote control method requires explicit authorization.

The core owns storage. Clients neither edit its database nor depend on its internal file layout. User-authored YAML configuration and package manifests have their own versioned schemas; they are not additional competing definitions of RPC messages.

## Protocol selection

| Communication pattern | Main | Fallback | Typical work |
| --- | --- | --- | --- |
| Request and response | ConnectRPC with Protobuf | HTTPS with Connect JSON encoding | CRUD, commands, configuration and service calls |
| Server to client stream | Connect server-streaming | WebSocket | Progress, tokens, logs and notifications |
| Reliable two-way stream | WebTransport streams | WebSocket | Interactive sessions and live audio workflows |
| Loss-tolerant two-way updates | WebTransport datagrams | WebSocket with stale-update policy | Positions, telemetry and live cursors |
| Media and peer-to-peer | WebRTC | WebRTC through TURN, including TCP 443 | Calls, screen sharing and peer transfer |
| Wake a suspended mobile/web app | UnifiedPush on Android; Web Push in browsers | FCM on Android | Wake-up notification only |

Binary Protobuf and ProtoJSON represent the same service contract. Connect JSON is the request/response compatibility encoding, not independently maintained REST business logic. Streams have explicit message framing and terminal errors. WebSocket fallback maps the same logical operations and events, not a second application workflow. [Connect protocol](https://connectrpc.com/docs/protocol/)

The minimum interoperable network implementation is HTTPS/JSON plus WebSocket. If a deployment needs long polling when WebSocket is blocked, use the same bounded event log and cursor semantics. Do not enable every transport just because this blueprint lists it. Push carries a wake-up signal, never the authoritative app state or a secret.

Use Unix sockets for local native clients, equivalent protected IPC where supported, and loopback HTTP for browsers. Remote connections use TLS/QUIC/DTLS as appropriate. TCP/UDP 443 is a conventional deployment edge, not a requirement to bind every local app to privileged port 443. Namespaced RPC methods can share a listener. Multi-app products define their listener policy separately.

## Implementation languages and dependencies

| Component | Development dependencies | Installed runtime |
| --- | --- | --- |
| Core and protocol server | Rust via rustup, Cargo; Tokio, Tower/Axum/Hyper, Connect runtime | Native executable and declared system libraries |
| Protocol generation | Protobuf compiler, Buf, matching message/service generators | Generated bindings, not compiler tools |
| Shared SDK | Rust, same protocol/runtime versions as server | Linked into full clients |
| Lean CLI | Rust, small argument parser, maintained HTTP/system-TLS adapter | Small native executable |
| TUI | Rust and ratatui plus app SDK | Native executable |
| Desktop GUI | Rust, Slint, OS window/font/input development libraries | Native GUI plus declared platform libraries |
| Android | JDK, Android command-line tools, SDK platforms/build-tools, NDK, Gradle wrapper, Kotlin/Compose, UniFFI | APK containing native Rust libraries and Android UI |
| Browser | HTML/CSS/vanilla JS; Rust/Wasm toolchain only for shared computation | Browser assets, no Node/npm requirement |
| MCP | Rust rmcp with stdio and optionally authorized HTTP transport | Thin adapter to the app, not duplicated workflows |
| Linux native and cross-build support | Git, C/C++ compiler, CMake, Ninja, pkg-config, SSL and UI headers | Only declared linked dependencies |
| macOS build support | Xcode command-line tools and SDK, Rust, signing tools for distribution | Signed/notarized packages as appropriate |

Add Quinn/wtransport only for WebTransport use, str0m or a validated WebRTC implementation only for media, and Web Push/FCM dependencies only for wake-up support. Use Kotlin/Compose on Android and Swift/SwiftUI if iOS is later added. Slint is the selected compiled desktop toolkit. Games may instead use a thin rendering framework over a transport-independent shared simulation; the headless server does not depend on graphics.

Pin application dependency lockfiles and generator versions together. Rust Connect is currently a pre-1.0 implementation, so treat its generator/runtime pair as a tested toolchain unit. Do not install Cargo crates globally when they are ordinary application dependencies. [Rust Connect runtime](https://docs.rs/crate/connectrpc/latest)

Install build prerequisites separately from apps. Use the Gradle wrapper pinned by the project rather than a global distribution Gradle. The Linux build host can produce Linux and Android artifacts; the Mac produces macOS artifacts. Android cross-builds do not certify microphone, background lifecycle or model execution on a real phone. [Android command-line tools](https://developer.android.com/tools/sdkmanager)

## SDK and client behavior

An SDK owns connection negotiation, credentials, typed operations, fallback, cancellation, backpressure and replay policy. Full clients use it. Thin CLI/web clients can call the wire contract directly but share the same validation and event rules. A browser stream parser must handle message boundaries and terminal errors correctly.

An app that depends on inference uses the inference app's SDK. Its own clients use its own SDK. Local versus remote changes the connection, not the owning app or workflow. Hardware-native operations such as microphone capture remain on the device that granted permission.

Expose health, readiness, app/schema versions, supported operations, transports, limits and permission requirements. Capabilities describe a particular node. They are not a promise that every deployed instance supports every optional operation.

## Lifecycle and deployment

On desktop/server hosts, run persistent servers under systemd or launchd. Provide foreground serve, service install/start/stop/status and graceful shutdown. Support socket activation where available. Avoid self-detaching servers as the normal supervision mechanism.

On Android, embed the Rust core and server in the app lifecycle and use an authorized foreground service when work must continue visibly. An optional Termux/PRoot deployment can run Linux server builds. It needs host-aware lifecycle and memory discovery, not an assumption that systemd or a desktop kernel is available. [Android foreground services](https://developer.android.com/develop/background-work/services/fgs), [PRoot-Distro limitations](https://github.com/termux/proot-distro#limitations)

Distinguish installed, configured, stopped, starting, ready, busy, draining and failed. Launching one app must not restart unrelated apps. Closing a UI does not implicitly stop an explicitly enabled background service. Microphone activation always requires a distinct authorized action.

## Connections and distributed state

Connect to a service when any compatible healthy instance may serve the request. Connect to a specific node when controlling its machine, device or local session. A fallback server must match requested capabilities and authorization, not just answer health checks.

Classify state as stateless requests, owned sessions or shared data. An inference prompt cache may be disposable; an agent conversation, document or recording session has an owner. Losing a cache can cause recomputation. Losing an owned session requires recovery, explicit accepted loss or a separately designed replication mechanism.

A coordinator, synchronization service or peer-discovery service is another app, not mandatory infrastructure for a single-device app. Coordinators may provide directories while clients connect directly to a selected service. Shared state can use a database, consensus or CRDTs according to its consistency requirements. Do not promise transparent mid-stream failover just because later requests can use another node.

## Security and reliability

- Keep administration credentials separate from ordinary app access; scope credentials to operations and resources. No embedded shared administrator secret in a distributed client.
- Store native credentials in protected files/OS stores; use scoped browser sessions with origin/CSRF protection. Remote control is opt-in and authenticated.
- Validate payloads, file access, tool permissions, sizes and media types on the authoritative server. Remote paths are not local paths.
- Give every call a deadline and propagate cancellation to downstream work. Bound queues, buffers, event retention and concurrent jobs.
- Retry only eligible operations with backoff/jitter. Durable idempotency records must match the scope/lifetime of supported retries.
- Sequence streams that can be replayed. If replay is unavailable or expired, return an explicit resynchronization outcome rather than silently regenerating or executing tools twice.
- Drain on shutdown; release device handles, child workers and leases. Separate liveness from readiness and downstream engine health.

## Observability and compatibility

Structured JSON logs are the default; OpenTelemetry export is optional. Propagate trace IDs across SDK calls. Never log credentials or full private media by default. Expose bounded metrics without opening privileged management to every network client.

Add Protobuf fields without reusing numbers. Reserve removed fields/names and treat ProtoJSON field names as part of the compatibility contract. Breaking changes get a new versioned package. Run Buf compatibility checks in CI. [ProtoJSON compatibility](https://protobuf.dev/programming-guides/json/)

## Build and acceptance requirements

Produce separate artifacts for OS, CPU architecture and interface family. Package metadata tells the installer which artifacts work on the device; users choose compatible interfaces. Every planned UI belongs in the release matrix even when another machine builds it. Measure binary size and idle memory rather than assuming a language guarantees a particular footprint.

Tests cover core operations without a UI, generated schema validation, main/fallback equivalence, authorization, reconnect/cancellation, state recovery, old/new clients, concurrent load and minimum per-platform UI/device acceptance. Linux and Android builds run on Linux; macOS builds run on Mac. Build jobs and memory are derived from available headroom, with explicit upper caps.

See [Mica app designs](02-mica-app-designs.md) for the five apps and [implementation adaptation](03-implementation-adaptation.md) for migration of existing code. The accompanying draft contracts live in `../../../proto/mica/`.
