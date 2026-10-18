# Mica application suite drafts

Draft architecture, updated 2026-10-18. Mica is becoming a monorepo of independently installable applications. These documents describe the target; the current C++/Lua applications have not yet been migrated to the Rust blueprint.

The implementation order is **Inference first, Mica manager second**. Assistant, Dictation and Search follow. Every app includes a core, a server implementing its selected protocol and fallback, an SDK and thin clients. One repository does not mean one binary, one process or an installation of every app.

## Design documents

| Document | Scope |
| --- | --- |
| [Generic application blueprint](01-generic-app-blueprint.md) | Shared architecture, protocols, dependencies, security and lifecycle |
| [Application suite](02-mica-app-designs.md) | Relationships, hardware compatibility, connections and port allocations |
| [Implementation adaptation](03-implementation-adaptation.md) | Migration of the working implementation and acceptance gates |
| [Android runtime options](04-android-runtime-options.md) | Native Android, Termux, PRoot, DroidDeck and device validation |

## Per application drafts

| Order | Draft | Responsibility |
| --- | --- | --- |
| 1 | [Inference](apps/infer.md) | Host models and schedule inference across compatible engines |
| 2 | [Mica manager](apps/mica.md) | Discover, install, update, configure and manage applications |
| Later | [Assistant](apps/assistant.md) | Conversations, media, tools and speech workflows |
| Later | [Dictation](apps/dictation.md) | Device recording, transcription and optional text cleanup |
| Later | [Search](apps/search.md) | Local ingestion, embedding storage and semantic retrieval |

Each app's CLI, TUI, desktop UI, browser UI, Android UI and MCP adapter are release targets. macOS and Linux get native servers and desktop clients. Android gets its own local core/server and native UI, plus terminal interfaces through an appropriate terminal environment. The browser interface remains available on every compatible browser. Platform-specific builds and real-device acceptance are distinct requirements.

Mica selects compatible release artifacts using host discovery and app manifests. Users choose apps and interfaces; installing Mica does not silently install inference engines, models or every UI. Installed apps remain usable when the manager is closed.

The versioned [draft protocol contracts](../../../proto/) compile and lint, but current binaries do not yet implement those services. User-authored YAML configuration is separate from RPC messages. The roadmap must track design, implementation, cross-build and hardware certification separately.

## Checkpoint and release policy

Commit `7fa9095` preserves the pre-migration drafts, contracts and QA tooling; `11aa488` contains the working Pi/thinking fixes. Keep existing installations, listeners, credentials and model caches unchanged until an app migration passes its acceptance gates. Build macOS packages on the Mac, and Linux/Android packages on Linux. Publish separate versioned artifacts and checksums for each app, platform and interface.
