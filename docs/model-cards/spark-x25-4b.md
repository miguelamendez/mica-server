---
language:
- multilingual
library_name: transformers
pipeline_tag: text-generation
license: apache-2.0
base_model: XHToken/Spark-X2.5-4B
tags:
- mica-server
- text-generation
- thinking
- mlx
- gguf
- commercial-use
mica:
  schema: 1
  model_id: spark-x25-4b
  source_repo: XHToken/Spark-X2.5-4B
  source_revision: 0bcb35678590218655dff3765b9e61c83b35e9c4
  parameters: 4B-class
  context:
    unit: tokens
    architecture_max_sequence: 1048576
    training_max_sequence: 1048576
    training_max_input: null
    model_max_output: null
    semantics: input-plus-output
  reasoning:
    supported: true
    default: enabled
    modes: [enabled, disabled]
    effort_levels: null
---

# Spark-X2.5-4B — Mica Q4/Q8

Mica runtime artifacts for
[`XHToken/Spark-X2.5-4B`](https://huggingface.co/XHToken/Spark-X2.5-4B),
pinned to revision `0bcb35678590218655dff3765b9e61c83b35e9c4`. The
source and derivatives are Apache-2.0 licensed.

## Model facts and provenance

| Field | Value | Meaning |
|---|---:|---|
| Parameter class | 4B | Upstream model name; an exact parameter total is not published in the card |
| Architectural context | 1,048,576 tokens | Declared by the pinned configuration; input plus output |
| Training context | sequences extending to 1M tokens | Upstream reports a dedicated long-context stage |
| Pretraining | approximately 20T tokens | Upstream disclosure |
| Maximum input alone | not disclosed | Do not infer it from the total context |
| Maximum generated output alone | not disclosed | The upstream 131,072-token example is not treated as a hard model limit |
| Thinking | supported, enabled by default | The chat template also permits `enable_thinking=false` |
| Thinking effort levels | not disclosed | Mica uses token budgets, not invented low/medium/high semantics |

The architecture combines one full-attention layer with three sliding-window
layers. Upstream reports more than 200 languages and evaluates the model in
thinking mode. Training data categories are described at a high level, but an
itemized dataset inventory is not published.

## Mica artifacts

The logical repository is intended to contain all validated formats:

```text
mlx/q4/    MLX affine Q4, group size 64
mlx/q8/    MLX affine Q8, group size 64
gguf/      Q4_K_M and Q8_0
vllm/      not published; structural candidates failed the production gate
```

The intended portable Linux/NVIDIA comparison was AutoRound W4A16 group-128
versus GPTQ W4A16 group-128, followed by GPTQ W8A16 group-128. AutoRound 0.14.2
cannot calibrate this Spark revision's custom four-dimensional attention mask,
so GPTQ is now the active Q4 path. The embedding and `lm_head` stay at source
precision. Candidates use compressed-tensors and remain outside the serving
catalog until the custom Spark architecture loads and passes real vLLM
generation plus BF16 quality comparison. Blackwell-only formats are not part
of this matrix.

Two-sample structural GPTQ candidates for both W4A16 and W8A16 now serialize,
reload, and generate with batch size two under the 16-GiB guard. They are not
production artifacts; Q4's much higher calibration error and the tiny local
fixture make the configured 512-sample calibration and BF16 comparison
mandatory.

Weight precision and KV-cache precision are independent. A runtime profile may,
for example, use Q4 weights with a Q8 KV cache.

## Mica validation boundary

On the Apple M4 host, MLX Q4 and Q8 passed real generation and server inference.
Measured peak model memory was 2.403 GiB for Q4 and 4.453 GiB for Q8 in the
recorded smoke cases. GGUF Q4_K_M and Q8_0 also passed real Metal inference.

These tests do **not** validate the upstream 1M-token claim locally. Until the
long-context matrix passes, Mica profiles must use their own certified context
limit and warn above the longest locally tested input. The smaller 1.7B sibling
failed the lossless classic-speculation gate and is not an accepted drafter.
No native MTP or DFlash checkpoint was found for this target. The Apple M4
vLLM-Metal probe failed during custom-config parsing before model load, and the
architecture is not in the plugin's support table.

Validation records:

- `docs/validation/spark-x25-4b-mlx-q4.md`
- `docs/validation/spark-x25-4b-mlx-q8.md`
- `docs/validation/spark-x25-4b-mlx-classic-speculative.md`
- `docs/validation/spark-vllm-quantization-macos.md`
- `docs/validation/gguf-runtime-macos-metal.md`

## Profile validation requirements

A Spark profile must satisfy:

```text
max_input_tokens + max_output_tokens <= 1,048,576
max_reasoning_tokens + min_visible_output_tokens <= max_output_tokens
active_batch_tokens <= configured KV/cache admission budget
```

Exceeding Mica's tested context is a warning in experimental mode and an error
in certified mode. Exceeding the architectural total is always an error.
