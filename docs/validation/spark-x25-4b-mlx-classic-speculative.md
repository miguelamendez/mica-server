# Spark-X2.5-4B MLX classic speculative validation

Date: 2026-09-16

Status: **failed; do not enable the 1.7B sibling as an MLX drafter.**

## Pair and conversion

- Target: `XHToken/Spark-X2.5-4B`, MLX affine Q4/group 64
- Draft: `XHToken/Spark-X2.5-1.7B` revision
  `14d6e83c13c7add2b62a7c39b2131f4ed1cddcf8`, converted from BF16 to MLX
  affine Q4/group 64
- Draft output weights: 916 MiB; converter-reported average 4.501 bits/weight
- Runtime: MLX 0.32.2, MLX-LM 0.31.3, MLX-VLM 0.7.1

All four tokenizer artifacts (`tokenizer.json`, `tokenizer_config.json`,
`special_tokens_map.json`, and `generation_config.json`) are byte-identical
between the official 4B and 1.7B checkpoints. Both use vocabulary size 131072
and a 512-token sliding window. The 1.7B model is 28 layers/2048 hidden; the 4B
model is 36 layers/2560 hidden.

MLX-VLM does not expose a classic peer-model drafter kind. The validation
harness adapts both MLX-VLM language models to MLX-LM's ordinary
`speculative_generate_step` and compares the emitted greedy token IDs with
plain target-model decoding.

## Results

The same 35-token prompt and 192-token completion limit were used for each
case. Peak memory includes both resident checkpoints.

| Draft block | Plain decode | Speculative decode | Draft-token share | Speedup | Exact greedy equality |
| --- | ---: | ---: | ---: | ---: | --- |
| 1 | 38.40 tok/s | 39.37 tok/s | 41.15% | 1.025x | **No** |
| 2 | 38.09 tok/s | 38.29 tok/s | 54.17% | 1.005x | **No** |

Combined peak MLX memory was 3.133 GiB, versus the previously measured 2.403
GiB Q4 target-only server peak. The extra draft therefore cost about 0.73 GiB
in this run without producing a material speedup.

Both speculative runs diverged from plain greedy output. Classic speculative
decoding is expected to preserve the target distribution, and greedy output
must be identical. The pair is therefore disabled regardless of its acceptance
rate. This is consistent with open upstream MLX-LM reports for divergent
speculative output, including with byte-identical tokenizers:

- <https://github.com/ml-explore/mlx-lm/issues/1423>
- <https://github.com/ml-explore/mlx-lm/issues/1470>

Retest only after the upstream correctness issue is fixed. A llama.cpp/GGUF
test is a separate backend validation and must not inherit this MLX result.

Machine-readable results:

- `artifacts/benchmarks/spark-x25-4b-q4-classic-spec-d1.json`
- `artifacts/benchmarks/spark-x25-4b-q4-classic-spec-d2.json`

