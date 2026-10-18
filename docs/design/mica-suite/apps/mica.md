# Mica manager application draft

Status: target design, second application to migrate, after Infer. App ID: `mica`. Mica is the discovery, installation and management application, not the inference proxy.

## Core and server

Mica owns trusted sources, app discovery, package verification, installation receipts, interface selection, named connections and lifecycle management on its own device. Sources can publish one app or a catalogue of apps, including sources on GitHub or Hugging Face.

Each package identifies immutable source/version, OS/CPU/runtime compatibility, interface artifacts, dependencies, digests and publisher verification. Checksums establish artifact integrity; trusted metadata/signatures establish the expected publisher. Downloads are verified before extraction/installation, with safe paths and rollback. Exact manifest fields are to be finalized before implementation.

Mica can collect independently released binaries from this monorepo or other sources. Installation remains selective. Installing Assistant need not install Search, local inference, every UI or development tools.

## SDK and interfaces

CLI, TUI, web, desktop, Android and MCP clients use the manager's own services/SDK. ConnectRPC/JSON covers operations; server streams with WebSocket fallback report bounded job progress. Default new-install listener: `55001`.

A dashboard action installs on the device hosting that manager. Installing on the browsing device requires a local Mica installation or an explicit platform package download; a web page cannot silently install native software. Use app identity/receipts to detect installed apps, not assumptions based on whether a URL contains localhost.

## Consumer setup

1. Select an app and compatible interfaces, then approve its package/dependencies.
2. Discover a local Infer node. Offer reuse, approved local installation, or a configured remote node.
3. Validate credentials, version, workload capabilities and the inference host's hardware compatibility.
4. Save the connection and workload binding, then launch the app independently.

Support multiple saved inference connections and user-selected workloads. Choosing remote inference must not download local models. Closing Mica leaves installed apps running and usable; installing an app or engine does not restart unrelated services.

## Platforms and acceptance

Mica uses shared local hardware discovery to select macOS/Linux packages and Android APK/native/runtime artifacts. Android package approval and storage restrictions remain enforced by Android. All interface families are targets, including CLI/TUI through a terminal environment and a native Android UI/core/server.

Accept the manager when app installation, update, connection setup, opening, stopping and removal work independently; an Assistant can use remote Infer without a local engine; and failed updates restore the previous package/state. Test each device, not just generated release binaries.

Draft services: [manager contracts](../../../../proto/mica/manager/v1/manager.proto). Shared rules: [application blueprint](../01-generic-app-blueprint.md).
