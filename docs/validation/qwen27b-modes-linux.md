# Qwen 27B modes on a 16 GiB NVIDIA GPU

These experimental workloads reuse the **same** pinned GSQ-RCO IQ3_XXS target
weights. Bundle IDs select its dependencies; they are not separately trained
models. The DFlash and MTP heads are independently pinned to their publisher
repositories. Nothing was requantized in this experiment.

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

The full-window partial-CPU filled-context validation was still pending when
this initial report was written. Its successful short request is not enough
to certify a filled 256K window. All long-output reservations still need a
real extended-output stress test.

## Quality and prior calibration caveats

The combined image prompt identified the blue/red squares but did not
transcribe the printed label. Initial greedy coding outputs also differed
between target-only and speculative runs. Therefore the speed measurements
do **not** establish lossless decoding or unchanged vision/coding quality.
Follow-up strict-answer checks and an OCR-only prompt are tracked separately.

An existing user `qwen` launcher and its `profiles.ini` were found on the Linux
host. That launcher previously used a different IQ3_S target with embedded
MTP, not this GSQ-RCO IQ3_XXS file. Its saved cache calibration reported PPL
5.7920 for Q8/Q8, 5.8078 for Q4/Q4 and 5.8005 for Q8/Q4, with approximately
0.068 reported uncertainty. Q4/Q4 was about 0.27% higher, within those error
bands. This is useful historical evidence, **not** current-target calibration
and not evidence that Q4 is lossless.

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
69,632 bytes/token including its extra head. DFlash's bounded sliding-window
state is covered by its separate base reservation rather than multiplying
every drafter layer by the target's entire context window.
