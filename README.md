# mica-server

`mica-server` is a local, OpenAI-compatible model router. Its control plane is a
native C++ binary, while the model catalog and scheduling policy are Lua files.
Python is isolated behind the MLX and vLLM workers; GGUF workers are native
`llama.cpp` and `audio.cpp` processes.

## Backend lifecycle

Install any required runtime stacks during setup (only one is served at a time):

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j

./build/mica-server setup --backends mlx --profile all --quant q4
./build/mica-server setup --backends gguf --profile all --quant q8
./build/mica-server setup --backends mlx,gguf --profile all --quant q4
./build/mica-server setup --backends mlx,gguf --profile all --quant q4,q8
./build/mica-server setup --backends vllm --profile all --quant q4
./build/mica-server setup --backends mlx,vllm --profile all --quant q4
```

Linux and Windows through WSL support GGUF and vLLM. The vLLM installer selects
its runtime from detected hardware:

| Machine | `--vllm-device auto` |
| --- | --- |
| Apple Silicon, macOS 15+ | vLLM-Metal with matching prebuilt vLLM/Metal wheels |
| Linux/WSL with NVIDIA GPU | upstream vLLM CUDA |
| Linux/WSL without a supported GPU | upstream vLLM CPU |

Native upstream vLLM also has an experimental CPU-only Apple Silicon build. Use
`--vllm-device cpu` to request it explicitly; setup builds it from source. Use
`--vllm-device metal` or `cuda` to require those accelerators and fail rather
than silently falling back. vLLM-Metal requires native arm64 Python 3.12 and
macOS 15 or newer. On macOS, MLX, GGUF/Metal, and vLLM-Metal share unified
memory and therefore share the same `--ram-gib` residency limit.

For GGUF, a nonzero `--vram-gib` enables the NVIDIA CUDA build on Linux/WSL;
zero uses CPU. For vLLM auto-selection, a detected NVIDIA GPU selects CUDA and
an omitted/zero budget defaults to the largest detected device. Select
`--vllm-device cpu` to force CPU-only execution.

`setup` installs only the requested runtime stacks and writes
`~/models/mica-server/runtime.json`. The file records:

- installed backends;
- enabled profile models and requested quantization per backend;
- RAM and VRAM budgets;
- exact resolved runtime commits;
- every completed model download and smoke-validation state.

The first `serve` for the selected backend downloads its configured artifacts
from the curated `miguelamendez/*` repositories. Setup may install both runtime
stacks, but each server process serves exactly one backend. Selecting `q4,q8`
caches both quantizations for that backend. Completion markers make later
launches reuse the cache. A model is not marked smoke-validated until its worker
has produced an actual inference response.

## Run

```sh
./build/mica-server serve --backend mlx --port 8080
./build/mica-server serve --backend gguf --port 8080
./build/mica-server serve --backend vllm --port 8080
```

If setup installed both stacks, `--backend` is required at startup. Running a
second backend means stopping this server and starting it with the other backend
selection. With one configured precision, public model IDs are ordinary IDs such
as `spark-x25-4b`. With multiple precisions, `/v1/models` exposes explicit IDs
such as `spark-x25-4b@mlx:q4` and `spark-x25-4b@mlx:q8`; an unsuffixed request
selects the configured default. All models use the same proxy URL/routes.

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

## Add and quantize models

Register a model using its Hugging Face URL and one of the four allowed
modalities:

```sh
./build/mica-server add-model \
  --url https://huggingface.co/owner/model \
  --modality text-to-text \
  --id my-model \
  --description "Short catalog description"
```

Allowed modalities are `tts`, `asr`, `text-to-text`, and
`img-text-to-text`. Registration reads the model card metadata and rejects an
unknown or non-commercial license; the current allowlist is Apache-2.0, MIT,
BSD-2-Clause, BSD-3-Clause, BSD, and ISC. Custom model definitions and their
variants are stored in `~/models/mica-server/custom-models.json`.

Quantize either a registered custom model or a built-in Lua catalog model for
an installed conversion backend:

```sh
./build/mica-server quantize --model my-model --backend mlx --quant q4
./build/mica-server quantize --model my-model --backend mlx --quant q8 --group-size 64
./build/mica-server quantize --model my-model --backend gguf --quant q4
./build/mica-server quantize --model my-model --backend gguf --quant q8
```

For a reproducible run, pass the immutable source commit. Without `--revision`,
the command resolves the repository's current commit before downloading:

```sh
./build/mica-server quantize \
  --model spark-x25-4b --backend mlx --quant q4 \
  --revision 0bcb35678590218655dff3765b9e61c83b35e9c4
```

Every run first downloads the complete original snapshot to
`~/models/staging/<model>/<revision>/source`, verifies its current model-card
license against the commercial-use allowlist, and writes
`source-provenance.json`. Conversion always reads that local pinned snapshot.
The source is retained until the Q4/Q8 artifacts have passed inference,
publishing, and clean re-download checks.

Registration and one quantization can be combined:

```sh
./build/mica-server add-model \
  --url https://huggingface.co/owner/model \
  --modality img-text-to-text \
  --backend mlx --quant q4
```

For upstream or already-quantized repositories that vLLM can load directly,
register a native lazy-loaded variant and give the scheduler a measured or
conservative reservation:

```sh
./build/mica-server add-model \
  --url https://huggingface.co/owner/model \
  --modality text-to-text \
  --backend vllm \
  --model-memory-gib 5.5
```

The repository is downloaded only when the model is first requested. Text,
image-to-text, and supported ASR architectures can use this path. TTS is
rejected for now because it requires a separate vLLM-Omni runtime adapter.

The converter is selected by modality, with an optional `mlx_converter`
override in Lua for models such as Spark that use the MLX-VLM architecture
registry despite being text-only. MLX dispatches to `mlx_lm.convert`,
`mlx_vlm.convert`, or `mlx_audio.convert`. A model with `mlx_extract_mtp = true`
must also produce a standalone, equally quantized MTP drafter or the conversion
is rejected. GGUF dispatches text/vision through
llama.cpp conversion plus `llama-quantize`, and audio through `audiocpp_gguf`.
Architecture compatibility is still enforced by those runtimes: a failed or
unsupported conversion is not recorded as ready. GGUF vision models must also
produce a valid `mmproj` artifact.

vLLM is a serving backend, not a generic Q4/Q8 converter. It consumes model
formats it supports (for example MLX checkpoints through vLLM-Metal, or
AWQ/GPTQ/compressed-tensors checkpoints on supported upstream platforms).
`quantize --backend vllm` is therefore rejected instead of manufacturing a
mislabelled artifact. A curated vLLM model is declared with `vllm_repo`,
`vllm_supported`, `vllm_native_path` (or a genuinely compatible Q4/Q8 path),
and measured memory fields in
`config/models.lua`. The four initial catalog models remain disabled for vLLM
until a device-compatible artifact passes real inference smoke validation;
their MLX and GGUF paths are unchanged.

## Development

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --clean-first -j
ctest --test-dir build --output-on-failure
```
