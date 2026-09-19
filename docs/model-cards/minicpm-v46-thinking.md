---
language:
- en
- zh
library_name: transformers
pipeline_tag: image-text-to-text
license: apache-2.0
base_model: openbmb/MiniCPM-V-4.6-Thinking
tags:
- mica-server
- image-text-to-text
- video-text-to-text
- thinking
- mlx
- gguf
- commercial-use
mica:
  schema: 1
  model_id: minicpm-v46-thinking
  mica_server_repo: https://github.com/miguelamendez/mica-server
  curated_repo: https://huggingface.co/miguelamendez/mica-minicpm-v46-thinking
  source_repo: openbmb/MiniCPM-V-4.6-Thinking
  source_revision: 93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a
  parameters: approximately-1.2B-components
  context:
    unit: text-and-visual-tokens
    architecture_max_sequence: 262144
    training_max_sequence: null
    training_max_input: null
    model_max_output: null
    semantics: input-plus-output
  reasoning:
    supported: true
    default: enabled
    modes: [enabled, disabled]
    effort_levels: null
---

# MiniCPM-V 4.6 Thinking — Mica Q4/Q8

Mica runtime artifacts for
[`openbmb/MiniCPM-V-4.6-Thinking`](https://huggingface.co/openbmb/MiniCPM-V-4.6-Thinking),
pinned to revision `93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a`.
The source and derivatives are Apache-2.0 licensed.

The runtime configuration, conversion policy, and validation evidence are
maintained in the [Mica Server repository](https://github.com/miguelamendez/mica-server).
Validated artifacts are published together in the
[`miguelamendez/mica-minicpm-v46-thinking`](https://huggingface.co/miguelamendez/mica-minicpm-v46-thinking)
repository, with manifests recording their source, size, and digest.

## Model facts and provenance

The model combines a SigLIP2-400M vision encoder and a Qwen3.5-0.8B language
model with mixed 4x/16x visual-token compression. The pinned text configuration
declares 262,144 architectural positions. Upstream does not disclose a maximum
training sequence, maximum input alone, or a separate hard output ceiling, so
the architectural number must not be represented as a locally validated or
training-safe context.

This is the long-chain-of-thought variant. Thinking is enabled by default and
the pinned chat template permits it to be disabled. No low/medium/high effort
levels or recommended reasoning-token budgets are disclosed.

For video, upstream defaults to at most 128 frames: approximately 1 FPS for
short videos and uniform sampling for longer videos. Upstream examples request
512 new tokens for image work and 2,048 for video, but these are example request
budgets rather than declared hard generation limits.

## Mica artifacts and quantization policy

```text
mlx/q4/    selective MLX affine Q4, group size 64
mlx/q8/    selective MLX affine Q8, group size 64
gguf/      Q4_K_M and Q8_0 plus full-precision projector
vllm/      not published; structural candidates failed the production gate
```

MLX quantizes eligible language projections while preserving the vision tower,
vision mergers, recurrent linear-attention blocks, token embeddings, and output
head. The effective checkpoints therefore average 13.274 bits per weight for
Q4 and 14.222 for Q8; the directory names describe eligible affine layers, not
blanket whole-model precision.

The portable Linux/NVIDIA comparison is AutoRound W4A16 group-128 versus GPTQ
W4A16 group-128, followed by GPTQ W8A16 group-128. The vision tower, merger,
recurrent linear-attention modules, token embeddings, and output head stay at
source precision. A text-calibrated artifact is only an early candidate; it
cannot be promoted until image/video calibration, real multimodal vLLM
inference, and BF16 quality comparison pass. Blackwell-only formats are not in
the current matrix.

Q4 and Q8 structural runs now calibrate from a real image and an ordered
two-frame video. Both serialized checkpoints reload for batch-2 image and video
generation. This is still not a quality artifact: the production calibration,
BF16 comparison, and native Linux/NVIDIA vLLM tests remain mandatory.

No MTP drafter is included. Although the config declares
`mtp_num_hidden_layers`, the published checkpoint contains no separable MTP or
next-token tensors.

Current vLLM core recognizes this exact MiniCPM architecture. The earlier Apple
vLLM-Metal 0.29 probe still fails during image-processor dispatch before
loading weights; a newer core support entry does not establish Metal-plugin
compatibility.

## Mica validation boundary

MLX Q4 and Q8 passed real text, image, video, and OpenAI-compatible server
inference. Q4 omitted a requested JSON suffix in one image test; Q8 matched the
BF16 formatting and is preferred when structure fidelity matters. Recorded
video peaks were 3.029 GB for Q4 and 3.176 GB for Q8. GGUF Q4_K_M and Q8_0 also
passed ordered image/video inference.

The tests do not certify 262K context. Mica must count text, image slices, video
frames, system/tool tokens, reasoning, and visible output within the selected
profile's much smaller tested budget.

Validation records:

- `docs/validation/minicpm-v46-thinking-mlx.md`
- `docs/validation/minicpm-v46-drafter-audit.md`
- `docs/validation/minicpm-vllm-quantization-macos.md`
- `docs/validation/gguf-runtime-macos-metal.md`

## Profile validation requirements

A MiniCPM profile must define text input, visual-token/media limits, maximum
output, reasoning budget, KV precision, and concurrent sequences. It must warn
above upstream-recommended or training context when those values become known,
and fail above 262,144 total tokens or the active backend limit.
