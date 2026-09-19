<p align="center">
  <img src="apps/mica-readme-header.svg" width="100%" alt="Mica Server — democratizing local AI">
</p>

# Mica Server

> [!WARNING]
> **Active deslopification and ongoing testing.** I am actively simplifying
> Mica, removing experimental clutter, and validating it across models and
> hardware. Be mindful that profiles, engine integrations, and APIs may change
> before the first stable release. Do not expose this prototype directly to an
> untrusted network.

Mica is a small local AI model server: an OpenAI-compatible proxy, model loader,
and memory-aware load balancer for multiple inference engines. It makes a team
of specialized models practical on consumer hardware without keeping every
model resident at once.

A profile defines the models needed for a task, the engine and quantization for
each one, context/KV-cache policy, memory reservations, warmup priority, and
eviction behavior. Switch from an assistant profile to a coding profile without
reinstalling Mica or deleting cached artifacts.

The included local chat client supports text, voice, images, video, PDFs,
streaming ASR/TTS, safe Markdown, voice references, custom system prompts,
themes and branding, session history, and ZIP import/export.

## Why Mica

- One authenticated URL for text, ASR, TTS, vision, and agent routes.
- MLX on Apple Silicon; GGUF through native runtimes on macOS/Linux/WSL2.
- Hardware-aware setup for Apple, NVIDIA, AMD, Intel, CPU, and TPU paths.
- Deterministic RAM admission, warmup, lazy loading, idle eviction, and model
  swapping.
- Profiles define which models run, which engine and quantization each uses,
  their memory/context limits, and when Mica loads or unloads them.
- Q4/Q8 model variants with provenance, sizes, memory reservations, and model
  cards in a filterable registry.
- Native C++20 control plane and embedded Lua policy; Python exists only behind
  engines that require it. A GGUF-only profile does not install Python.

## Current model set

| Model | Capability | MLX | GGUF | vLLM |
| --- | --- | --- | --- | --- |
| Spark-X2.5-4B | Text generation | Q4, Q8 | Q4_K_M, Q8_0 | Not certified |
| Granite Speech 5.0 470M TurboCTC | ASR | Q4, Q8 | Q4_K, Q8_0 | Not certified |
| Audio8 TTS Preview 0.6B | TTS and voice cloning | Q4, Q8 | Q4_0, Q8_0 | Future adapter |
| MiniCPM-V 4.6 Thinking | Image/video to text | Q4, Q8 | Q4_K_M, Q8_0 | Not certified |

The four public models use permissive commercial-use licenses. Internal route
fixtures are not presented as supported models. Provenance, context/training
limits, protected quantization layers, and quality evidence live in the
[model cards](docs/model-cards/).

## Architecture

```text
OpenAI client / Mica chat
          │
          ▼
  mica-server :8080
  auth · routing · profiles
  warmup · admission · eviction
          │
          ▼
 profile-selected engine workers
  ├─ MLX: mlx-lm / mlx-vlm / mlx-audio
  ├─ GGUF: llama.cpp / audio.cpp
  └─ vLLM: hardware-specific runtime (certification in progress)
```

The proxy serves one engine family at a time, while each profile model declares
its exact engine. Engines, environments, models, caches, state, logs, and
secrets live under `~/.mica` by default. `MICA_HOME` changes that home, and
`--root PATH` is the highest-priority override.

## Install

Tagged releases provide static-Lua binaries and SHA-256 files for macOS ARM64,
Linux x86-64, and Linux ARM64:

- [Download GitHub Releases](https://github.com/miguelamendez/mica-server/releases)
- [Release and source-install instructions](docs/getting-started.md)

The tested Apple Silicon binary is about 2.2 MiB; its compressed package is
under 1 MiB. Engines and model weights are intentionally downloaded on demand.

Source build:

```sh
brew install cmake lua uv git curl libomp
git clone https://github.com/miguelamendez/mica-server.git
cd mica-server
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure -j2
```

The two-job build cap prevents large native runtime compilation from consuming
the whole machine.

## Quick start on Apple Silicon

```sh
# Inspect the deterministic plan first.
./build/mica-server plan --profile mica-assistant-mlx --ram-gib 8

# Install only the engines selected by this profile.
./build/mica-server setup --profile mica-assistant-mlx --ram-gib 8

# Start the authenticated OpenAI-compatible proxy.
./build/mica-server serve --port 8080
```

In another terminal, start the local chat:

```sh
python3 apps/mica_playground.py \
  --mica-url http://127.0.0.1:8080 \
  --api-key-file "$HOME/.mica/secrets/api-key" \
  --port 8090
```

Open <http://127.0.0.1:8090/chat>. See the
[getting-started guide](docs/getting-started.md) for release installation,
GGUF/Linux setup, hardware detection, the complete `~/.mica` layout, and
troubleshooting.

## Recommended profiles

| Profile | Purpose | Status |
| --- | --- | --- |
| `mica-assistant-mlx` | Four-model Apple Silicon assistant | Runnable |
| `mica-assistant-gguf` | Portable native assistant with no Python | Runnable |
| `mica-assistant-gptq` | Planned four-capability vLLM/GPTQ profile | Awaiting native-hardware certification |

List local or GitHub-hosted profiles:

```sh
./build/mica-server profile list
./build/mica-server profile list --remote
./build/mica-server profile install mica-assistant-mlx
```

Profiles can also be created, validated, installed, edited, or loaded directly
from a local JSON file. See [Profiles and memory policies](docs/profiles.md).

## Model registry

```sh
./build/mica-server registry list
./build/mica-server registry list --modality asr
./build/mica-server registry list --modality video-text-to-text --backend mlx
./build/mica-server registry ping --modality tts
```

The ledger exposes descriptions, modalities, licenses, repositories,
quantization types, formats, artifact sizes, and memory reservations. See
[Models, quantization, and publishing](docs/models.md).

## Documentation

| Guide | Contents |
| --- | --- |
| [Getting started](docs/getting-started.md) | Releases, source builds, engines, chat, and `~/.mica` |
| [Profiles](docs/profiles.md) | Model collections, memory policy, context, batching, and custom profiles |
| [API reference](docs/api.md) | Authentication, model discovery, chat, ASR, TTS, agent streaming, and sessions |
| [Models](docs/models.md) | Registry, custom models, quantization, provenance, and Hugging Face publication |
| [Development](docs/development.md) | Tests, benchmarks, release packaging, troubleshooting, and security |
| [vLLM quantization](docs/vllm-quantization.md) | Candidate methodology and certification gates |
| [Architecture decisions](docs/design/architecture-decisions.md) | Runtime, storage, engine, and scheduler decisions |
| [Agent state machine](docs/design/agent-chat-state-machine.md) | Multimodal chat/tool flow |
| [Validation evidence](docs/validation/) | Hardware-specific inference and quality results |

## Roadmap

- [ ] Certify the four-model GPTQ/vLLM assistant on CUDA/ROCm/XPU hardware.
- [ ] Add image-generation engines, routes, profiles, and Q4/Q8 artifacts.
- [ ] Add Stable Audio/DiffRhythm engines and quantized music profiles.
- [ ] Add a Qwen2.5-Coder-7B coding profile.
- [ ] Expand native-hardware release and quality testing.

## License

Mica Server is licensed under [GPL-3.0](LICENSE). Model artifacts retain their
own licenses; consult each model card before redistribution or commercial use.
