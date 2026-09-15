# vLLM quantization decision sheet

Date: 2026-09-15

This decision is quality-first: a format is eligible only if the target vLLM
runtime has a maintained loader/kernel and the quantized checkpoint passes
held-out model-specific evaluation. Small smoke prompts do not establish
quality parity.

## Selected candidates

| Goal | Primary candidate | Control / fallback | Why |
|---|---|---|---|
| NVIDIA Q8, least degradation | compressed-tensors W8A16 | BF16 reference; GPTQ W8A16 | Keeping activations at 16-bit is more conservative than W8A8. At 8-bit, validate whether calibration improves meaningfully over RTN. |
| NVIDIA Q4, mature baseline | GPTQ W4A16, group 128, calibrated | AWQ W4A16 | GPTQ is the established broad-compatibility baseline. AWQ remains a model-specific challenger. |
| NVIDIA Q4, quality challenger | AutoRound W4A16, group 128, `best` recipe | GPTQ baseline | Current LLM Compressor documentation reports leading or on-par INT4 quality and particular benefit for small-to-medium models; it exports compressed-tensors directly loadable by vLLM. |
| NVIDIA FP8 throughput | W8A8 FP8, calibrated when possible | W8A16 | Hardware-specific Ada/Hopper option. Activation quantization makes this a throughput candidate, not the least-degradation default. |
| Blackwell-only throughput | NVFP4 / NVFP4A16 | AutoRound or GPTQ W4A16 | Eligible only on compatible hardware and only after accuracy comparison. |
| x86/Arm CPU | compressed-tensors INT8 W8A8 or supported GPTQ | BF16 | Must be tested on the actual CPU runtime; this is separate from the NVIDIA artifact. |
| Apple vLLM-Metal | validated native MLX Q8/Q4 | MLX backend itself | Do not assume CUDA compressed-tensors formats transfer to vLLM-Metal. Its narrower support matrix controls eligibility. |

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
