# Qwen 27B modes on a 16 GiB NVIDIA GPU

These experimental workloads reuse the **same** pinned GSQ-RCO IQ3_XXS target
weights. Bundle IDs select its dependencies; they are not separately trained
models. The DFlash and MTP heads are independently pinned to their publisher
repositories. Nothing was requantized in this experiment.

## Verified component layout

The cached `Qwen3.8-27B-GSQ-RCO-IQ3_XXS.gguf` has 851 tensors and 64
decoder blocks. Reading its GGUF metadata and tensor directory on 2026-10-06
found no `nextn_predict_layers` metadata or MTP/`nextn` tensors. **This exact
target does not contain an embedded MTP head.** This is an artifact-specific
finding, not a claim about every Qwen GGUF quantization.

The separate `mtp-Qwen3.8-27B-Q4_0.gguf` has 18 tensors,
`qwen35.nextn_predict_layers = 1`, and prediction tensors at `blk.64`.
Vision+MTP therefore downloads three files: target, BF16 projector, and
external Q4_0 MTP head. Text+DFlash downloads the target and external
Q4_K_M DFlash2 drafter, without a projector. DFlash and MTP are alternatives,
not two drafting methods stacked in the same worker.

The model manifests include exact pinned Hugging Face links for these
components. References describe availability; only entries in an artifact's
`files` array are downloaded for that bundle. Speculative decoding remains an
engine/component compatibility requirement, not a new semantic model ability.

## Workloads

| Workload / selected bundle | Input ceiling | Output reservation | Total window | Placement |
| --- | ---: | ---: | ---: | --- |
| `qwen27b-modes` / `qwen38-27b-text-dflash` | 81,920 | 32,768 | 114,688 | Target + DFlash GPU; no projector |
| `qwen27b-modes` / `qwen38-27b-vision-mtp` | 49,152 | 16,384 | 65,536 | Target + BF16 projector + Q4 MTP GPU |
| `qwen27b-modes-long` / `qwen38-27b-text` | 163,840 | 32,768 | 196,608 | Target GPU; no projector or drafter |
| `qwen27b-modes-max-context` / `qwen38-27b-text` | 229,376 | 32,768 | 262,144 | 56 of 64 decoder blocks GPU; eight CPU; no projector/drafter |

Here K means 1,024 tokens. The requested “160 input” was interpreted as
**160K input**, not 160 tokens. Input ceilings include chat framing and media
tokens. Output reservations are configuration ceilings, not claims that the
model will produce 32K useful tokens or that a 32K continuation was tested.
All workloads use Q4 K/V, one concurrent request and one resident worker.
Changing modes swaps the worker instead of loading two 27B copies into VRAM.
Reference examples require 16 GiB allocated host RAM and 15.5 GiB GPU budget,
with a separate 0.5 GiB Mica admission reserve. They target CUDA device 0.

The fast mode defaults to text+DFlash. Name `qwen38-27b-vision-mtp` for an image
request, or omit `model` and let its image default route the request. There is
no speech model in these workloads. This bundle declares images, not a
certified video or agent/tool-call workflow.

```sh
mica-server plan --profile qwen27b-modes --ram-gib 16 --vram-gib 15.5
mica-server setup --profile qwen27b-modes --ram-gib 16 --vram-gib 15.5
mica-server serve
```

An existing `machine.yaml` remains authoritative: flags cannot silently
override a smaller machine allocation. From a running, already configured
server, use the authenticated `POST /admin/profile/activate` API with
`{"profile":"qwen27b-modes"}`. API admission still checks those allocations.
Model downloads are lazy. Matching size-and-SHA pinned files in sibling model
caches are reused with a hard link when possible, avoiding duplicate copies of
the target. A fresh installation downloads the specified publisher files.

## Hardware and method

Measured on Ubuntu 24.04, Ryzen 5 9600X, 60.5 GiB physical RAM and RTX 5060 Ti
with 16,311 MiB reported GPU memory; driver 580.178.04, CUDA 12.8.93.
The native engine was llama.cpp build 11434 at
`5e03bdd8700948b9c41c54dd1b00f28a2aebc03f`. The user-owned machine allocation
remained 16 GiB host RAM / 15.5 GiB GPU memory throughout. The experiments
stopped only Mica's own Linux services, restoring them afterward; unrelated
GPU applications were left running.

Single slot; prefill batch 512, micro-batch 128; Flash Attention on; native
Jinja template; thinking disabled with
`chat_template_kwargs: {enable_thinking: false}`; temperature 0, seed 42;
no automatic fitting; two context checkpoints and no native RAM prompt cache.
Draft depth seven; draft K/V Q4. Image tests used 1,024 image tokens.
Native runs used six decode threads/eight prefill threads; Mica uses the
machine's configured thread limit. Results below are single runs, not a
multi-task throughput benchmark or a quality certification.

A watchdog sampled total GPU usage and the native server process tree,
terminating only the experimental worker when free GPU memory fell below
768 MiB or its host RSS exceeded 15 GiB. The reported GPU peak includes the
small unrelated desktop/application allocation. A window loading is not
proof that every batch/media workload fits: actual filled-context tests are
reported separately. Mica reservations are not OS-enforced VRAM limits.

## Short-prompt capacity and decoding

Same 39-token coding prompt and 256-token output cap. The rate is native
reported decode speed, not time-to-first-token or end-to-end throughput.

| Configuration | Total context | KV | Peak GPU MiB | Minimum free MiB | Decode tokens/s |
| --- | ---: | --- | ---: | ---: | ---: |
| Target only | 32,768 | Q4 | 10,717 | 5,123 | 33.12 |
| DFlash GPU | 32,768 | Q4 | 13,083 | 2,757 | 70.28 |
| DFlash GPU | 114,688 | Q4 | 14,863 | 977 | 70.29 |
| DFlash GPU | 65,536 | Q8 | 14,819 | 1,021 | 76.95 |
| Projector GPU + MTP GPU | 65,536 | Q4 | 14,729 | 1,111 | 53.86 |
| Projector GPU + MTP GPU | 32,768 | Q8 | 14,359 | 1,481 | 56.46 |
| Projector CPU + MTP GPU | 98,304 | Q4 | 14,525 | 1,315 | 53.77 |
| Target only, all GPU | 196,608 | Q4 | 14,277 | 1,563 | 33.06 |
| Target only, all GPU | 229,376 | Q4 | 14,989 | 851 | 33.06 |
| Target only, 56 GPU layers | 262,144 | Q4 | 14,487 | 1,354 | 17.82 |
| Target 56 GPU layers + CPU DFlash | 196,608 | Q4 | 14,175 | 1,666 | 23.38 |

Q4 projector+MTP at 98,304, all-GPU target-only at 262,144, and all-GPU
target+CPU-DFlash at 196,608 breached the safety margin during loading and
were stopped. These are **not certified settings**, even though no system OOM
was allowed. CPU drafting was not the best tradeoff for 160K input: the
all-GPU target-only configuration was faster and left more GPU headroom.
The largest tested all-GPU target-only window was 229,376, but its 851 MiB
remaining margin is tight; the recommended long variant uses 196,608 instead.

Q8 KV is supported but requires a smaller window. Change both target cache
precision and the context limits together; copying the Q4 maximum into a Q8
workload will not fit the same budget. Keep the drafter cache Q4 initially.

## Actually filled contexts

Repetitive text was tokenized with the native tokenizer and inserted into a
cold, uncached request. The final task was to reply `context loaded.` This
checks allocation and inference, **not** retrieval or coding quality at length.

| Configuration | Actual input tokens | Prefill seconds | Decode tokens/s at that input | Peak GPU MiB | Minimum free MiB |
| --- | ---: | ---: | ---: | ---: | ---: |
| DFlash GPU, 114,688 window | 81,815 | 140.41 | 28.84 | 14,879 | 961 |
| Target only GPU, 196,608 window | 163,736 | 352.30 | 5.49 | 14,293 | 1,547 |
| Target only, 56 GPU layers, 262,144 window | 229,271 | 732.80 | 2.69 | 14,517 | 1,324 |

The full-window partial-CPU configuration passed with 229,271 actual input
tokens and 1,324 MiB minimum GPU headroom, but prefill took over 12 minutes.
Measured host process-tree RSS peaked at approximately 9.76 GiB during these
tests. The largest input preserves the configured 32K output reservation;
all long-output reservations still need a real extended-output stress test.

## Quality and prior calibration caveats

The combined image prompt identified the blue/red squares but did not
transcribe the printed label, in both baseline and MTP modes. An OCR-only
follow-up correctly returned `MICA 31415` in both. The strict-answer checks
below agreed across target-only, DFlash and MTP; this is a small smoke suite,
not an accuracy benchmark. Initial greedy coding outputs differed between
target-only and speculative runs, so the speed measurements do **not**
establish lossless decoding or unchanged coding quality.

| Check | Expected | Target only | DFlash | Vision + MTP |
| --- | --- | --- | --- | --- |
| 17 × 23 | `391` | `391` | `391` | `391` |
| 17 sheep, all but nine leave | `9` | `9` | `9` | `9` |
| Structured record extraction | Ada / billing / false | Correct JSON | Same JSON | Same JSON |
| OCR-only image request | `MICA 31415` | Correct with projector | Not applicable | Correct |
| Square colors | blue / red | Correct with projector | Not applicable | Correct |

## Actual Mica proxy validation

The native-only tests were followed by authenticated requests through the
installed Mica server. All four configurations were activated via
`POST /admin/profile/activate`; the test waited for readiness and inspected
the actual worker process arguments after inference. It verified DFlash
without `--mmproj`, external MTP with a projector and 1,024-image-token cap,
the 196,608 window, and the 262,144 window with 56 GPU layers. Each step had
exactly one resident worker. The text+DFlash coding request measured 70.39
decode tokens/s through Mica; arithmetic returned `391` in all three text
configurations. Image requests returned `blue, red` and `MICA 31415`.

The shared target files were reused without downloading another target copy.
All 77 regression tests passed on macOS and Linux; Linux core compilation
peaked at 1.14 GiB process-tree RSS with a monitored 4 GiB cap and one job.
The original `mica-coder-qwen-gguf` workload and both Linux services were
restored after validation. New workloads are available but are not the new
startup default. The Mac server was left unchanged and remained ready.

Raw native case JSON/logs and the proxy report remain under
`~/.mica/state/qwen27b-tuning/` on the test host. The versioned, path-redacted
summary is [`artifacts/qwen27b-tuning-2026-10-06.json`](../../artifacts/qwen27b-tuning-2026-10-06.json).

An existing user `qwen` launcher and its `profiles.ini` were found on the Linux
host. That launcher previously used a different IQ3_S target with embedded
MTP, not this GSQ-RCO IQ3_XXS file. Its saved cache calibration reported PPL
5.7920 for Q8/Q8, 5.8078 for Q4/Q4 and 5.8005 for Q8/Q4, with approximately
0.068 reported uncertainty. Q4/Q4 was about 0.27% higher, within those error
bands. This is useful historical evidence, **not** current-target calibration
and not evidence that Q4 is lossless.

## Sustained-output screening (2026-10-06)

A subsequent screen compared 22 configurations at an 8,192-token reserved
window, eight decode/prefill threads, and one request at a time. Each
configuration generated 2,304 tokens from a real coding instruction. Vision
requests included the `MICA 31415` test image and requested an image-grounded
SVG generator. Thinking was disabled. Rates use native decode timing, not
image encoding, prompt processing, loading, or end-to-end latency.

| Configuration | Target K / V cache | Draft K / V cache | Draft depth | Decode tokens/s |
| --- | --- | --- | ---: | ---: |
| Text, no drafter | Q4 / Q4 | — | — | 32.24 |
| Text, DFlash2 | Q8 / Q8 | Q8 / Q8 | 7 | 83.49 |
| Text, MTP | Q8 / Q8 | Q8 / Q8 | 7 | 66.71 |
| Vision, no drafter | Q4 / Q4 | — | — | 31.25 |
| Vision, MTP | Q8 / Q4 | Q8 / Q8 | 7 | 71.70 |
| Vision, MTP | Q8 / Q8 | Q8 / Q8 | 7 | 70.67 |

These are the best observed configurations in a **single-prompt screen**, not
universal winners. Cache precision also differs from the no-drafter
baselines. All 22 passed the minimum-output-length and arithmetic smoke
checks; vision runs additionally passed simple label/color grounding checks.
These checks do not certify complete coding correctness or broad model
quality. Outputs reached the configured token cap.

Increasing vision MTP draft depth to 15 reduced throughput to 56.05–60.53
tokens/s while using more memory. A higher acceptance fraction alone does
not imply higher throughput either: fewer drafted tokens can have higher
acceptance but yield fewer useful tokens per verification step.

Raw outputs, exact requests, commands, timings, and memory observations are
under `~/.mica/state/qwen27b-peak-20261006/` on the Linux test host.
Repeat runs across coding and explanatory prompts remain pending. The 8K reserved window was not filled; these rates
must not be extrapolated to 160K/224K filled contexts. Both Mica Linux
services were restored after the screen; production workloads were unchanged.

## Context capacity and filled-budget follow-up (2026-10-06)

The fast 8K candidates were also screened with larger **reserved**, but not
filled, contexts and 2,304 generated tokens:

| Configuration | Reserved total context | Decode tokens/s | Minimum GPU free MiB | Outcome |
| --- | ---: | ---: | ---: | --- |
| Text DFlash2, target Q8/Q8, draft Q8/Q8, depth 7 | 70,656 (69 Ki tokens) | 83.53 | 794 | Passed the smoke checks |
| Same text configuration | 71,680 (70 Ki tokens) | — | 762 | Memory guard stopped it |
| Vision MTP, target Q8/Q4, draft Q8/Q8, depth 7 | 58,368 (57 Ki tokens) | 71.73 | 772 | Passed the smoke checks |
| Same vision configuration | 59,392 (58 Ki tokens) | — | 744 | Memory guard stopped it |

These near-boundary settings are not comfortable production defaults. Actual
inputs were still short, so the rates do not establish sustained speed with
tens of thousands of populated KV positions. Mixed Q8/Q4 CUDA vector kernels
are not enabled in this runtime's build; mixed precision must not be described
as a fully optimized native-kernel path.

The large-budget batch was **started**, not completed, on October 6. It fills the
reference context with nginx access-log records and asks for a complete parser,
tests, and explanation. The vision variant additionally asks for a grounded
SVG implementation using the image fixture. Thinking is disabled. Each case
requests 2,304 output tokens, with a minimum 2,000-token smoke criterion.

| Requested input budget | Reserved output budget | Total window |
| ---: | ---: | ---: |
| 163,840 (160 Ki tokens) | 32,768 | 196,608 |
| 163,840 (160 Ki tokens) | 65,536 | 229,376 |
| 196,608 (192 Ki tokens) | 32,768 | 229,376 |

There are 12 cases: each budget in text/vision, with and without drafting.
Document tokens leave a small allowance for framing, the task, and image
tokens; the recorded native `prompt_n`, not the label, is the actual input
length. The 32K/64K output numbers are reservations, **not** completed output
lengths. This batch does not certify generation of a full 32K/64K response.

All-GPU speculative settings at these larger windows failed admission.
Subsequent short admission checks passed with Q4 target/draft KV, batch 128,
microbatch 32, and the following candidates:

- Text DFlash2: target 56 GPU layers at 196,608 total, 52 at 229,376; drafter
  remains on GPU. Short-check GPU headroom was 874/968 MiB.
- Vision MTP: target 48 GPU layers at 196,608 total, 44 at 229,376; projector
  on CPU, external MTP head on GPU. Short-check headroom was 1,642/1,664 MiB.
- Text baseline: 99 GPU layers and no drafter. Vision baseline: 56 GPU layers
  with GPU projector and no drafter.

These are admitted candidates, **not** proven optimal filled-context modes.
CPU-drafter screening was slower than the target-only short checks; a drafting
speedup must be measured rather than assumed.

The batch ran under the transient Linux user service
`mica-qwen-large-bench-20261006.service`, with a 16 GiB cgroup memory ceiling,
15 GiB worker-tree RSS stop, and GPU stop below 768 MiB free or above 15.5 GiB
total used. Only Mica's two services are paused; unrelated GPU processes are
left untouched. The runner restores previously active Mica services on normal
completion and handled interruption. A benchmark-only systemd `ExecStopPost`
hook also starts both previously active Mica services if the process is killed
before Python cleanup can run. The test unit has a 12-hour maximum runtime;
exhausting that deadline leaves unfinished cases unvalidated.

Exact requests, outputs, commands, timing, and memory observations are written
per case beneath `~/.mica/state/qwen27b-peak-20261006/`. The filenames begin
with `filled-`; `filled-budgets-summary.json` is produced when the batch ends.
Five cases completed before the batch was paused for the controlled context
sweep. The sixth case (text DFlash, 160K input / 64K output reservation) was
intentionally interrupted during prefill; this is not a quality failure. The
remaining cases are pending, not certified.

| Completed case | Actual input tokens | Generated tokens | Decode tokens/s | Outcome |
| --- | ---: | ---: | ---: | --- |
| Text baseline, 160K / 32K reservation, all GPU layers | 163,717 | 2,304 | 5.54 | Length/arithmetic smoke checks passed |
| Text DFlash, 160K / 32K reservation, 56 GPU layers | — | — | — | Prefill stopped at 758 MiB GPU free (below the 768 MiB guard) |
| Vision baseline, 160K / 32K reservation, 56 GPU layers | 163,479 | 2,304 | 3.60 | Length passed; first-line visual grounding check failed |
| Vision MTP, 160K / 32K reservation, 48 GPU layers, CPU projector | 163,479 | 2,304 | 3.30 | Length/arithmetic/first-line grounding smoke checks passed |
| Text baseline, 160K / 64K reservation, all GPU layers | 163,717 | 2,304 | 5.54 | Length/arithmetic smoke checks passed |

These are decode-only rates, excluding prefill. For example, the first text
case spent 589.77 seconds on prefill and 415.67 seconds decoding. The earlier
83.49 tokens/s DFlash result used only 124 input tokens, Q8 target/draft KV, and
a different placement/window. Comparing it directly with this populated-160K,
Q4-KV, no-drafter result does not isolate a single cause of slowdown. A smoke
pass is not a comprehensive correctness assessment of the generated module.

### Controlled filled-context sweep with prefix reuse

On October 6, `scripts/benchmark_context_sweep.py` was launched under
`mica-qwen-context-sweep-20261006.service`. It tests increasing actual prompt
budgets of 16,384, 32,768, 65,536, and 131,072 tokens, with 2,304 generated
tokens requested at each stage and a minimum of 2,000 required for the smoke
check. Each mode keeps a single worker alive throughout the ladder.

All stages reserve a fixed 139,264-token window, use Q4 target/draft KV,
batch 128 / microbatch 32, eight CPU threads, and disabled thinking. Text
baseline and DFlash both use 56 target GPU layers. Vision baseline and MTP
both use 48 target GPU layers and a CPU projector; drafting depth is seven
and the draft model stays on GPU. Matching target placement within each pair
controls an important confound, but these deliberately fixed placements are
**not** claimed to be per-budget fastest configurations. Some target layers
are on CPU throughout, rather than being progressively moved as input grows.

The task appears before the growing reference log so requests share a stable
prefix. Prior generated answers are never included in subsequent requests.
`cache_prompt` is enabled; reported `cache_n` and native logs establish how
much was actually reused. Qwen's recurrent-state checkpointing can restrict
reuse: enabling caching does not guarantee that every prefix token survives.
Separate baseline/draft workers do not share KV state. Per-request draft
toggling is disabled in this pinned native server, so pretending to switch
drafting off inside the same loaded worker would not be a valid baseline.

Requests, unabridged responses, per-stage timing/validation, mode commands,
and memory observations are saved to
`~/.mica/state/qwen27b-context-sweep-20261006/`. The shared benchmark lock
prevents concurrent workers from these two runners. Resource guards and
service recovery remain enabled. Each result is pending until its stage has a
saved response; neither cache reuse nor a speedup is assumed in advance.

Completed text sweep (fixed placement, not an all-GPU fastest-mode claim):

| Actual input tokens | Baseline decode tokens/s | DFlash decode tokens/s | Prefix tokens reused in both |
| ---: | ---: | ---: | ---: |
| 16,127 | 12.81 | 22.38 | 0 |
| 32,511 | 9.87 | 17.09 | 16,091 |
| 65,278 | 7.04 | 12.50 | 32,475 |
| 130,815 | 4.41 | 7.84 | 65,242 |

All eight responses generated 2,304 tokens and passed the length, input-budget
and no-thinking smoke criteria. Baseline minimum GPU headroom was 4,038 MiB;
DFlash minimum was 1,914 MiB. Prefix caching worked but did not make attention
to populated context free. No definitive kernel/CPU bottleneck diagnosis is
claimed without a profiler trace.

The vision baseline completed its 16K point at 9.46 tokens/s, with 2,304 output
tokens and a passing first-line grounding check. Its 32K point was deliberately
interrupted to prioritize the user's all-GPU profile tests; the remaining
partial-offload vision/MTP results are pending, not failures.

### Existing all-GPU workload sweep

The replacement service `mica-qwen-gpu-profile-sweep-20261006.service` tests
the existing GPU-only workload settings, rather than the earlier deliberately
partial-offload diagnostic placements. It uses the same increasing-prefix
prompts, disabled thinking and 2,304-token generation cap. Native trace logs
must confirm complete GPU-layer offload and no CPU KV allocation before a
case can produce an accepted GPU-only result; automatic fitting is disabled.
Host-side tokenization, process bookkeeping and mapped files still use RAM.
These are native engine timing tests with settings copied from the workloads,
**not** a new end-to-end proxy certification.

| Selected configuration | Workload | Total window | Input points (Ki tokens) |
| --- | --- | ---: | --- |
| Text baseline, no drafter | `qwen27b-modes-long` | 196,608 | 16, 32, 64, 128 |
| Text DFlash2, depth seven | `qwen27b-modes` | 114,688 | 16, 32, 64, 80 |
| Vision MTP, GPU projector, depth seven | `qwen27b-modes` | 65,536 | 16, 32, 48 |
| Vision baseline, GPU projector, no drafter | `qwen27b-vision-long-gpu` (experimental) | 196,608 | 16, 32, 64, 128, 160, if admitted by the native memory guard |

Each preserves Q4 target/draft KV, 512-token batches, 128-token microbatches,
two checkpoints, one slot, eight CPU threads and the profile's full output
reservation. DFlash's 128K point and vision MTP's 64K/128K points exceed their
unchanged workload input ceilings. They are recorded as outside the profile,
not assigned a fabricated speed or silently run with CPU layers.

The experimental 160K vision workload validates as YAML and was installed
without changing the active default workload. However, Mica's **estimated
VRAM reservation rejects it** under the current 15.5 GiB allocation. Its native
capacity test is a separate diagnostic with the same physical-memory guard;
even a native success would not establish that Mica can activate this workload
until its reservation policy is reconciled. The budget will not be silently
increased to make it pass.

Results are saved separately to
`~/.mica/state/qwen27b-gpu-profile-sweep-20261006/`, including the selected
workload YAML snapshots, exact native commands, unavailable ladder points,
full requests/responses, cache counts, decode timing and peak memory. Completed
earlier runs are preserved. All three existing all-GPU configurations completed
their permitted ladders on October 6. Each completed response generated 2,304
tokens, with prefill excluded from these decode rates:

| Actual text input tokens | Baseline tokens/s | DFlash2 tokens/s |
| ---: | ---: | ---: |
| 16,127 | 21.80 | 69.31 |
| 32,511 | 16.41 | 72.39 |
| 65,278 | 10.99 | 57.94 |
| 81,663 | Not tested | 53.47 |
| 130,815 | 6.62 | Outside the existing DFlash profile |

| Actual vision input tokens (including image/framing) | MTP tokens/s |
| ---: | ---: |
| 16,006 | 58.10 |
| 32,390 | 52.89 |
| 48,774 | 46.02 |

The initial stage has no reusable prefix. Text 32K, 64K and 128K stages reused
15,995, 32,379 and 65,146 prefix tokens, respectively; DFlash's 80K stage
also reused 65,146. Vision stages reused 15,874 and 32,258 tokens. All eleven
completed stages passed the output-length, input-budget and no-thinking smoke
criteria. All three vision stages passed the first-line fixture grounding
check. These checks do not certify every generated code block or long-context
reasoning task.

| Configuration | Verified GPU offload | Peak total GPU use (MiB) | Minimum GPU free (MiB) |
| --- | --- | ---: | ---: |
| Text baseline | Target 65/65, KV on CUDA | 14,310 | 1,530 |
| Text DFlash | Target 65/65, drafter 6/6, KV on CUDA | 14,896 | 944 |
| Vision MTP | Target 65/65, MTP 66/66, KV on CUDA; CLIP using CUDA0 | 14,748 | 1,092 |

Compared with the earlier partial-offload sweep, the all-GPU drafting profiles
are substantially faster. This comparison also changes prefill batch/microbatch
from 128/32 to the original profiles' 512/128, so it is not an isolated causal
measurement of CPU offload alone. Baseline-only decoding still slows with
populated context even when all model layers and KV remain on GPU.

The extra vision baseline at a 196,608-token total window **did not pass the
current safety policy**: after loading and a short arithmetic warmup, the guard
stopped it at 15,306 MiB GPU use / 534 MiB free, below the required 768 MiB
headroom. No filled 160K vision response was generated, so there is no valid
long-context decode speed to report. This is a safety-budget failure, not proof
of an absolute hardware impossibility or a model-quality failure. Mica's prior
estimated-reservation rejection remains consistent with keeping this workload
experimental and unvalidated for activation.

The sweep service ended normally and restored the original Mica server and
chat services. No experimental GPU workers remain; the default Spark worker
was observed running afterward. The local Mac inference server remains stopped.

## Artifact sources

- [GSQ-RCO target and BF16 projector](https://huggingface.co/ISTA-DASLab/Qwen3.8-27B-GSQ-RCO-GGUF)
- [DFlash2 Q4_K_M GGUF](https://huggingface.co/z-lab/Qwen3.8-27B-DFlash2-GGUF)
- [Unsloth Q4_0 MTP head](https://huggingface.co/unsloth/Qwen3.8-27B-GGUF/tree/main/MTP)
- [DFlash2 native implementation](https://github.com/ggml-org/llama.cpp/pull/27342)
- [Vision + DFlash limitation](https://github.com/ggml-org/llama.cpp/issues/27862)

File sizes, full SHA-256 hashes, immutable publisher revisions and the tested
minimum engine commit are in the three `config/model-manifests/qwen38-27b-*.yaml`
bundle records. The target header contains 64 decoder blocks and 16 full
attention layers; target F16 K/V is 65,536 bytes/token. The MTP bundle charges
69,632 bytes/token including its extra head. DFlash has sliding-window
attention plus additional draft state/workspace; its extra costs are covered
by the tested base reservation and conservative target-cache padding, not an
exact general-purpose formula for every possible drafter or context size.
