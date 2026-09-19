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

`auto` selects MLX on Apple Silicon and GGUF elsewhere. A schema-2 profile
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
mica-server detect --output "$HOME/.mica/state/hardware-profile.json"
```

Plan makes no changes:

```sh
mica-server plan --profile mica-assistant-mlx --ram-gib 8
```

Set up only the engines required by the selected profile:

```sh
mica-server setup --profile mica-assistant-mlx --ram-gib 8
```

Setup uses the hardware profile to choose Metal, CUDA, HIP/ROCm, SYCL,
Vulkan, XPU, TPU, or CPU paths. Native builds use at most two compiler jobs and
a 16 GiB compilation ceiling. Models are downloaded lazily during warmup or
their first request.

## Run the server

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
│   └── profiles/                installed schema-2 profiles
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
