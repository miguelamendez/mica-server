# vLLM-Metal validation: Apple M4

Date: 2026-09-16  
Host profile: `artifacts/hardware-profile.json`  
Status: backend installation and a supported control model passed; the four
catalog models remain disabled for vLLM-Metal

## Runtime and memory policy

The installed isolated environment is `$MICA_ROOT/environment-vllm`
and occupies approximately 1.6 GiB on disk. The resolved packages were:

| Package | Version |
| --- | --- |
| vLLM | `0.29.0+cpu` |
| vLLM-Metal | `0.29.0` |
| MLX | `0.32.1` |
| MLX-LM | `0.32.0` |
| MLX-VLM | `0.6.17` |
| PyTorch | `2.13.0` |
| Transformers | `5.17.0` |

The host has 24 GiB unified memory and mica-server is configured with an 8 GiB
RAM limit. Direct probes used `VLLM_METAL_MEMORY_FRACTION=0.30`,
`--gpu-memory-utilization 0.30`, `--max-model-len 512` or 2,048, and at most two
sequences. Probes ran one at a time.

The proxy now translates an Apple unified-memory limit into
`--gpu-memory-utilization = max_ram_gib / unified_memory_gib`, clamped to
0.01–0.95. Thus the current 8/24 GiB configuration supplies `0.333`. Discrete
CUDA, ROCm, and XPU devices use `max_vram_gib / device_memory_gib`; CPU and TPU
do not receive this flag. This bounds vLLM's allocator, including weights,
runtime overhead, and paged KV cache, instead of accounting only for checkpoint
bytes.

This is an allocator limit, not yet mica-server's final observed-RSS hard-stop.
The process-level monitor and emergency eviction described in the profile
design remain pending.

## Supported control

`mlx-community/Qwen3-0.6B-4bit` was used only to validate the installed
vLLM-Metal runtime. It loaded on the MLX GPU and completed real
OpenAI-compatible requests. With a 0.30 device fraction, startup reported about
5.72 GB usable Metal memory, a 0.34 GB model, 0.79 GB overhead, and about 4.60
GB left for paged KV cache. The configured 2,048-token request limit remained
the request boundary even though the cache budget could hold more tokens.

| Workload | Concurrency | Completion tokens | Batch wall time | Aggregate rate |
| --- | ---: | ---: | ---: | ---: |
| reasoning smoke | 1 | 64 | 0.866 s | 73.9 tok/s |
| reasoning smoke | 2 | 128 | 0.445 s | 287.4 tok/s |
| knowledge smoke | 1 | 64 | 0.397 s | 161.1 tok/s |
| knowledge smoke | 2 | 128 | 0.404 s | 317.2 tok/s |

Every matrix response stopped at the deliberately small 64-token limit because
Qwen thinking output consumed the budget. These aggregate concurrency numbers
are a runtime smoke measurement, not single-stream decode rates or a model
quality benchmark. A separate non-thinking sheep prompt answered incorrectly,
so this 0.6B control is not a quality reference.

The explicit `/v1/chat/completions/batch` endpoint also passed with two inputs,
returning indexed outputs `Alpha.` and `BETA.` with normal stop reasons.
The detailed matrix is
`artifacts/benchmarks/matrix/apple-m4-vllm-metal-qwen3-06b-q4-control.json`.

## Catalog compatibility probes

Each Q4 MLX artifact was started directly with the same constrained settings
and `--trust-remote-code`. A load/import failure is not counted as inference.

| Model | Result | Exact boundary |
| --- | --- | --- |
| Spark-X2.5-4B | Failed before worker load | Spark custom configuration passes a scalar `rope_parameters` value where Transformers 5.17 expects a mapping. `spark2_5` is also absent from the vLLM-Metal supported-model table. |
| Granite Speech 5 TurboCTC | Failed in Metal worker load | vLLM-Metal routes the checkpoint through MLX-LM, which has no `granite_speech5_ctc` model implementation. |
| Audio8 TTS Preview 0.6B | Rejected during vLLM model validation | `ArkttsModel` is not a registered architecture. TTS would additionally require a compatible speech/Omni output worker rather than text-completion semantics. |
| MiniCPM-V 4.6 Thinking | Failed during multimodal processor setup | vLLM core recognizes `MiniCPMV4_6ForConditionalGeneration`, but current Transformers/vLLM processor dispatch rejects the checkpoint's `MiniCPMV4_6ImageProcessor`. The file does contain `image_processor_type`, so this is not missing artifact metadata. No weight inference occurred. |

MiniCPM is the only target that reached a native architecture-specific vLLM
path, but it did not reach model loading or image/video inference. It remains
disabled just like the three conclusive unsupported architectures. Patching a
checkpoint merely to pass startup is not acceptable evidence; image and video
requests must both pass on the selected Metal runtime.

## Decision

- Keep all four catalog entries `vllm_supported = false` on Apple M4.
- Use MLX/MLX-audio or GGUF for the four target models on this machine.
- Keep vLLM-Metal installation available for supported added models.
- Do not publish vLLM-labeled Q4/Q8 copies of these four artifacts yet.
- Re-run the compatibility gate when vLLM-Metal, Transformers, or the model
  repositories change; enable a model only after actual modality-specific
  inference and memory measurement pass.

Primary upstream references:

- <https://docs.vllm.ai/projects/vllm-metal/en/stable/installation/>
- <https://docs.vllm.ai/projects/vllm-metal/en/stable/configuration/>
- <https://github.com/vllm-project/vllm-metal/blob/main/docs/supported_models.md>

