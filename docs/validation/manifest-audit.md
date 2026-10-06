# Manifest and routing audit

Updated: 2026-10-05. This is a local validation record, not certification of
every registered engine, model, platform, or memory limit.

## Corrected findings

| Finding | Implemented correction |
| --- | --- |
| Optional engine/artifact fields had no working automatic selection | Resolve compatible installed engines first, then manifest order; retain explicit pins and save concrete setup selections. |
| Automatic Metal placement could select zero GPU layers | Native Metal automatic placement enables GPU layers and charges unified memory to RAM once. |
| Admission reservations ignored context and batching | Account for artifact/workspace floors plus context, concurrent sequences, and KV precision; correct Audio8 reservations and voice-workload requirements. |
| MLX did not enforce the configured token/cache settings | Guard tokenized input, output, and total context; pass supported KV/concurrency settings and cap MLX device allocators. |
| Declared endpoint metadata was discarded | Honor contract paths/calls; reject unsupported transports, streaming, and tool calls. |
| Engine variants could overwrite artifacts sharing a quantization | Use engine-qualified lookup and cache paths where variants collide. |
| Existing MLX environments skipped newly required packages | Reconcile selected dependencies and install packaged worker adapters. |
| Existing native audio installs could lack a new model family | Rebuild with the required audio.cpp family union. |
| Combined image/video input and completion defaults were inconsistent | Align model interactions, schemas, parser defaults, and routing. |
| Diarization was not exposed consistently | Add `/v1/audio/diarizations`, normalize native speaker times, and use audio.cpp's details endpoint rather than its transcript-only route. |

Packaged task definitions are now named **workload profiles** and live in
`config/workloads/`; schemas are `workload-v4`/`workload-v5`. The public CLI
keeps `profile`/`--profile`; installed user profiles keep their existing
`~/.mica/config/profiles/` location. Model IDs and API routes are unchanged.

## Verification

- Single-job C++ build and 68 CTest regression cases.
- 52 engine/model/workload/catalog documents validated against JSON Schemas;
  one explicitly unavailable catalog proposal is not an executable workload.
- Temporary installed-package smoke check loads 23 visible schema-5 workloads
  from the packaged `config/workloads/` directory, including diarization. The
  24th packaged definition is a deliberately hidden vLLM test control.
- Actual Nemotron Q8 inference via Mica, using the same 28-second audio:
  MLX 0.540 seconds, audio.cpp GGUF 0.156 seconds. Both returned four speaker
  IDs; segment boundaries differ. These timings are individual warm requests,
  not a statistical performance study or a labeled diarization-error test.
- BF16 is registered but has not been inferred locally; no Q4 variant was
  verified in the supplied Nemotron repositories.

## Still open

Admission estimates and MLX device allocator caps do **not** constitute a
strict OS-enforced process-RSS ceiling. Native/GPU workspace peaks remain a
source of estimation error. Native projected media is bounded by the total
worker context; a separately enforced exact media-input token quota is not
available yet. Linux vLLM CPU/GPU and real concurrent multi-GPU execution
require target-hardware certification. Do not describe these as completed.

The audited manifest/routing/loading correctness fixes are implemented;
the open safeguards and platform validations above remain separate work.

## GSQ-RCO versus Bonsai: local comparison

Date: 2026-10-05. Hardware discovered by Mica: Apple M4, 10 cores,
24 GiB unified memory. Only one comparison worker was allowed resident at
a time; unrelated applications were not stopped. This is a smoke/performance
comparison, not a benchmark-suite accuracy certification.

### Artifacts and engines

- GSQ-RCO IQ3_XXS: 10,094,357,632-byte model and 931,146,528-byte BF16
  projector, downloaded from the publisher and SHA-256-verified by Mica.
  Revision `d562806dbafae37109975e970aae91b43e73b440` is pinned in the
  [model manifest](../../config/model-manifests/qwen38-27b-gsq-rco.yaml).
- Bonsai PQ2_0: cached approximately 7.21-GB model plus 0.93-GB BF16
  projector. This is not the smaller PTQ1_0 packing.
- Standard llama.cpp revision `e6ab7c1a41054a888ada952eab4c886444c2f5ad`
  serves GSQ; Prism llama.cpp revision
  `9a9394a895b96003ca842a6041cb28ac49a108f7` serves Bonsai.
  Existing installed engines were reused, not freshly compiled for this test.
- Neither model was locally requantized; no MTP variant was tested.

Publisher results are useful background, but not a head-to-head comparison:
[GSQ's card](https://huggingface.co/ISTA-DASLab/Qwen3.8-27B-GSQ-RCO-GGUF)
reports IQ3_XXS AIME25 100.00, GPQA-Diamond 88.89 and LiveCodeBench v6 84.57.
[Bonsai's card](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf)
reports AIME25 95.00 and LiveCodeBench 90.07 without the same version label.
Their own base-model AIME25 scores differ (100.00 versus 96.67), demonstrating
that the protocols are not identical. Do not infer a controlled quality
ranking or percentage retention between these models from those numbers.

### Matched short-context settings and results

The [isolated workload](../../config/workloads/qwen38-gsq-bonsai-comparison.yaml)
uses an 8192-token worker context, Q8 K/V cache, four CPU threads, full Metal
layer offload, one concurrent request and on-demand residency for both models.
The short tests used native batch 128 / microbatch 64; projector GPU offload
was left at the engines' default. Requests used temperature 0, seed 42,
thinking disabled via `chat_template_kwargs.enable_thinking=false`, and
`cache_prompt=false`. Returned reasoning was empty and cached tokens were
zero. Separate arithmetic warmup was excluded from timed cases.

Each text prompt targeted 512 tokens; actual counts are below. This is one
sample per case, not a confidence interval. The output cap was 256 for Bonsai
and 384 for the earlier completed GSQ short sample; neither sample hit its
cap, but the cap mismatch is recorded rather than hidden.

| Model / task | Actual input tokens | Output tokens | Decode tokens/s | Request seconds |
| --- | ---: | ---: | ---: | ---: |
| GSQ IQ3_XXS / sheep reasoning | 487 | 27 | 7.33 | 13.43 |
| Bonsai PQ2_0 / sheep reasoning | 487 | 16 | 10.26 | 11.48 |
| Bonsai / palindrome coding | 482 | 125 | 9.98 | 22.12 |
| Bonsai / seasons knowledge | 481 | 142 | 9.41 | 25.23 |
| Bonsai / project summary | 483 | 95 | 9.65 | 20.55 |
| Bonsai / synthetic image OCR and colors | 324 | 57 | 8.86 | 15.40 |

On the paired sheep prompt, Bonsai decode throughput was approximately 1.40x
GSQ, but this does not establish an overall speed or accuracy advantage.
Different response lengths affect total request latency. Prompt processing
was 48.80 tokens/s for Bonsai and 50.01 for GSQ on that prompt.

Both answered that nine sheep remain. Bonsai produced a plausible palindrome
function and four correct sentences about seasons. The summary preserved
alpha, omega and all three risks, but added an unsupported claim that the
project was currently in progress: it is not an unqualified accuracy pass.
The image answer correctly read **MICA 31415**, blue upper-left square and
red lower-right square. These observations are manual fixture checks, not
HumanEval, MMLU or a representative vision evaluation.

### Memory stops and incomplete coverage

The monitor watched process-tree RSS against 16 GiB and additionally stopped
at macOS system memory pressure level 2 (warning). These are polling safeguards,
not an OS-enforced physical-memory ceiling. Metal/unified-memory pressure can
increase while RSS is well below the configured threshold.

| Attempt | Settings | Outcome |
| --- | --- | --- |
| Initial GSQ matrix | Default native batches | Five cases saved; warning observed and run stopped; sampled RSS peak 5.57 GiB. |
| Smaller GSQ matrix | Batch 128 / microbatch 64 | Short sheep case completed; next 2048-target prefill stopped automatically at pressure warning; sampled RSS peak 11.87 GiB. |
| CPU-projector fallback | Batch 64 / microbatch 32, projector CPU | Stopped during startup warmup at warning; sampled RSS peak 10.33 GiB; no timed cases. |
| On-demand Bonsai then GSQ | Batch 128 / microbatch 64 | All five Bonsai cases completed with normal observed pressure; subsequent GSQ warmup triggered the guard; whole-session sampled RSS peak 10.65 GiB. |

In the initial, differently batched GSQ run, sheep prompts targeting 2048 and
4096 tokens began with **"8 sheep remain"** while the explanation said nine.
Those contradictory outputs are retained as failures, not discarded.
Its coding samples were plausible, but those timings must not be presented
as batch-matched comparisons against the later Bonsai matrix.

The fresh short GSQ matrix did not complete warmup; no result file is claimed
for that attempt. GSQ OCR on the full synthetic fixture, long-context matched
quality/speed, creativity, repeated samples and video comparison remain
incomplete. Model loading included tiny-image startup warmup, which is not a
substitute for the full image test. All comparison workers were stopped;
downloaded weights remain cached. Normal/default setup was not changed.

### Saved evidence and reproduction

- [Initial GSQ partial matrix](../../artifacts/benchmarks/matrix/apple-m4-gsq-iq3xxs-default-batch-partial.json)
- [Smaller-batch GSQ partial matrix](../../artifacts/benchmarks/matrix/apple-m4-gsq-iq3xxs-small-batch-partial.json)
- [Completed Bonsai short matrix](../../artifacts/benchmarks/matrix/apple-m4-bonsai-pq2-short.json)

JSON files contain exact requests, outputs, token counts, finish reasons and
native timings; API keys are not included. The comparison setup lives under
an isolated Mica root, using the installed runtime and model caches. To
reproduce with your own isolated root and cached bundles:

```sh
mica-server setup --profile qwen38-gsq-bonsai-comparison \
  --machine-file tests/fixtures/machine/gsq-bonsai-macos.yaml \
  --ram-gib 16 --root "$MICA_COMPARISON_ROOT"
env LLAMA_ARG_BATCH=128 LLAMA_ARG_UBATCH=64 \
  python3 scripts/run_memory_limited.py --limit-gib 16 \
  --macos-pressure-limit 2 --poll-seconds 0.25 -- \
  mica-server serve --root "$MICA_COMPARISON_ROOT" --port 9298
```

Define `MICA_COMPARISON_ROOT` explicitly before running these commands. Run
the matrix client separately with `--disable-thinking --warmup
--no-prompt-cache --context-sizes 512 --concurrency 1`, the model ID and the
isolated root's key file. Avoid simultaneous workers or removing the warning
guard just to finish the larger matrix on this hardware.

Regression verification after these additions: single-job C++ build,
69/69 CTest cases passed (including the offline memory-monitor tests),
54 engine/model/workload/catalog documents validated, and `git diff --check`
passed. Local API tests require loopback permission; their sandbox-denied
attempt was rerun successfully with that permission. No changes were committed
or pushed as part of this comparison.
