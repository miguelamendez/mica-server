# GGUF runtime validation: Apple M4 Metal

Date: 2026-09-16  
Host profile: `artifacts/hardware-profile.json`  
Status: native runtimes compiled; all four catalog models passed direct Q4/Q8 inference; proxy acceptance pending

## Resource policy

- Build parallelism: 2
- Declared compilation budget: 16 GiB
- audio.cpp composite: `granite5asr,audio8_tts`
- Build priority: `nice -n 10`
- System-wide free-memory reading during the audio.cpp converter build: 87%
- System-wide free-memory reading after both audio.cpp targets: 84%

The free-memory readings are macOS `memory_pressure` observations, not peak-RSS
measurements or a kernel-enforced build cgroup.

## llama.cpp

- Source revision: `fb27a525d28381a16a4bb038858a10e4927381ca`
- Runtime version: `0.4.1-dev`, build 11009
- Compiler/platform: AppleClang 17, Darwin arm64
- Targets: `llama-server`, `llama-quantize`
- Backend: Metal
- Result: compiled and both binaries execute their version/help commands.

## audio.cpp

- Source revision: `930a47807093a7e7747ec4d8ac350c5cdfe06c4e`
- Build: Release, AppleClang 17, Darwin arm64
- Enabled backends: CPU and Metal
- Runtime devices reported:
  - `MTL:0` GPU
  - `BLAS:0` Accelerate
  - `CPU:0` Apple M4
- Targets: `audiocpp_gguf`, `audiocpp_server`
- Result: compiled and both binaries execute their help/version commands.

CMake cache evidence:

```text
AUDIOCPP_MODEL_SET=custom
AUDIOCPP_MODELS=granite5asr,audio8_tts
ENGINE_ENABLE_METAL=ON
ENGINE_ENABLE_CUDA=OFF
ENGINE_ENABLE_HIP=OFF
ENGINE_ENABLE_VULKAN=OFF
```

The shared engine still compiles common audio modules and VAD dependencies;
that does not add unrelated model families to the linked registry.

## Granite Speech 5.0 470M TurboCTC

- Source: `ibm-granite/granite-speech-5.0-470m-turboctc`
- Pinned revision: `6c14d3d052a602d850f1bc4aa017f25f4adf6aa0`
- License: Apache-2.0
- Validation clip: LibriSpeech `6930-75918-0000`, 3.505 seconds
- Reference: `CONCORD RETURNED TO ITS PLACE AMIDST THE TENTS`
- Q4_K artifact: 260 MiB; loaded server RSS 848 MiB
- Q8_0 artifact: 481 MiB; loaded server RSS 1.27 GiB
- Result for both: exact normalized transcript match

After one warm-up request, three measured requests per artifact gave:

| Quant | Wall time (ms) | RTF | Effective real-time factor |
|---|---:|---:|---:|
| Q4_K | 72.67, 49.11, 48.43 | 0.0207, 0.0140, 0.0138 | 48.2x, 71.4x, 72.4x |
| Q8_0 | 49.15, 50.53, 47.56 | 0.0140, 0.0144, 0.0136 | 71.3x, 69.4x, 73.7x |

The initial request also compiles Metal pipelines and is not a steady-state
latency measurement. Both GGUFs contain 550 tensors, 16 rank-zero scalars, 13
embedded sidecars, and a `granite5asr` model spec. The runtime warns that this
embedded spec is legacy and falls back to its current schema-v1 contract; this
must be eliminated before publishing a fully standalone package.

## Audio8 TTS Preview 0.6B

- Source: `Audio8/Audio8-TTS-Preview-0.6b`
- Pinned revision: `f07040f3d151f1ba0253bfb92cb2f5dd38b44594`
- License: Apache-2.0
- Prompt: `Daddy, can you read me a bedtime story before I go to sleep?`
- Seed/options: 42, temperature 0.7, top-p 0.9, top-k 50, retries disabled
- Clone reference: LibriSpeech `6930-75918-0000` with its exact transcript

| Quant | File | Loaded RSS | Plain wall/audio/RTF | Clone wall/audio/RTF | ASR round-trip |
|---|---:|---:|---:|---:|---|
| Q4_0 | 1.0 GiB | 1.51 GiB | 5.282 s / 4.876 s / 1.083 | 5.086 s / 4.180 s / 1.217 | exact, plain and clone |
| Q8_0 | 1.3 GiB | 1.81 GiB | 5.412 s / 4.644 s / 1.165 | 5.666 s / 4.365 s / 1.298 | exact, plain and clone |

The exact normalized ASR result in all four cases was `daddy can you read me a
bedtime story before i go to sleep`. Repeated seeded requests were byte-identical.
On this Apple M4 Metal portability path, neither precision guarantees streaming
faster than real time for every request.

Q4_K was rejected despite passing intelligibility: its 256-element block
alignment left too many codec tensors at source precision. It produced a 1.4
GiB file, used about 1.93 GiB RSS, and had no speed advantage. A mixed
Q4_K-generator/Q8_0-codec candidate was still larger at 1.5 GiB. Both rejected
artifacts remain recoverable under
`$MICA_ROOT/rejected/audio8-tts-06b/2026-09-16-q4-k-experiments/`.

## Spark-X2.5-4B

Source-weight provenance passed before adopting community GGUFs. The five
safetensors shards, index, config, tokenizer, and chat template have identical
Hugging Face blob hashes at the quantizer's pinned source revision
`ea14618d20e76b5b093d3ee20a5b9d733bb12410` and the locally staged current
revision `0bcb35678590218655dff3765b9e61c83b35e9c4`; intervening upstream commits
only changed README content.

Q4_K_M evidence:

- Quant repository: `abenzerps/Spark-X2.5-4B-GGUF`
- Pinned quant revision: `f186f3d265c583a49d5e9d19bff23150b002202b`
- SHA-256: `7934660bfc5b9bf04be0a0ac6179a1d16e1d4331b448857c86b8b2801b3ef72c`
- File size: 2,600,223,552 bytes
- Runtime compatibility: quant requires llama.cpp build 10828 or newer; tested
  runtime is build 11009
- Loaded RSS with 4096-token context and four server slots: 3.09 GiB
- Metal decode: 31.72–32.25 tokens/second across the corrected suite
- Prompt ingestion: 128.68–164.37 tokens/second after prefix-cache reuse

The reasoning and coding tasks completed correctly. The 512-token knowledge
case reached the limit near the end of its correct fourth sentence, and the
creative case used the full limit in the explicit reasoning trace before
emitting public content. These are Thinking-mode budget outcomes, not crashes
or empty generations. Both `reasoning_content` and public `content` are saved
in `artifacts/benchmarks/spark-x25-4b-gguf-q4.json`.

Q8_0 evidence:

- Quant repository and pinned revision: same as Q4_K_M
- SHA-256: `58a4fc627cc2b2cbea02f81fb22960938e86bf3e62a2b3ae01c55a678481d46b`
- File size: 4,375,020,352 bytes
- Loaded RSS with 4096-token context and four server slots: 4.92 GiB
- Metal decode: 21.24–21.40 tokens/second
- Prompt ingestion: 123.22–164.97 tokens/second after prefix-cache reuse

Q8 produced the same correct reasoning and coding outcomes as Q4. Its knowledge
and creative cases also reached the 512-token Thinking-mode limit. Q8 was about
34% slower and used about 1.83 GiB more RSS than Q4 on this host. Full outputs
and separate reasoning traces are in
`artifacts/benchmarks/spark-x25-4b-gguf-q8.json`.

## MiniCPM-V 4.6 Thinking

The official OpenBMB GGUFs were adopted after pinning revision
`2e49fbd34ad9df11f0e93c93c2c5a80ba7cdc5e5`. The original source revision is
`93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a`; the model and official quants are
Apache-2.0 licensed.

| Artifact | Size | SHA-256 |
|---|---:|---|
| Q4_K_M | 529,101,536 bytes | `2d15cea059289533a4e51cff895888a2afaf8ed0c88bcc11ba590d2a45c6f174` |
| Q8_0 | 811,591,648 bytes | `1cfadbb76a841c6a448870dda3f2f46209ae6d552a848ee4ab3205de76b5adfb` |
| F16 multimodal projector | 1,108,746,976 bytes | `b9d09d261de167b291a69958b520c6411877cbff50e96a83d06e9835627c70f6` |

Both variants used the same F16 projector, one 8192-token slot, and Metal
offload. The image fixture contains `MICA 31415`, a blue upper-left square, and
a red lower-right square. The six-second MP4 contains First Red, Second Green,
and Third Blue in chronological order.

| Quant/task | Prompt/prefill | Decode | Wall time | Loaded RSS | Result |
|---|---:|---:|---:|---:|---|
| Q4 image | 183.26 tok/s | 118.24 tok/s | 2.196 s | 2.25 GiB | exact facts |
| Q4 video | 236.51 tok/s | 101.74 tok/s | 22.869 s | 2.25 GiB | exact order and ordinals |
| Q8 image | 258.68 tok/s | 90.34 tok/s | 2.0 s | 2.28 GiB | exact facts |
| Q8 video | 237.34 tok/s | 84.83 tok/s | 22.2 s | 2.28 GiB | exact order and ordinals |

The direct MP4 path was exercised through llama.cpp's native `input_video`
OpenAI-compatible content item; frames were not extracted or substituted by
the test client. Full responses are in
`artifacts/benchmarks/minicpm-v46-thinking-gguf-q4.json` and
`artifacts/benchmarks/minicpm-v46-thinking-gguf-q8.json`.

## Remaining acceptance work

The tensor-by-tensor trusted-versus-local reproduction analysis is recorded in
[`gguf-local-reproduction-audit.md`](gguf-local-reproduction-audit.md). It
confirmed that the official MiniCPM files and Spark Q8 are reproducible at the
tensor-payload level, while the adopted Spark Q4 has the same standard
Q4_K_M precision map but different packed values. The tested trusted artifacts
remain the deployment set.

1. Register the adopted Spark and MiniCPM files in runtime state with immutable
   provenance and accepted smoke status.
2. Validate all public routes through mica-server, not only direct runtimes.
3. Exercise concurrent requests and deterministic memory admission/eviction.
4. Implement and validate true streaming passthrough; the current proxy buffers
   worker responses.
