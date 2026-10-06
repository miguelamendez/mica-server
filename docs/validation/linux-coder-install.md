# Linux coding-workload installation

## Scope and status

The requested workload is `mica-coder-qwen-gguf`, not the older Bonsai/Ling
workload. It uses stock llama.cpp for all models. Prism is installed separately
for future Bonsai use; it is not required by this workload.

The target is Ubuntu 24.04 on a Ryzen 5 9600X, with approximately 60.5 GiB host
RAM and an RTX 5060 Ti reporting 15.93 GiB dedicated memory. Detection is
performed by Mica, not a separate Python hardware detector.

Machine policy permits CPU and CUDA device 0, allocates 16 GiB host RAM and
15.5 GiB VRAM to inference, and limits builds to one compiler job / 8 GiB.
The test harness additionally monitors build-process-tree RSS. These are not
claims that every backend has an OS-enforced GPU memory limit.

CUDA compiler 12.8.93 and development libraries were installed without
upgrading the working NVIDIA driver. Native Blackwell compilation is enabled.
The detector currently reports zero physical CPU cores on this host; logical
core detection works. That discovery bug remains open.

## Workload

| Model | Artifact | KV cache | Startup | Context allocation |
| --- | --- | --- | --- | --- |
| Spark-X2.5-4B | Q4_K_M | Q8 | Default text model, warm first | 65,536 input + 16,384 output |
| Qwen3.8 27B GSQ-RCO | IQ3_XXS + BF16 projector | Q4 | On demand | 65,536 input + 16,384 output |
| Qwen3.5 9B | Q4_K_M + BF16 projector | Q8 | On demand; default image/video model | 65,536 input + 16,384 output |

Maximum residency is one worker. Mica must evict an idle model before loading
another, rather than trying to keep all three in VRAM. The large coder uses
Q4 KV cache because the conservative Q8 reservation at 81,920 total tokens
would exceed the allocated VRAM after the safety margin. Admission estimates
are not measured peak usage or long-context quality certification.

The workload downloads pinned, existing quantizations; no requantization is
performed. The Qwen bundles include their BF16 projectors. Model manifests
record byte sizes, SHA-256 checksums, revision IDs, and original sources.

## Reproduction

From a fresh checkout and installed Mica core:

```sh
export CUDACXX=/usr/local/cuda-12.8/bin/nvcc
export CUDAToolkit_ROOT=/usr/local/cuda-12.8
export PATH=/usr/local/cuda-12.8/bin:$PATH
mica-server plan --profile mica-coder-qwen-gguf --ram-gib 16 --vram-gib 15.5
mica-server setup --profile mica-coder-qwen-gguf --ram-gib 16 --vram-gib 15.5
mica-server serve --host 127.0.0.1 --port 8092
```

An existing `~/.mica/config/machine.yaml` must permit these allocations.
Setup does not silently raise machine limits. Models are downloaded on first
serve, before startup warmup. Authentication uses the private API-key file;
never commit it or embed its value in example commands.

## Acceptance checklist

- Core source build and native hardware detection: passed before this workload.
- Existing core test suite: 75/75 passed on Linux before the new model addition.
- New manifest/schema checks: passed locally (59 documents).
- Prism CUDA build: in progress.
- Stock llama.cpp CUDA build: pending completion of the Prism build.
- New workload setup and model checksums: pending.
- Real Spark and Qwen coder inference: pending.
- Real Qwen3.5 image/video inference and streaming: pending.
- API authentication, default routing, and one-worker swaps: pending.
- Full 64K-input / 16K-generation quality and peak-memory certification: not tested.

Completion will be recorded only after actual inference, not merely a healthy
proxy endpoint or successful download.
