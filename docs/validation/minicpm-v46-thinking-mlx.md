# MiniCPM-V 4.6 Thinking MLX Q4/Q8 validation

Date: 2026-09-16

Status: Q4 and Q8 passed real text, image, video, and server inference. Q8 is
preferred when output-format fidelity matters; Q4 is accepted with a formatting
caveat.

## Provenance and artifacts

- Source: <https://huggingface.co/openbmb/MiniCPM-V-4.6-Thinking>
- Immutable revision: `93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a`
- License: Apache-2.0
- Source BF16 weights: 2,600,957,528 bytes
- Q4 weights: 2,157,823,786 bytes, SHA-256
  `02618a1891a802e3f69ad77b432ff6c5ce86fae6a6faa64f8e65bdc480d8454f`
- Q8 weights: 2,311,964,514 bytes, SHA-256
  `c0928bf32a570e87979b8da433fec141aed7b553124437527cbc380cde27fa7a`

The source remains in the revisioned staging directory until upload and a clean
redownload validation are complete.

## Quantization policy

Both variants use MLX affine quantization with group size 64. MiniCPM-V's
model-specific predicate quantized 96 language modules:

- all 24 MLP `gate_proj`, `up_proj`, and `down_proj` modules (72 total);
- `q_proj`, `k_proj`, `v_proj`, and `o_proj` in the six full-attention layers
  (24 total).

The following quality-sensitive components remained at source precision:

- the complete vision tower;
- `vit_merger` and `merger` visual projection paths;
- all recurrent `linear_attn` modules;
- token embeddings and the language output head.

Header inspection found zero quantization-scale tensors under a protected path.
Because most of this small multimodal model remains protected, Q4 averages
13.274 bits/weight and Q8 averages 14.222 bits/weight. These names therefore
describe the quantized language projections, not blanket checkpoint precision.

## Direct inference

| Variant/task | Prompt or prefill | Decode | Peak memory | Result |
| --- | ---: | ---: | ---: | --- |
| BF16 text | 512.43 tok/s | 59.21 tok/s | 2.698 GB | ran |
| Q4 text | 451.01 tok/s | 80.61 tok/s | 2.274 GB | ran |
| Q8 text | 454.14 tok/s | 71.89 tok/s | 2.429 GB | ran |
| BF16 image | 335.05 tok/s | 58.30 tok/s | 3.260 GB | facts + format passed |
| Q4 image | 183.35 tok/s | 78.24 tok/s | 2.814 GB | facts passed; JSON omitted |
| Q8 image | 335.93 tok/s | 70.23 tok/s | 2.971 GB | matched BF16 response |
| Q4 video | 333.06 tok/s | 84.65 tok/s | 3.029 GB | exact pass |
| Q8 video | 339.34 tok/s | 77.74 tok/s | 3.176 GB | exact pass |

The deterministic image contains `MICA 31415`, a blue upper-left square, and a
red lower-right square. All variants recovered the facts. Q4 did not obey the
request to append JSON, whereas Q8 emitted the same JSON-like response as BF16.

The six-second deterministic video presents red, green, then blue stages. Both
quantizations sampled 12 of 24 frames and returned exactly
`FIRST=red; SECOND=green; THIRD=blue`.

BF16, Q4, and Q8 all failed the intentionally tricky “all but nine sheep” text
prompt in the same way, answering eight instead of nine. This is recorded as a
model limitation, not a quantization-specific regression.

## OpenAI-compatible server

Both variants loaded in `mlx_vlm.server`, appeared in `/v1/models`, and served
real `/v1/chat/completions` requests. Q4 returned `MICA_SERVER_Q4_OK`; Q8
returned `MICA_SERVER_Q8_OK`. The initial Q8 request with a 64-token limit ended
during its reasoning trace, demonstrating that Thinking models need a sufficient
completion budget. It passed with a 256-token budget.

Q8 streaming also passed: incremental reasoning arrived in
`reasoning_content`, final tokens arrived in `content`, the final chunk reported
`finish_reason: stop`, and the stream ended with `[DONE]`.

Machine-readable measurements are in
`artifacts/benchmarks/minicpm-v46-thinking-mlx.json`.
