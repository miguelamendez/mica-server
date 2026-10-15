# Apps: clients packaged with default workloads

Apps are optional programs that call Mica's API. Workloads still own the model
collection, context, placement and residency policy; engines own inference.
Installing an app never silently changes the active workload or starts code.

## Server versus device clients

Use **`mica-client`** to discover/install apps on your device and select local,
remote, or later inference setup. Its CLI, TUI and HTML manager share the same
catalogue and package verifier. See [client setup](client.md).

The **server dashboard lists installed host apps only** and points to Mica
Client for more apps. `mica-server app` remains the explicit host-management
interface described below; when run over SSH it installs on the SSH host, not
the computer displaying your terminal. No browser can silently install a native
app on the browsing device.

Web apps share their hosting listener under `/apps/APP_ID/`. Private upstreams
use the 200-slot app pool; inference uses a separate 100-slot worker pool. Native
apps need no HTTP port unless they declare a service. See [port pools](ports.md).

## Manage installed apps on a server

Open `mica-server tui`, press **7** for **Apps**, and select an installed app.
**Enter** inspects; **w** prepares a compatible bundled workload; **o** opens a
browser UI or its declared foreground TUI; **s** starts a background instance;
**x** closes the app; **d** unregisters it. Apps do not have to replace the
server default workload. For discovery and installation, run `mica-client tui`
on the intended device, or explicitly use `mica-server app install` on the host.

The dashboard opens web apps under `/apps/APP_ID/` on the server's own address.
Native Dictation belongs on the device with the microphone. An SSH session on
headless Linux does not give your Mac a recording shortcut. A device's Mica
Client manager hosts its own web apps on port 55001 and can use remote inference.

The official `mica-examples` catalogue is bundled offline. Listing or installing
an app does not start a model download, inference server or app process.

The assistant packages browser chat. Dictation is a native C++ client with a
small FTXUI setup interface, local/remote server connections, microphone capture,
ASR, optional LLM cleanup and clipboard delivery. Ctrl+Alt/Option+R toggles
recording (maximum two minutes). Automatic paste is opt-in on macOS and requires
Accessibility permission; if focus changed since recording started, it copies
instead of pasting into the wrong app. Global shortcuts require Input Monitoring
on macOS or an X11 desktop on Linux. Wayland global shortcuts/automatic typing,
speaker alignment and signed desktop installers remain unimplemented. Linux
recording uses installed PipeWire `pw-record` or ALSA `arecord`; clipboard uses
`wl-copy`/`xclip`. The TUI Record button works independently of global shortcuts.
macOS uses native audio/clipboard APIs (no FFmpeg recording dependency).

Both demos have stdio MCP entry points using Python 3's standard library,
**no MCP SDK/pip environment**. MCP tools never start recording or type into
other applications; file uploads require explicit allowed directories.

## App location and inference location are separate

The server dashboard manages installed apps on its host and links to Mica
Client. Use `mica-client tui` or `mica-client ui --open` to discover/install on
your device, then choose local, remote or later inference. Optional local server
setup is separately confirmed. A browser cannot silently install software or
reliably detect native apps on its device.

```sh
mica-client install mica-assistant --accept-code --inference later
mica-client connect linux --url http://HOST:55000 --workload WORKLOAD_ID \
  --api-key-file /path/to/private-inference-key
mica-client bind mica-assistant --connection linux
mica-client ui --open
```

Client connections live in `~/.mica/client/config/connections.json`; per-app
bindings in `~/.mica/client/config/app-connections/`. The server has a separate
app registry and bindings under `~/.mica`. Installed apps and model caches are
independent. See [Client](client.md) for local detection and installation.

### Older portable exports

Previously exported standalone packages remain a separate lightweight option.
If you already have such an archive, extract it and run:

```sh
python3 client.py install
python3 client.py connect --connection linux --url http://HOST:55000 --workload WORKLOAD_ID
# Enter the inference key privately; never use the admin key.
python3 client.py start --connection linux
# Dictation setup interface:
python3 client.py start --connection linux --tui
# Optional local inference, never performed by install/connect:
python3 client.py setup-local
```

Those older packages keep per-app connections in `~/.mica/config/app-connections/APP_ID.json`,
with private key-file references. Choose another connection without reinstalling.
A device-only Dictation install can build its small native client with CMake/C++
and a capped single-job build; it does not build Mica or download models for
remote inference. Local Mica source setup additionally needs Lua development
headers and asks before preparing engines/models.

```sh
mica-server app connect mica-assistant --connection linux --url http://HOST:55000 \
  --api-key-file /private/path/inference-key --workload WORKLOAD_ID
mica-server app start mica-assistant --connection linux --accept-code
mica-server app connection-select mica-assistant --connection linux
```

For explicit server-host app management, TUI Apps: **c** adds a connection,
**k** chooses one, **s** starts. **w** prepares
local inference separately. Bundled workload selection follows engine/device
compatibility, not file ordering; Linux must not select Mac-only workloads.
With a running dashboard-enabled server, **w**, `app setup`, and `workload install`
use the same serialized preparation job. Inference drains and pauses during
preparation, then resumes; the server listener, open apps, and default workload
stay unchanged. Without the dashboard, prepare while stopped. Setup-only machine
overrides are rejected during live preparation rather than silently ignored.
`app upgrade ID --accept-code` replaces verified files after closing the app,
retains previous files, and preserves workload edits/connections. Updating Mica
does not automatically replace already installed app packages.
After updating, close an old Dictation instance and use
`app upgrade mica-dictation --accept-code` to obtain its new interactive launch
declaration.

## MCP

Each app includes `mcp.py`. Configure an MCP client to launch:

```sh
python3 ~/.mica/apps/mica-dictation/mcp.py --connection linux --allow-directory /path/to/audio
python3 ~/.mica/apps/mica-assistant/mcp.py --connection linux --allow-directory /path/to/media
```

For Mica Client installs use `~/.mica/client/apps/APP_ID/mcp.py` with
`--root ~/.mica/client --connection linux`. Older portable exports used
`~/.mica/clients/APP_ID/mcp.py` and `python3 client.py mcp --connection linux`.
Dictation exposes `models`,
`transcribe`, `cleanup`; Assistant exposes `models`, `assistant_chat` (text,
session ID and optional media paths). MCP messages alone go to stdout; keys are
read per request from private files, not embedded in inputs/downloads. The
adapter supports per-request discovery/tools and legacy initialize-based
clients; it is a minimal tools interface, not an HTTP MCP service or background
microphone controller. See the official
[MCP transport](https://modelcontextprotocol.io/specification/latest/basic/transports)
and [versioning](https://modelcontextprotocol.io/specification/latest/basic/versioning) contracts.

## CLI

```sh
mica-server app list
mica-server app show mica-dictation
mica-server app install mica-dictation --accept-code
mica-server app setup mica-dictation mica-dictation--mac
mica-server start --workload mica-dictation--mac
mica-server app open mica-dictation --accept-code
# Or start the global-shortcut client in the background:
mica-server app start mica-dictation --accept-code
mica-server app stop mica-dictation
```

Choose the CUDA workload instead on compatible NVIDIA hardware. Mica's global
machine allocation must satisfy the selected workload; change the allocation
through Settings if appropriate. Bundled manifests are examples, not a promise
of certification on every machine. An app can also use another compatible
workload rather than its bundled default using `app start ID --workload ID
--accept-code`. Opening/stopping apps does not restart Mica. Requests from
multiple apps use the [global scheduler](scheduling.md); app workload selection
and resident model workers are separate concepts.

## Real-model acceptance

After installing both demos, preparing their workloads, and starting Assistant,
run the opt-in acceptance script on the Mica host:

```sh
python3 scripts/demo_apps_acceptance.py \
  --audio artifacts/benchmarks/audio8-gguf/q8_0-plain.wav \
  --output /tmp/mica-demo-apps-acceptance.json
```

The defaults target the CUDA/CPU workloads and port 55000. For another host,
pass `--base-url`, `--assistant-workload`, and `--dictation-workload`. The script
reads private key files, checks actual ASR and LLM cleanup, calls Assistant
through its authenticated app route, tests both MCP adapters, overlaps requests
under the single-worker strategy, and checks balanced-mode memory reservations.
It restores the prior scheduler strategy and
checks that the server PID and default workload did not change. This is a smoke
test with known speech, not a quality benchmark or a microphone permission test.

Desktop validation is separate: run `mica-dictation --tui` on the device with
the microphone, save its server connection, use **Record / stop**, and check the
transcript and clipboard. Then enable the global shortcut and test it outside
the TUI. Automatic paste remains opt-in: validate it in a scratch text document,
not a terminal or valuable document. macOS permission prompts require user action;
successful file transcription does not certify recording, shortcuts, or paste.
ASR worker warm-up uses a small packaged synthetic speech WAV; Linux does not
need `espeak` for a matching binary/config installation.

## Add another source

In **Mica Client → Discover → Add publisher repository**, enter a canonical GitHub/Hugging Face repository URL,
or a direct HTTPS `apps.yaml`/`app.yaml` URL. CLI equivalents:

```sh
mica-client source add my-apps https://github.com/OWNER/REPO
mica-client source add research https://huggingface.co/OWNER/REPO
mica-client source add custom https://example.com/apps.yaml
mica-client discover
mica-client show my-apps:APP_ID
mica-client install my-apps:APP_ID --accept-code
```

Repository discovery resolves an immutable HEAD/main commit, then searches
`apps/apps.yaml`, `apps.yaml`, and `app.yaml`. GitHub discovery needs `git`;
HTTPS discovery needs `curl`. Hugging Face model/dataset/Space repositories are
supported. Other hosts need a direct HTTPS manifest/index URL, not an arbitrary
web page. Re-run `source add` with the same source ID to explicitly refresh it.
No authentication tokens are accepted in URLs; private-source authentication
and signed publishers are not implemented.

The cached catalogue works offline. Source entries are stored in
`~/.mica/client/config/app-sources.json` (or `~/.mica/config/app-sources.json`
for explicit server-host management); they are discovery metadata, not YAML workload
policy. Remote catalogue IDs are namespaced as `SOURCE:APP_ID`. App IDs are unique
per installation; selecting the same ID from another publisher cannot replace
an installed app silently. Removing a source keeps installed apps discoverable.

## Package contracts

Single-app repositories place `app.yaml` at their root. Multi-app repositories
use an index, with manifest paths relative to that index:

```yaml
schema: 1
apps:
  - id: dictation
    name: Dictation
    description: Local speech transcription and cleanup.
    manifest: dictation/app.yaml
    sha256: "<SHA-256 of the exact app.yaml bytes>"
```

An app manifest identifies its publisher and declares verified package files:

```yaml
schema: 1
id: dictation
name: Dictation
description: Local speech transcription and cleanup.
version: 0.1.0
source:
  repository: https://github.com/OWNER/REPO
  path: .
  # revision: <optional immutable 40/64-character commit digest>
distribution:
  format: files
  files:
    - path: app.py
      size_bytes: 1234
      sha256: "<64 lowercase hexadecimal characters>"
      # url: https://example.com/releases/0.1.0/app.py
    # Include README, assets and every referenced workload with their hashes.
requirements:
  operations: [audio.transcribe]
permissions: [microphone, clipboard]
launch:
  executable: python3
  arguments: ["{app_dir}/app.py", "--config", "{mica_config}", "--port", "{port}"]
  port: 55201
  path: /
workloads: []
```

This is a **file-based package installer**, not a general-purpose package manager:
1–512 files, at most 64 MiB each and 256 MiB total. Archives, arbitrary dependency
recipes, unattended app upgrades, signatures and executable sandboxes are not
implemented. Files default to URLs relative to the app manifest; explicit HTTPS
URLs may point elsewhere. Paths must be safe ASCII relative paths. No symlinks,
implicit shell interpretation or path traversal are accepted. `source.repository` is
attribution; `distribution` determines the exact bytes downloaded.

For a static web app use `launch: {static_entry: index.html}` and include that
HTML file in the verified distribution; no executable or private port is needed.
For process-backed web apps, `launch.port` is a legacy hint; the actual upstream
is leased from 55200–55399 and passed via `{port}`. Always use that placeholder,
not a hard-coded port in the arguments. The browser opens `/apps/APP_ID/` on
55000 (server) or 55001 (client), not the upstream port.

`launch.port` and `launch.path` are optional together: omit both for a headless
or native desktop app. `launch.executable` can name a program on PATH, or a
verified package file such as `{app_dir}/bin/app`. The latter must declare
`executable: true` in its distribution file record. Arguments are passed as an
argv array, not interpreted as a shell command.

For an interactive terminal UI, declare `interfaces: [tui, ...]` and add
`launch.tui_arguments`: the full alternative argument array for the same
executable. `app open` uses it in the foreground with a real terminal; `app start`
continues to use `launch.arguments` in the background. Interactive arguments
can use `{app_dir}`, `{mica_config}`, `{mica_root}`, `{connection}` and
`{workload}`. Opening the setup TUI does not require a local model server.
Process-backed browser apps need `launch.port` and `launch.path`; static apps
instead declare only `launch.static_entry`. Host CLI/TUI open uses a
`/dashboard?app=APP_ID` login link when the local dashboard is enabled, without
putting credentials in the URL. The dashboard then authorizes the app cookie
and opens `/apps/APP_ID/...`. Popup-blocked browsers navigate in the same tab.

Bundled workloads are schema-5 YAMLs registered as `APP_ID--WORKLOAD_ID`, with
their models checked against Mica's model/engine registry before registration.
Unknown models/engines require registration separately; this version does not
silently import additional model/engine manifests from app publishers.

See [app schema](../schemas/app-v1.schema.json),
[index schema](../schemas/app-index-v1.schema.json) and the
[demo manifests](../apps/apps.yaml) for complete examples.

## Integrity, permissions and lifecycle

Mica stages files, verifies byte sizes and **SHA-256 before registration**, then
publishes the installed directory under `~/.mica/apps/APP_ID`. A manifest/file
mismatch aborts installation; no code runs. Every launch verifies installed
package files again. Checksums from the same publisher detect inconsistency,
**not publisher impersonation**. `--accept-code` means you trust the publisher;
their code runs as your user, not inside a sandbox. Declared permissions are
informational; actual microphone/clipboard/accessibility approval belongs to
the OS/browser. The Mica configuration may contain credentials—only launch apps
you trust with that access. Never install untrusted apps because a hash matches.

Launching requires a running Mica server and a selected workload whose model
interactions and engine contracts provide the required operations. The app
forwards that workload on requests; it does not change the server default.
The dictation client refreshes its bound workload's operations before each
transcription. The assistant picker changes only its own bridge's binding.
Backend dependencies and models are installed by ordinary workload setup and
reused across apps. App process state/logs live in `~/.mica/state/apps/`.
Launch reports a process and URL, not an inference/readiness certification.

Uninstall refuses while the app runs or its workload is active/default. It
removes unchanged registered workloads, preserves edited ones, and moves app
files to a recoverable `.removed-…` directory. Shared model/engine caches and
user data are not deleted. Do not store personal data inside package files:
put it in an application-specific data directory outside the immutable package.

For maintainers, `scripts/package_demo_apps.py` refreshes generated assistant
assets and package hashes after edits. Schema validation also verifies hashes.
