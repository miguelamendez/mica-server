# Inference application draft

Status: target design, first application to migrate. Working app ID/name: `infer` / Infer. Existing `mica-server` commands remain unchanged until an explicit installation migration.

## Core and server

Infer owns hardware-aware engine setup, model/component downloads, machine policy, workload preparation, request routing and memory-aware model loading/offloading. A workload groups models and configurations; it is not a separate server. Compatible workers can serve requests from multiple workloads without duplicate loading. In-flight requests must finish or be explicitly cancelled before eviction.

Machine, engine, model and workload YAML remain shareable configuration. Model abilities intersect with engine-supported operations; the resulting capabilities drive endpoint discovery and UI availability. Memory planning includes weights, projectors/drafters, KV cache, context, batching and workspace, with estimates distinguished from measured usage.

The core exposes data and control services through ConnectRPC, JSON encoding and server streams with WebSocket fallback. Preserve OpenAI-compatible HTTP/SSE endpoints for existing tools, including Pi. These are adapters to the same core, not an additional inference implementation.

## SDK and interfaces

The Infer SDK owns authentication, typed operations, deadlines, cancellation and stream handling. CLI, TUI, dashboard, desktop GUI, native Android UI and MCP are sibling clients. The server never calls its own CLI for business operations. Its UI shows node hardware, workload/model/engine compatibility, actual residency and operation support.

Default new-install listener: `55000`, including `/dashboard`. Protected local IPC is preferred for native same-device clients. Preserve explicitly configured existing listeners until migration is approved.

## Platforms and installation

Build native macOS and Linux servers/clients; build Android core/server libraries and UI packages with the NDK/Kotlin toolchain. Also provide terminal clients and a Linux ARM64 runtime path through Termux/PRoot where needed. Android is a local server target, not merely a remote dashboard. See [runtime choices](../04-android-runtime-options.md).

Mica may install Infer and its selected interfaces, but Infer must also be independently installable. Install engines/models only after selecting and approving a workload. Hardware detection runs on Infer's own device using the shared discovery library; remote clients cannot raise its policy limits. Unsupported engines stay unavailable on Android rather than being assumed portable.

## Acceptance gates

1. Preserve existing model discovery, tool calls, thinking controls, workload routing and OpenAI streaming behavior.
2. Exercise the same core operation from CLI, TUI, browser, desktop, Android and MCP.
3. Verify request draining, compatible worker reuse, memory admission and independent workload requests.
4. Run a small real GGUF inference locally on Android; certify GPU acceleration separately by device/engine.
5. Test install/update/rollback without losing credentials, workloads or caches.

Draft services: [Infer contracts](../../../../proto/mica/infer/v1/infer.proto). Shared rules: [application blueprint](../01-generic-app-blueprint.md).
