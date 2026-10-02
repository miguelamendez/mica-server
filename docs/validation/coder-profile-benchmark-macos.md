# Coding-profile task and media benchmark: Apple M4

Date: 2026-09-21  
Profile: `mica-coder-bonsai-macos`  
Hardware: Apple M4, 24 GiB unified memory, Metal  
Route: authenticated Mica `/v1/chat/completions`, one request at a time

The existing `scripts/benchmark_matrix.py` client ran the prior reasoning, coding, creativity, and synthetic-project-summary tasks against Bonsai PQ2_0/Prism Metal and Spark Q4/MLX. Each task used target input lengths of 512, 2048, and 4096 tokens and a 384-token completion cap. Actual model-reported prompt lengths were about 500, 2000, and 4100 tokens. The same client tested the deterministic image and six-second video assets against MiniCPM-V 4.6 Thinking Q4/MLX. All requests used temperature 0 and concurrency 1.

Decode rates below are the backend-reported `predicted_per_second`, including private reasoning tokens when the model emits them. They are **not** end-to-end response rates or a claim of answer quality. Each cell lists 512 / 2048 / 4096-target results in tokens per second.

| Task | Bonsai PQ2_0 | Spark MLX Q4 |
| --- | --- | --- |
| Reasoning | 10.17 / 9.61 / 8.98 | 30.54 / 34.52 / 35.75 |
| Coding | 8.55 / 7.86 / 7.43 | 37.92 / 37.11 / 35.22 |
| Creativity | 7.30 / 7.06 / 6.81 | 37.33 / 36.21 / 35.47 |
| Summary | 6.90 / 8.73 / 8.63 | 37.58 / 36.88 / 35.73 |

Bonsai's all-case mean was 8.17 tok/s; its mean across the nine cases that stopped normally with visible output was 8.54 tok/s. Spark's all-case mean was 35.86 tok/s, but all twelve visible responses being complete does **not** mean all twelve were correct.

## Answer checks

- Bonsai correctly answered the “all but 9 sheep” question with **9** at all three input lengths. Its palindrome code and milestone/risk summaries produced visible, relevant answers and stopped normally.
- Bonsai's creativity task produced **no visible story** at the 384-token cap for all three lengths. At the 512-target input, repeating with 768 and then 1024 completion tokens still filled the cap inside the private reasoning trace with no visible content. Those decode rates measure thinking, not usable creative-writing throughput.
  A later local smoke test using the same lighthouse prompt with `reasoning_effort=low`, `thinking_budget_tokens=64`, and `max_tokens=384` did produce a visible story (215 completion tokens total). This confirms the budget control addresses that failure mode; it is not a rerun of the full benchmark matrix.
- Spark's sheep answers were wrong or nonresponsive: at 512 it claimed insufficient information, while at 2048 and 4096 it answered **8**, misreading “all but 9” as “9 ran away.” Its palindrome functions were usable, though the 4096-target answer added an unnecessary preface. It wrote scenes at 512 and 2048, but at 4096 asked for clarification instead of writing the requested scene. Its summaries retained the synthetic log's milestones and risks.
- MiniCPM-V read `MICA 31415` and the blue/red regions in the image (49 generated tokens, 73.74 tok/s, 4.93 s end-to-end). Through the Mica proxy's `input_video` route it described the six-second fixture in the correct **FIRST RED → SECOND GREEN → THIRD BLUE** order (59 generated tokens, 73.20 tok/s, 18.15 s end-to-end). This validates the MLX video route for this small fixture, not long-video reliability.

The server ran under `scripts/run_memory_limited.py --limit-gib 16`. Peak observed process-tree RSS was **9.56 GiB**; the monitor did not terminate it. This is an RSS observation and stop policy, not complete Metal/unified-memory accounting or a kernel-enforced memory cap. The server was stopped after the runs and the model cache retained.

## Machine-readable results

- [Bonsai 12-case matrix](../../artifacts/benchmarks/matrix/apple-m4-bonsai-coder-profile-gguf-pq2.json)
- [Bonsai 768-token creativity retry](../../artifacts/benchmarks/matrix/apple-m4-bonsai-coder-profile-creative-768.json)
- [Bonsai 1024-token creativity retry](../../artifacts/benchmarks/matrix/apple-m4-bonsai-coder-profile-creative-1024.json)
- [Spark 12-case matrix](../../artifacts/benchmarks/matrix/apple-m4-spark-coder-profile-mlx-q4.json)
- [MiniCPM image and video](../../artifacts/benchmarks/matrix/apple-m4-minicpm-coder-profile-mlx-q4-media.json)

This subset does not cover the profile's full 8192-token context, concurrent/batched inference, Linux/NVIDIA, or longer videos. The 8192- and 16384-target cases from the older general suite were deliberately omitted because the active Bonsai/Spark profile limits input to 6144/7168 tokens respectively.
