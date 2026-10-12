<p align="center">
  <img src="apps/mica-readme-header.svg" width="100%" alt="Mica Server — democratizing local AI">
</p>

# Mica Server

**Your models. Your engines. One local server.**

Mica is a configurable local AI hosting server, proxy, and memory-aware model
load balancer. Choose a **workload**—a collection of models for your task—and
Mica resolves compatible engines, prepares dependencies and artifacts, and
loads or swaps models behind one authenticated API.

The core idea is **shareable YAML configuration**, not a fixed model set.
Combine models for chat, coding, vision, speech or retrieval. Keep them resident
when memory permits, load them sequentially, or balance residency by priority.
Switch workloads without deleting caches; compatible workers stay loaded.

> [!WARNING]
> **Alpha: active development, deslopification and ongoing tests.** Integrations
> and APIs may change. Declared compatibility is not inference certification.
> Memory reservations are estimates, not a universal hard RSS/VRAM cap.
> Do not expose Mica directly to an untrusted network.

## Why Mica

| Capability | What it gives you |
| --- | --- |
| **Workload profiles** | Share and switch model collections for assistants, coding, meetings, visual analysis or your own tasks. |
| **Memory-aware serving** | Lazy loading, residency priorities, idle eviction and hot-swaps that retain compatible workers. |
| **Multiple engines** | Hardware-aware installation/build recipes for registered native GGUF, MLX and vLLM integrations. |
| **One API** | Route chat, media, speech, embeddings and other implemented operations to compatible models. |
| **Native TUI** | Inspect hardware, workloads, models, engines and endpoints; configure, start, stop and swap. |
| **Optional apps** | Discover checksum-verified clients with bundled default workloads, then explicitly install and launch them. |
| **Small control plane** | C++/Lua orchestration; Python only for engines or optional clients that need it. |

A model's abilities and its engine's implemented contracts determine available
operations. Adding a model requires a compatible registered engine/adapter;
Mica does not claim universal compatibility with every model or engine.

## Start with the terminal interface

[Install Mica](docs/getting-started.md), then run:

```sh
mica-server tui
```

![Mica native terminal interface: workload details](docs/assets/tui-workload.svg)

*Actual terminal capture from an isolated application home, without inference
or private credentials.*

| View | Main actions |
| --- | --- |
| **Server** | Status, active/default workload, loaded model names, start/stop/swap; machine information and updates. |
| **Workloads** | Browse, install, start, edit/clone YAML, and navigate to the collection's models. |
| **Models** | Categories, tasks, modalities, context, artifacts, projectors/drafters, and supported engines. |
| **Engines** | Hardware support, installation, revisions, build recipes and endpoint contracts. |
| **Endpoints** | Registered routes, descriptions, authentication and example calls. |
| **Settings** | Address, port, default workload, RAM/VRAM, API-key rotation and release/source updates. |
| **Apps** | Demo catalogue and custom sources; inspect, verify/install, prepare workloads, launch/stop. |

**1–7** select views; **Tab** changes focus; **↑/↓** select; **Enter** explores;
**Esc** returns. **g** cycles model categories; **u** reveals incompatible entries.
Actions require confirmation. Closing the TUI leaves the server running.
[TUI walkthrough](docs/tui.md).

## Configuration is the architecture

```text
Hardware discovery ──► machine policy: devices and global allocations
                               │
Engine YAMLs ────────► compatible install/build/launch recipes
                               │
Model YAMLs ─────────► modalities, abilities, context + artifact bundles
                               │
Workload YAMLs ──────► model collection, budgets, placement, KV and residency
                               │
                               ▼
                         Mica server
                    admit · load · route · swap
                               │
                  ┌────────────┴────────────┐
                  ▼                         ▼
             Unified API              Native TUI
                  │
                  ▼
           Optional client apps
           + bundled workloads
```

Models declare supported engines and artifacts. A workload may pin a choice
or let Mica prefer an installed compatible engine, then the model's declared
order. Projectors, codecs and drafters belong to artifact bundles. Context,
placement and residency are workload policies. Workload requirements and limits
must fit the global machine allocation.

Use **all**, **sequential**, or **balanced** residency where the selected models
and budget permit. Component/KV estimates support planning; they are not
measurements of peak process memory.
[Configuration contract](docs/design/configuration-contract.md) ·
[Workload design](docs/profiles.md) · [Memory planning](docs/memory-estimation.md).

## Useful commands

```sh
mica-server workload list
mica-server workload install WORKLOAD_ID --ram-gib 16
mica-server start --workload WORKLOAD_ID
mica-server status
mica-server workload activate OTHER_PREPARED_WORKLOAD_ID
mica-server endpoints
mica-server stop
```

Use IDs from the workload list. Setup installs missing dependencies and models;
hot-swapping requires a prepared workload. Localhost is the default; use
`--host 0.0.0.0` only for a trusted LAN, with authentication configured.

```sh
mica-server update --check
mica-server update --restart
mica-server update --channel source --restart
```

Updates prefer a compatible release binary; otherwise they compile under
live-memory safeguards. Binary/configuration updates preserve application
settings and model caches. API-key rotation shows the new key in a temporary
copy dialog while existing keys remain redacted.
[Updating and older-binary bootstrap](docs/updating.md).

## Apps that use your workloads

The default **Mica demo apps** source includes:

- **Local Assistant:** the existing browser chat, with streaming, attachments,
  optional voice, workload selection and conversation import/export.
- **Dictation:** an open-source, WhisperFlow-style demo: record/upload → ASR →
  optional LLM cleanup → editable text → explicit copy. No global desktop hooks.

Each app has its own folder, `app.yaml`, verified assets and default workloads.
Apps reuse Mica's API and shared caches; they never silently switch workloads.
Browse **TUI → Apps**, or:

```sh
mica-server app list
mica-server app show mica-dictation
mica-server app install mica-dictation --accept-code
```

Add GitHub/Hugging Face repositories or direct HTTPS manifests/indexes through
**Add source**. Every package file has a verified SHA-256. Installation,
workload setup and code execution are separate actions. Downloaded code is
**not sandboxed**—only launch publishers you trust. The optional browser demos
use Python's standard library, without a pip environment.
[App setup, packaging and lifecycle](docs/apps.md).

## Footprint and documentation

C++20 and embedded Lua implement the CLI, TUI, discovery and scheduling.
Native GGUF serving does not require Python; MLX/vLLM and some optional clients
do. Backend environments and models are separate from the core binary.
Application state defaults to `~/.mica` (`MICA_HOME` or `--root` overrides it).

- [Install and run](docs/getting-started.md), including Linux GPU setup
- [TUI](docs/tui.md) · [Apps](docs/apps.md) · [Updating](docs/updating.md)
- [Workloads](docs/profiles.md) · [Engines and artifacts](docs/engines-and-profiles.md)
- [API reference](docs/api.md) · [Schema vocabulary](docs/schema-vocabulary.md)
- [Configuration schemas](docs/configuration-schemas.md) · [Models](docs/models.md)
- [Memory estimates](docs/memory-estimation.md) · [Measured profiling](docs/model-memory-profiling.md)
- [Validation evidence](docs/validation/) · [Development](docs/development.md)

## Roadmap

- [ ] Native macOS/Linux desktop app for Mica management.
- [ ] Desktop dictation: global hotkeys, explicit typing and OS permissions.
- [ ] Signed app publishers, app upgrades and trusted distribution recipes.
- [ ] Image-generation and music/audio-generation engine integrations.
- [ ] Complete vLLM certification on native CUDA/ROCm/XPU hardware.
- [ ] Expand hardware-specific inference, concurrency and memory testing.

## License

[GPL-3.0](LICENSE). Model artifacts and third-party apps retain their own
licenses; consult their model cards and manifests before redistribution.
