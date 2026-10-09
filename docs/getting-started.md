# Getting started

This guide covers release installation, source builds, engine setup, the
`~/.mica` application home, and the local chat client.

> **Work in progress:** Mica is under active deslopification and ongoing
> hardware/model testing. Expect some profiles and engine integrations to
> change before the first stable release.

## Choose an engine profile

| Profile | Platform | Engines | Python |
| --- | --- | --- | --- |
| `mica-assistant-mlx` | Apple Silicon macOS | `mlx-lm`, `mlx-vlm`, `mlx-audio` | Yes |
| `mica-assistant-gguf` | macOS, Linux, WSL2 | `llama.cpp`, `audio.cpp` | No |
| `mica-assistant-gptq` | Future CUDA/ROCm/XPU target | vLLM | Blocked pending four-modality certification |

`auto` selects MLX on Apple Silicon and GGUF elsewhere. A schema-5 profile
selects the concrete engine and artifact for every model; CLI backend flags do
not silently override it.

## Install a release

Tagged releases contain a static-Lua `mica-server` binary, the Lua/JSON
configuration, helper scripts, the license, and a SHA-256 file. Choose the
archive for macOS ARM64, Linux x86-64, or Linux ARM64 from
[GitHub Releases](https://github.com/miguelamendez/mica-server/releases).

Example for Apple Silicon:

```sh
shasum -a 256 -c mica-server-v0.1.0-macos-arm64.tar.gz.sha256
tar -xzf mica-server-v0.1.0-macos-arm64.tar.gz
install -d "$HOME/.local/bin" "$HOME/.local/share"
install -m 755 mica-server-v0.1.0-macos-arm64/bin/mica-server "$HOME/.local/bin/"
cp -R mica-server-v0.1.0-macos-arm64/share/mica-server "$HOME/.local/share/"
export PATH="$HOME/.local/bin:$PATH"
mica-server --version
```

The binary embeds Lua. Python is installed only when a selected MLX or vLLM
engine needs it.

## Build from source

Every platform needs CMake 3.24+, a C++20 compiler, Git, cURL, and Lua 5.4+
development files.

Apple Silicon macOS:

```sh
xcode-select --install
brew install cmake lua uv git curl libomp
git clone https://github.com/miguelamendez/mica-server.git
cd mica-server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure -j2
```

Debian, Ubuntu, or WSL2:

```sh
sudo apt update
sudo apt install build-essential cmake git curl python3 liblua5.4-dev
git clone https://github.com/miguelamendez/mica-server.git
cd mica-server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure -j2
```

Install `uv` separately when selecting MLX or vLLM. GGUF-only profiles do not
create a Python environment.

To install a source build under the user account:

```sh
cmake --install build --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
```

The installed binary locates shared configuration relative to itself.
`MICA_CONFIG_DIR` may explicitly select another configuration directory.

## Detect, plan, and set up

Inspect the machine first:

```sh
mica-server detect --output "$HOME/.mica/state/hardware.yaml"
```

Plan makes no changes:

```sh
mica-server plan --profile mica-assistant-mlx --ram-gib 8
```

`plan` reads `~/.mica/config/machine.yaml` if it exists. The machine file is
user-owned and may restrict devices, RAM, dedicated memory per device, CPU
threads, and native build jobs. To inspect a different policy without
changing your setup, pass `--machine-file PATH` to `plan`. A CPU-only example:

```yaml
schema: 1
allowed_devices: [cpu]
limits:
  inference: {ram_gib: 8}
  build: {ram_gib: 16, parallel_jobs: 1}
```

The workload profile and CLI may narrow these ceilings but cannot enable a
machine-disallowed device. Apple Metal uses unified RAM, so it has no separate
dedicated-memory allowance. Mica currently uses reservation-based admission;
these limits are not an OS-enforced hard RSS cap. The CPU-thread setting is
validated and applied to llama.cpp generation and prefill workers. Other
engines do not yet have a common CPU-thread control.

Set up only the engines required by the selected profile:

```sh
mica-server setup --profile mica-assistant-mlx --ram-gib 8
```

Setup uses detected hardware and machine policy to choose Metal, CUDA,
HIP/ROCm, SYCL, Vulkan, XPU, TPU, or CPU paths. It creates a minimal
`~/.mica/config/machine.yaml` only when absent and never overwrites an
existing one. If old runtime state exists, the first setup imports its last
resolved RAM ceiling and an unambiguous single-GPU VRAM ceiling. It also writes
`~/.mica/state/hardware.yaml` plus the legacy
hardware snapshots. Native builds use at most two compiler jobs and a
configurable build-memory planning budget up to 16 GiB; the latter is not a
kernel-enforced process limit. Models are downloaded lazily during warmup or
their first request.

## Run the server

Use the native CLI or terminal UI:

```sh
mica-server tui
mica-server workload list
mica-server workload show mica-spark-small-gguf
mica-server workload install mica-spark-small-gguf
mica-server start --host 0.0.0.0 --port 8092
mica-server status
mica-server endpoints
mica-server workload activate ANOTHER_PREPARED_WORKLOAD
mica-server stop
```

`workload install` detects hardware, installs or verifies required engines, and
downloads missing model bundles immediately. It inherits the RAM allocation
from `machine.yaml` unless `--ram-gib` narrows it. Stop before installation:
setup changes the saved runtime selection. For a fresh installation,
`start --workload mica-spark-small-gguf` combines setup, download and background
launch. Closing the terminal or TUI does not stop that server; this command
does not enable reboot autostart. Logs are in `~/.mica/logs/server.log`.

`workload activate` hot-swaps a prepared workload without restarting. Compatible
workers are retained. Missing engines require stopping and installing first.
`workload edit ID` uses `$EDITOR`, validates YAML, and retains a failed edit as a
draft. Edited definitions are read after restart; the running registry is not
silently replaced. The older `profile` commands remain available for managing
YAML definitions; `profile install` installs a definition only, not its engines
and weights.

The TUI has six views: Server, Workloads, Models, Engines, Endpoints and Settings.
Machine is a Server subsection. Choose a workload from Server to start or swap;
Enter on a workload browses its models, and Enter on a model browses its supported
engines. Model categories and compatible-only inventories keep the lists focused;
`u` reveals other hardware targets. Arrows select, Tab switches focus, `1`–`6`
select a view, `/` filters, and `?` shows actions. Mutations require confirmation.
Long operations run outside the UI thread; API keys stay hidden. See the
[TUI walkthrough](tui.md) for installation markers, cache checks and shortcuts.
Memory reservations are estimates, not universal hard process/GPU caps.

Configure a stopped server using Settings or the CLI:

```sh
mica-server config show
mica-server config set --host 0.0.0.0 --port 8092 --default-workload mica-spark-small-gguf
mica-server config set --ram-gib 16 --vram-gib 15.5
mica-server config set --rotate-api-key
# Alternatively: config set --api-key-file /path/to/private/key
mica-server start
```

Dedicated GPU limits are separate from host RAM. Apple unified memory uses
`--ram-gib`, not `--vram-gib`. `start` reconciles the selected workload with the
new limits. Key rotation never prints the new key; it is stored with mode 0600.
The control CLI reads the server's configured API-key file, so prefer a file
instead of an inline `serve --api-key` argument.

`--host 0.0.0.0` binds all IPv4 interfaces. Use a trusted network and firewall;
protected routes still require the API key, but Mica does not add HTTPS.

`serve` remains the foreground command for systemd/launchd supervision:

```sh
mica-server serve --port 8080
```

Check it from another terminal:

```sh
curl http://127.0.0.1:8080/health
curl http://127.0.0.1:8080/ready
```

The authenticated API key is generated at `~/.mica/secrets/api-key`. See the
[API reference](api.md) for chat, ASR, TTS, streaming, media, and session
routes.

## Run the chat client

Chat Settings lists server workloads with their descriptions, selected models,
memory requirements, and known installation blockers. Choose one and select
**Switch workload** to hot-swap without discarding the conversation. Swaps are
server-wide, require the API key, and wait for startup warmup. Enable **Show
generation speed** for engine-reported decode tokens/s on each reply.

From a source checkout:

```sh
python3 apps/mica_playground.py \
  --mica-url http://127.0.0.1:8080 \
  --api-key-file "$HOME/.mica/secrets/api-key" \
  --port 8090
```

The key may instead be passed with `--api-key` or configured in
`~/.mica/config/server.json` as `api_key`/`api_key_file`. The file form is
preferred because a direct argument may be visible in shell history and process
listings. The Chat Settings panel can override the fallback for the current
browser tab. With no configured or tab-level key, API calls return
`401 api_key_required` and prompt for one.

Open <http://127.0.0.1:8090/chat>. The playground supports text, voice,
images, video, PDFs, Markdown, custom TTS voices, streaming, session history,
and ZIP import/export. A key loaded from the config or key file stays in the
local Python proxy. A key entered in Settings is held in `sessionStorage` and
is discarded when that browser tab closes.

An example shared server/UI configuration is available at
[`config/server.example.json`](../config/server.example.json). Copy it to
`~/.mica/config/server.json` and keep that file private if you place an
`api_key` directly in it. CLI options override configuration values.

For a release installation, run
`python3 ~/.local/share/mica-server/apps/mica_playground.py` with the same
arguments.

Keyboard shortcuts on macOS are `Command+Enter` to send and `Option+R` to
start or stop recording. Allow microphone access for `127.0.0.1` when prompted.

## Application home

The default application home is `~/.mica`. `MICA_HOME` overrides it, and an
explicit `--root PATH` has highest priority.

```text
~/.mica/
├── cache/                       uv, download, and Hugging Face caches
├── config/
│   ├── custom-models.json
│   └── profiles/                installed schema-5 YAML profiles
├── environments/               tools, MLX, and vLLM as selected
├── logs/                        engine worker logs
├── models/<backend>/            retained quantized artifacts
├── run/                         worker state and temporary chat sessions
├── runtimes/                    native runtime sources and builds
├── secrets/api-key              generated mode-0600 key
├── staging/                     pinned full-precision sources
└── state/                       hardware, runtime, and setup evidence
```

Evicting a model from memory never deletes its disk artifact. Rebuilding an
engine never deletes models. Do not commit `~/.mica`, tokens, conversations,
or downloaded weights.

## Next reading

- [Profiles and memory policies](profiles.md)
- [API reference](api.md)
- [Models, quantization, and publishing](models.md)
- [Development, tests, releases, and troubleshooting](development.md)
- [Architecture decisions](design/architecture-decisions.md)
