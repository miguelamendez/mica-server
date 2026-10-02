# vLLM quantization decision sheet

Date: 2026-09-17

This decision is quality-first: a format is eligible only if the target vLLM
runtime has a maintained loader/kernel and the quantized checkpoint passes
held-out model-specific evaluation. Small smoke prompts do not establish
quality parity.

## Selected candidates

| Goal | Primary candidate | Control / fallback | Why |
|---|---|---|---|
| Portable NVIDIA Q4 | AutoRound W4A16, group 128, calibrated | GPTQ W4A16, group 128 | AutoRound remains the general quality-first candidate and GPTQ the mature control. Model-specific compatibility decides which can be produced; Spark's current revision fails AutoRound block calibration, so GPTQ is its active Q4 path. Both target compressed-tensors artifacts for vLLM/Marlin. |
| Portable NVIDIA Q8 | GPTQ W8A16, group 128, calibrated | BF16 reference | Keeping activations at 16-bit is the conservative quality-first Q8 choice. It is compared with the original weights before promotion. |
| Optional NVIDIA throughput | W8A8 FP8 or INT8, calibrated | W8A16 | These are hardware-specific challengers, not default artifacts. Activation quantization can improve throughput but expands the quality gate. |
| x86/Arm CPU | compressed-tensors INT8 W8A8 where supported | BF16 | CPU runtime support and performance must be tested separately; a CUDA/Marlin result does not establish CPU compatibility. |
| Apple vLLM-Metal | native MLX affine Q8/Q4 for a listed family | MLX backend itself | The official-project Metal plugin consumes MLX checkpoints, but its narrower model table controls eligibility. CUDA compressed-tensors formats do not automatically transfer. |

Blackwell-only NVFP4/MXFP4 is intentionally excluded from the current artifact
matrix. Mica is optimizing for portable NVIDIA deployment rather than the best
result on one accelerator generation. It can be added later as an optional
hardware-specific challenger without changing the portable Q4/Q8 baselines.

“AutoRound”, “GPTQ”, and “W4A16” are not interchangeable labels: AutoRound or
GPTQ is the optimization algorithm, W4A16/W8A16 is the numerical scheme, and
compressed-tensors is the serialized artifact format. Marlin is the intended
NVIDIA execution kernel after vLLM accepts the model architecture.

Sensitive components stay at higher precision unless a model-specific test
proves otherwise: `lm_head`, embeddings, router/gate layers, vision/audio
encoders, multimodal projectors, linear-attention state layers, and MTP/drafter
weights. A mixed-precision artifact is preferable to silently degrading these
modules.

## EXL3 decision

EXL3 is not selected as a vLLM format. ExLlamaV3 is an active MIT-licensed
NVIDIA-oriented runtime and EXL3 is a QTIP/QuIP-style variable-bitrate format
with strong size/quality goals. However:

- upstream vLLM does not list EXL3 among its supported quantization formats;
- vLLM issue 19896 requesting EXL3 was closed as not planned, with no linked
  implementation;
- the current ExLlamaV3 architecture table does not list Spark2.5, MiniCPM-V
  4.6, Granite Speech 5 TurboCTC, or Audio8 ArkTTS;
- it is neither an Apple MLX format nor a CPU format.

EXL3 can become a separate NVIDIA backend in the future, not an artifact
advertised as vLLM-compatible. That would require an ExLlamaV3/TabbyAPI worker,
architecture support, actual inference, and quality/throughput comparison.

## Apple M4 decision

The September 16 vLLM-Metal release path requires Apple Silicon, macOS 15+, and
native arm64 Python 3.12. Stable installation uses prebuilt, mutually compatible
vLLM core and vllm-metal wheels; mica-server must not compile the generic vLLM
CPU source when `vllm_device=metal`.

The current vLLM-Metal support table does not list Spark's `spark2_5` custom
architecture, MiniCPM-V 4.6, Granite Speech 5 TurboCTC, or Audio8 ArkTTS.
Qwen3.5-family text support does not imply support for Spark's different model
type, and vLLM-Metal's current native multimodal table is limited to selected
image-only families. These four catalog entries therefore remain disabled on
the Apple M4 until a real worker loads and completes modality-appropriate
inference. A small listed model is used only to validate the backend install;
it does not validate the catalog models.

## Reproducible candidate workflow

The C++ command creates candidates under
`~/.mica/models/vllm/<model>/candidates/`. It never registers them as
servable variants. The defaults live in
`config/vllm_quantization_profiles.json`; every candidate writes
`mica-vllm-candidate.json` with its source revision, license, protected layers,
calibration settings, tool versions, and all validation fields initially
false. The profile pins the direct Python quantization dependencies; the
manifest records those plus the resolved transitive versions.

Toolchains are model-specific. Spark pins its checkpoint-declared Transformers
4.57.1 with LLM Compressor 0.11.0; MiniCPM and Audio8 use Transformers 5.14.1
with LLM Compressor 0.13.0. Granite needs Transformers 5.17.0 plus the
compatible LLM Compressor 0.13.1 alpha. AutoRound and GPTQ dependencies are
installed separately so one algorithm cannot silently upgrade the other's
Transformers version.

```sh
# Spark production Q4 candidate (GPTQ is the model override)
./build/mica-server quantize --model spark-x25-4b \
  --backend vllm --quant q4 --quant-device cuda

# Spark production Q8 candidate
./build/mica-server quantize --model spark-x25-4b \
  --backend vllm --quant q8 --algorithm gptq --quant-device cuda

# MiniCPM example; repeat with --quant q8
./build/mica-server quantize --model minicpm-v46-thinking \
  --backend vllm --quant q4 --quant-device cuda \
  --calibration-dataset /calibration/minicpm-production.jsonl

# Audio8 or Granite use their corresponding audited JSONL manifest.
./build/mica-server quantize --model audio8-tts-06b \
  --backend vllm --quant q4 --quant-device cuda \
  --calibration-dataset /calibration/audio8-production.jsonl
```

Local text calibration accepts JSON/JSONL records with `messages` or `text`.
Vision manifests require both `image` records (`media`, `prompt`) and `video`
records (`frames`, `prompt`). ASR records contain `audio` and optional
`sampling_rate`. TTS production manifests must mix plain `text` records and
records containing `reference_audio` plus matching `reference_text`. Relative
media paths resolve from the manifest directory. The quantizer validates the
mix before loading weights and records the manifest path, SHA-256, record
count, and modality mix in candidate metadata.

On the Apple M4 development host, conversion is additionally restricted to two
compute threads and a 16-GiB process-tree RSS watchdog. Q4 and Q8 structural
candidates now complete and reload for all four catalog models. Batch-2 tests
exercise Spark text, MiniCPM image and video, Granite ASR, and Audio8 plain and
reference-voice TTS. These tiny calibration sets validate wiring only; the
configured production size remains 512 records at up to 2,048 tokens or the
modality equivalent.

MiniCPM is registered in current vLLM core and Audio8 is listed by vLLM-Omni.
Spark still needs a compatible core architecture adapter, and Granite Speech 5
still needs a native CTC transcription loader. None of the structural artifacts
is registered for serving or eligible for upload.

For offline model-side batching, use `scripts/vllm_candidate_smoke.py` with
`--batch-size 2`. For later native-engine continuous batching, run
`scripts/openai_batch_smoke.py` against the proxy with batch sizes `1,2,4`.
The latter sends simultaneous requests to the public OpenAI-compatible routes
for text, vision, ASR, or TTS and writes a JSON report.

Detailed records:

- `docs/validation/spark-vllm-quantization-macos.md`
- `docs/validation/minicpm-vllm-quantization-macos.md`
- `docs/validation/granite-vllm-quantization-macos.md`
- `docs/validation/audio8-vllm-quantization-macos.md`

## Other methods considered

- AutoAWQ: mature vLLM input and useful Q4 comparator, but not automatically
  superior to GPTQ or AutoRound for every architecture.
- BitsAndBytes: broadly known and useful as an official-reference comparator
  (including OpenBMB's MiniCPM-V BNB checkpoint), but not the preferred curated
  serving artifact when calibrated compressed-tensors kernels are available.
- GGUF in vLLM: supported on some GPU paths but not the preferred vLLM artifact;
  use GGUF with llama.cpp in this project.
- SpinQuant and QuIP transforms: promising for low-bit accuracy and supported
  conceptually by LLM Compressor, but treated as challengers until their full
  export/runtime path is as mature as the selected baselines.
- SmoothQuant: useful for weight-plus-activation INT8, especially outlier
  handling; it does not replace the quality-first W8A16 candidate.
- RTN: fastest data-free control. It is appropriate for quick FP8/FP4 trials,
  but calibrated methods get priority for final Q4 publication.

## Acceptance gate

For each eligible target and hardware path:

1. Pin and retain the original source revision.
2. Build BF16/FP16 reference outputs and metrics on held-out, modality-specific
   data.
3. Produce the primary candidate and at least one control/challenger.
4. Run real vLLM inference, not only checkpoint loading.
5. Compare task quality, perplexity/KL where applicable, memory, prefill,
   decode, and concurrency.
6. Publish only the winner that meets the quality threshold; record unsupported
   architectures explicitly rather than forcing a misleading conversion.

Primary references:

- <https://docs.vllm.ai/en/latest/features/quantization/>
- <https://docs.vllm.ai/en/latest/features/quantization/llm_compressor/>
- <https://docs.vllm.ai/projects/llm-compressor/en/latest/steps/choosing-algo/>
- <https://docs.vllm.ai/projects/llm-compressor/en/latest/examples/autoround/>
- <https://github.com/vllm-project/vllm/issues/19896>
- <https://github.com/turboderp-org/exllamav3>
