# mica-server

`mica-server` is a local, OpenAI-compatible model router. Its control plane is a
native C++ binary, while the model catalog and scheduling policy are Lua files.
Python is isolated behind the MLX workers; GGUF workers are native `llama.cpp`
and `audio.cpp` processes.

## Backend lifecycle

On Apple Silicon, install MLX, GGUF, or both:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/mica-server setup --backends mlx --profile all --quant q4
./build/mica-server setup --backends gguf --profile all --quant q8
./build/mica-server setup --backends mlx,gguf --profile all --quant q4
./build/mica-server setup --backends mlx,gguf --profile all --quant q4,q8
```

Linux and Windows through WSL accept GGUF only. A nonzero `--vram-gib` selects
an NVIDIA CUDA build; zero uses CPU. On macOS, MLX and GGUF/Metal share unified
memory and therefore share the same `--ram-gib` residency limit.

`setup` installs only the requested runtime stacks and writes
`~/models/mica-server/runtime.json`. The file records:

- installed backends;
- enabled profile models and requested quantization per backend;
- RAM and VRAM budgets;
- exact resolved runtime commits;
- every completed model download and smoke-validation state.

The first `serve` for an active backend downloads its configured artifacts from
the curated `miguelamendez/*` repositories. Starting with `both` downloads both
backend families; selecting `q4,q8` downloads both quantizations as well.
Completion markers make later launches reuse the cache. A model is not marked
smoke-validated until its worker has produced an actual inference response.

## Run

```sh
./build/mica-server serve --backend mlx --port 8080
./build/mica-server serve --backend gguf --port 8080
./build/mica-server serve --backend both --port 8080
```

If setup installed both stacks, `--backend` is required at startup. With one
active backend, public model IDs are ordinary IDs such as `spark-x25-4b`. With
both backends or quantizations active, `/v1/models` exposes explicit IDs such as
`spark-x25-4b@mlx:q4` and `spark-x25-4b@gguf:q8`; an unsuffixed request selects
the first backend and default quantization deterministically (MLX/Q4 for the
corresponding combined selections). All models use the same proxy URL/routes.

The generated API key is stored at `~/models/mica-server/api-key` with user-only
permissions and is never printed:

```sh
curl http://127.0.0.1:8080/v1/models \
  -H "Authorization: Bearer $(< ~/models/mica-server/api-key)"
```

Available routes are `/health`, `/ready`, `/v1/models`,
`/v1/chat/completions`, `/v1/completions`, `/v1/audio/transcriptions`,
`/v1/audio/speech`, and `/admin/models`.

## Cache and residency are separate

Downloaded artifacts stay under:

```text
~/models/checkpoints/<backend>/<model>/<quantized artifact>
```

The RAM budget controls only live worker processes. Startup downloads the
configured cache, then warms required text and ASR models first, followed by TTS
and vision while capacity remains. On demand, the scheduler unloads zero-inflight
workers by expired TTL, oldest use, largest reservation, then lexical model ID.
Disk-cached files are not deleted by eviction.

## Catalog configuration

Edit [`config/models.lua`](config/models.lua) to choose curated repositories,
model descriptions, tags, source attribution, Q4/Q8 paths, and measured memory
reservations. Edit [`config/policy.lua`](config/policy.lua) for idle and timeout
policy. Runtime selection belongs in `runtime.json`; catalog facts belong in Lua.

The curated repositories are intended to contain only permissively licensed,
commercial-use artifacts and to retain attribution to both the base model and
any upstream quantizer.

## Development

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --clean-first -j
ctest --test-dir build --output-on-failure
```
