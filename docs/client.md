# Mica Client: apps on your device

Mica Server hosts inference. **Mica Client installs and runs apps on the machine
where the client executes**, using a local or remote inference server. Both live
in this repository and share verified app manifests/catalogues. A remote
connection requires no local model, engine, or inference server.

## Interfaces

```sh
mica-client --help
mica-client tui
mica-client ui --open
```

The native CLI/TUI uses C++ and Lua. The HTML interface uses the existing native
HTTP library and plain HTML/CSS/JavaScript: no npm, CDN, Python environment or
new web framework. Individual apps may have their own documented dependencies.
The HTML manager binds **loopback only**, defaults to `http://127.0.0.1:55001/`,
and uses its own private management key, not a remote server's admin key.

Client state defaults to `~/.mica/client`; server state remains `~/.mica`.
`--root`, `--server-root` and `--config-dir` allow explicit alternative paths.
Connections and secrets are device-local; keys never appear in URLs or inventory.

The default source/release bundle installs both management binaries and native
Dictation together. A **client-only** device can instead copy just `mica-client`,
optional `mica-dictation`, and the matching `share/mica-server` assets from the
same release archive; it does not need the `mica-server` executable. Do not copy
only a binary and omit its matching assets. A running client UI is not a running
inference server.

## Install, then choose inference

```sh
mica-client discover
mica-client show mica-assistant
mica-client install mica-assistant --accept-code
```

Interactive installs ask **local / remote / later**. Noninteractive installation
without a selection installs files only. Installing executable code requires
explicit publisher trust; SHA-256 integrity checking is not a sandbox.

- **Remote:** choose/add a server origin, inference key and workload. The client
  verifies the workload supplies the app's required operations before binding.
- **Local:** detect the local binary, saved configuration and live health
  endpoint. Reuse an installed server. If absent, separately approve building
  and installing it; CMake, Git, a C++20 compiler and Lua development headers
  are prerequisites. Model/engine downloads require another confirmation.
- **Later:** do not install engines/models or start a server.

```sh
mica-client connect linux --url http://SERVER:55000 --workload WORKLOAD_ID \
  --api-key-file /path/to/private-inference-key
mica-client bind mica-assistant --connection linux
mica-client open mica-assistant --accept-code
mica-client connections
mica-client detect
```

Local preparation explicitly selects an app's bundled workload:

```sh
mica-client setup-local mica-assistant --workload mica-assistant--mac \
  --accept-downloads
# Add --install-server only if you approve installing a missing local server.
```

The server validates hardware, engine and memory compatibility. Preparation of
a running server uses its existing maintenance queue; apps never blindly restart
it. Listener changes or a binary upgrade remain separate server operations.
Review a workload's memory plan before approving large downloads.
A new server initializes its allocation from the selected workload's RAM/VRAM
requirements (shown before confirmation). Hardware validation can reject them;
this is not a promise that every bundled workload fits every device. Existing
machine limits are preserved, never automatically raised to fit a new app.

## Discovery and app lifecycle

```sh
mica-client source add community https://github.com/OWNER/REPO
mica-client source add research https://huggingface.co/OWNER/REPO
mica-client source remove community
mica-client list
mica-client start mica-dictation --accept-code
mica-client stop mica-dictation
mica-client upgrade mica-dictation --accept-code
mica-client uninstall mica-dictation
```

A source can publish one `app.yaml` or an index of many apps. Install, upgrade,
bind, launch and stop are independent of the inference server lifecycle.
Connections can be changed without reinstalling an app. Uninstall retains app
files for recovery and preserves shared models/engines.

The server dashboard's **Apps** section lists **apps installed on that server**.
Discovery and device-local installation belong to `mica-client`. CLI management
of server-hosted apps remains available through `mica-server app` on that host.
A browser on a Mac viewing a Linux dashboard is not evidence of a Mac client
installation: browsers cannot silently install software or inspect native apps.

## Opening apps

Web apps are opened under `/apps/APP_ID/` on their hosting server/client. Static
assets need no private service. Apps that require HTTP processes use a private
loopback upstream from the app-service pool. Native CLI/TUI apps run on their
installed device, not on the browser's device by remote magic. The HTML manager
shows the local terminal command for native apps; it cannot manufacture a TTY or
global desktop shortcut. An SSH TUI controls the SSH host.

See [ports and service pools](ports.md) and [app packaging](apps.md).

Start `mica-client ui` before opening a web app from the CLI; `open` navigates to
the manager's app link, where login establishes a scoped app cookie. Native apps
open directly in an interactive terminal. The UI must stay running to serve
device-hosted `/apps/` routes; stopping it does not stop remote inference.
