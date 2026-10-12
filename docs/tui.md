# Mica terminal interface

The optional browser dashboard mirrors these seven sections using buttons,
filters, dialogs and clickable model/engine relationships. Enable it with
`mica-server start --dashboard` and open `/dashboard` on that host. It uses the
same native inventory; no web-framework installation is required. Its admin
key is distinct from inference keys. See [dashboard administration](api.md#dashboard-and-administration).

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
| `1`–`7` | Server, Workloads, Models, Engines, Endpoints, Settings, Apps |
| `Tab`, arrows | Change navigation/list focus and select entries |
| `Enter` | Explore a relationship or the selected action |
| `Esc` | Return to the previous workload/model/engine view or close a dialog |
| `g` in Models | Cycle All, LLM, VLM, ASR, TTS, Embeddings, Diarization and future generation categories |
| `u` | Show all hardware targets / restore compatible-only lists |
| `/`, `r` | Filter IDs, descriptions and model tasks (case-insensitive) / refresh the inventory |
| `PgUp`, `PgDn` | Scroll details |
| `?`, `q` | Help / close the TUI; closing does not stop Mica |

`Workloads → Enter → Models → Enter → Engines` lets you inspect the exact
collection and its supported runtimes. Browsing an engine is read-only: it does
not change a workload pin. Use the workload YAML editor to change engine,
artifact, context or residency selections. A model's supported engines can
include hardware-incompatible alternatives; press `u` to inspect those.

## Apps

Apps lists the bundled **Mica demo apps** source and explicitly registered
repository sources. Select a demo, inspect with Enter, install with `i`, prepare
a bundled workload with `w`, then start/select that workload in Server. `s`
launches trusted app code, `x` stops the app only, and `d` uninstalls it. Sources
can be added with a GitHub/Hugging Face repository or direct HTTPS manifest/index
URL. **Sources → d** removes a custom catalogue without deleting installed apps.

Every install verifies file sizes and SHA-256. Confirmation is not a sandbox:
code runs as your user. Apps never silently swap workloads. See
[Apps](apps.md) for package contracts, dependency requirements, permissions,
offline discovery and recoverable uninstall behavior.

## Server and Machine

The header shows the active workload, or the default when stopped. Server's
Overview shows status, loaded model **names**, listening address, planning
reservations and stored model/runtime/environment sizes. Disk sizes are logical
regular-file sizes, exclude symlinks, and are not RAM/VRAM measurements.

Select **Choose workload**, then Enter. When stopped, Enter on a workload reviews
a start command; when running, it reviews a hot-swap command. `s` opens the same
picker; `x` reviews stopping. A hot-swap requires installed dependencies and
cached models: first install missing resources from Workloads. Matching workers
are retained; residency is reconciled on requests under the global scheduler.
App-bound requests keep their selections. Actions are confirmed before
execution. Readiness is refreshed every two seconds; `r` refreshes disk/cache
inventory too.

**Machine** is a Server subsection. It shows detected OS, CPU, accelerators,
physical/unified memory, runtime targets and user allocation policy. Settings
edits allocations; it never changes detected hardware facts.

**Updates** is another Server subsection: Enter checks the latest published
release. Settings provides **Check for updates**, **Update latest release** and
**Update latest source**. Install actions require confirmation and restart a
running server with its active workload after staging/validation. Checks are
explicit; their cached result/timestamp is displayed in Server and Settings.
Reopen the TUI after installation. See [update channels and build safety](updating.md).
Machine and Server show live available-memory estimates and pressure separately
from static hardware facts and model reservations.

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

## Apps and server connections

Apps → Add source accepts GitHub/Hugging Face repositories containing one
`app.yaml` or an `apps.yaml` index. Each app declares its own bundled workloads,
permissions and optional `cli`/`tui`/`web`/`background`/`mcp` interfaces.
**i** installs on this device; **w** prepares a hardware-compatible bundled
workload separately; **c** adds a named local/remote server connection using a
private inference-key file; **k** selects one; **s/x** start/stop the app without
restarting Mica. Unsupported bundled workloads are not selected as defaults.

`mica-dictation --tui` is the native Dictation app's separate setup/recording
interface. It can use remote inference without a local model server. The web
dashboard offers device downloads instead of pretending it can install on a
browser's machine. See [Apps](apps.md) for MCP and desktop permission limits.

## Model, engine and endpoint information

Model details own modalities, abilities, supported tasks, training/context guidance, quantizations,
references, supported engine IDs and artifact components such as projectors,
MTP/DFlash drafters or codecs. Workload details own runtime policy: selected
artifact/engine, input/output/total context, KV precision, priority and residency.
Engine details own install/build recipes, targets and endpoint contracts.

Each details pane separates information with named sections and horizontal rules:

| View | Sections |
| --- | --- |
| Server | Overview → memory allocation → loaded models → stored data → actions |
| Workloads | Overview/description → memory and residency → availability → individual model blocks → actions |
| Models | Overview/description → capabilities and tasks → context/generation → availability/engines → artifact/component blocks → interactions → provenance → actions |
| Engines | Overview/description → availability → runtime/artifact compatibility → implemented worker operations → installation recipe → installed runtime → actions |
| Endpoints | Overview/description → access/availability → usage example |
| Settings | Server configuration → machine policy → editing/application |

Workload model blocks summarize model descriptions and tasks alongside **that
workload's** engine, artifact, context, KV cache and residency policy. Enter opens
the selectable model list for full model-owned information. `Description` replaces
the old UI label `Purpose`; all three manifest kinds use `description` in YAML.
Task declarations help discovery, but do not change API routing or certify model
quality. See the [task vocabulary and endpoint mapping](schema-vocabulary.md#descriptions-and-task-index).

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

Settings supports bind address, port, default workload, global scheduling strategy,
RAM, dedicated GPU memory, private key-file import and rotation. Default workload
and strategy edits apply live without restarting. Other edits validate the
values, stop Mica, save them and restart the same active workload. Invalid
values leave it running. A failed restart is reported; settings remain saved
for correction. While stopped, settings are saved without starting the server.
Changes cause brief downtime and may interrupt requests. **Rotate API key** also
creates the first key if absent. A newly changed key is shown once in a temporary
dialog: `c` copies using `pbcopy`, `wl-copy` or `xclip` when available; Esc hides
it, and it auto-hides after 60 seconds. Without a desktop clipboard utility,
use terminal selection. Clipboard contents can persist afterward. Existing keys
stay redacted in settings, snapshots and command output; clients must update
credentials after rotation. Use a trusted
LAN/firewall for `0.0.0.0`; Mica's HTTP service does not provide TLS. The CLI
equivalent is `mica-server config set --port 8092 --restart`; see [Updating Mica](updating.md).

See [installation](getting-started.md), [workload design](profiles.md),
[memory estimation](memory-estimation.md), [configuration schemas](configuration-schemas.md)
and [API reference](api.md).
