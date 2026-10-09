# Mica terminal interface

Run `mica-server tui` after installing the binary **and its matching configuration
files**. The interface is native C++/FTXUI with Lua presentation configuration;
it does not install or invoke a Python UI framework. The server can be stopped
while you browse. `mica-server tui --snapshot` emits a read-only diagnostic JSON
inventory, including incompatible entries and their compatibility flags, without
printing the API key. Interactive lists apply the compatibility filter.

## Navigation

The three panes separate navigation, selectable entries, and details.
Below 120 columns, navigation moves above the list/details panes.

| Control | Action |
| --- | --- |
| `1`–`6` | Server, Workloads, Models, Engines, Endpoints, Settings |
| `Tab`, arrows | Change navigation/list focus and select entries |
| `Enter` | Explore a relationship or the selected action |
| `Esc` | Return to the previous workload/model/engine view or close a dialog |
| `g` in Models | Cycle All, LLM, VLM, ASR, TTS, Embeddings, Diarization and future generation categories |
| `u` | Show all hardware targets / restore compatible-only lists |
| `/`, `r` | Filter names / refresh the inventory |
| `PgUp`, `PgDn` | Scroll details |
| `?`, `q` | Help / close the TUI; closing does not stop Mica |

`Workloads → Enter → Models → Enter → Engines` lets you inspect the exact
collection and its supported runtimes. Browsing an engine is read-only: it does
not change a workload pin. Use the workload YAML editor to change engine,
artifact, context or residency selections. A model's supported engines can
include hardware-incompatible alternatives; press `u` to inspect those.

## Server and Machine

The header shows the active workload, or the default when stopped. Server's
Overview shows status, loaded model **names**, listening address, planning
reservations and stored model/runtime/environment sizes. Disk sizes are logical
regular-file sizes, exclude symlinks, and are not RAM/VRAM measurements.

Select **Choose workload**, then Enter. When stopped, Enter on a workload reviews
a start command; when running, it reviews a hot-swap command. `s` opens the same
picker; `x` reviews stopping. A hot-swap requires installed dependencies and
cached models: first install missing resources from Workloads. Matching workers
are retained; incompatible workers drain/unload. Actions are confirmed before
execution. Readiness is refreshed every two seconds; `r` refreshes disk/cache
inventory too.

**Machine** is a Server subsection. It shows detected OS, CPU, accelerators,
physical/unified memory, runtime targets and user allocation policy. Settings
edits allocations; it never changes detected hardware facts.

## Curated workloads and inventory

Normal workload lists honor `catalog_visible`. Hidden legacy examples remain
available by explicit CLI ID for reproduction but are not revealed by the
hardware “show all” toggle. The active/default workload and user-installed YAML
definitions remain accessible even when hidden in the packaged catalog.

Inventory distinguishes:

| Marker/state | Meaning |
| --- | --- |
| `✓` | Engine executable/module installed; model bundle cached; or all workload engines and bundles present, depending on view |
| `↓` | Missing installation or complete cached bundle |
| `!` | No compatible engine/hardware target, or a workload's fixed device is absent |
| `$` | Workload requirements/strategy exceed allocation or machine policy disables a selected device |

An engine is compatible if its declared hardware targets match detected facts
and its engine family supports the OS. A model is compatible when at least one
supported artifact can use a compatible engine. Workloads use their resolved
engine/artifact and fixed placement requirements. Hardware support is derived
through these relationships, not copied into independent hardware allowlists
on model and workload manifests.

Installation checks the native executable or the Python interpreter **and the
engine module**. A shared environment alone does not mean every MLX engine is
installed. Cache checks use the same directory layout as serving, all declared
files and the completion marker's repository/revision. They do not rehash large
files on every UI refresh. These checks are not live inference certification,
driver/toolchain readiness, or proof that a workload fits its measured peak.

## Model, engine and endpoint information

Model details own modalities, abilities, training/context guidance, quantizations,
references, supported engine IDs and artifact components such as projectors,
MTP/DFlash drafters or codecs. Workload details own runtime policy: selected
artifact/engine, input/output/total context, KV precision, priority and residency.
Engine details own install/build recipes, targets and endpoint contracts.

Endpoints are drawn from Mica's route-discovery catalog with descriptions,
authentication requirements and example curl calls. Set `MICA_BASE_URL` and
`MICA_API_KEY` privately, and substitute `MODEL_ID`, `WORKLOAD_ID` or session
parameters. A registered route still requires a suitable active model; engine
endpoint contracts describe worker APIs, not additional public Mica routes.

## Editing and settings

In Workloads, `i` installs engines/models, `s` starts, `a` activates, `e` opens
the YAML editor, and `c` clones into user-owned configuration. Edited definitions
are validated; restart to load edits into the running registry. TUI jobs run off
the rendering thread, report concise results, and refresh inventory on completion.

Settings supports bind address, port, default workload, RAM, dedicated GPU memory,
private key-file import and rotation. Stop first; limits and key changes apply
on the next start. Key contents are never displayed. Use a trusted LAN/firewall
for `0.0.0.0`; Mica's HTTP service does not provide TLS.

See [installation](getting-started.md), [workload design](profiles.md),
[memory estimation](memory-estimation.md), [configuration schemas](configuration-schemas.md)
and [API reference](api.md).
