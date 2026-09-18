# MiniCPM-V 4.6 Thinking vLLM candidate generation on Apple M4

Date: 2026-09-17  
Host: Apple M4, 24 GiB unified memory  
Status: Q4/Q8 multimodal structural and batch smoke passed; production
calibration and Linux/NVIDIA vLLM validation pending

## Boundary

These tests use Transformers to reload compressed-tensors checkpoints. That
path decompresses weights and is useful for proving serialization and model
behavior, but its latency is not representative of native vLLM kernels. Every
process was limited to two compute threads and 16 GiB process-tree RSS.

## Protected components

Both candidates retain token embeddings, the tied output head, the complete
vision tower, the top-level multimodal merger, and every recurrent
linear-attention subtree at source precision. LLM Compressor requires subtree
patterns (`model.vision_tower.*`, `model.merger.*`, and
`model.language_model.layers.*.linear_attn.*`); exact parent names do not
protect descendants. The corrected policy selects 96 language-model linear
modules.

## Calibration smoke

The audited structural manifest contains one real image and one ordered
two-frame video. The processor uses one visual slice because Transformers
5.14.1 currently fails on unequal rectangular NaViT slice grids. Protected
vision modules run first to produce genuine multimodal embeddings, then GPTQ
calibrates the language layers. Q4 and Q8 both completed with group size 128.

| Candidate | Samples | Checkpoint | Conversion peak |
|---|---:|---:|---:|
| W4A16 | 1 image + 1 video | 2.0 GiB | not retained; the earlier watchdog could not read the sandboxed process tree |
| W8A16 | 1 image + 1 video | 2.2 GiB | 6.41 GiB |

The watchdog now fails closed after repeated process-accounting errors, so the
missing Q4 measurement cannot recur silently.

## Real batch inference

Each row is one Transformers call with two simultaneous media inputs and eight
new tokens per stream.

| Candidate | Input | Wall time | Peak RSS | Result |
|---|---|---:|---:|---|
| W4A16 | 2 images | 31.61 s | 3.66 GiB | two non-empty, identical completions |
| W4A16 | 2 two-frame videos | 49.88 s | 3.56 GiB | two non-empty, identical completions |
| W8A16 | 2 images | 31.91 s | 3.68 GiB | two non-empty, identical completions |
| W8A16 | 2 two-frame videos | 51.88 s | 3.68 GiB | two non-empty, identical completions |

The short completions begin `Got it, let's look at the`. This establishes real
pixel/frame ingestion and batched generation, not caption quality.

## Remaining gates

1. Replace the two-record fixture with the 512-record audited production set,
   including varied images, frame counts, resolutions, OCR, charts, and video.
2. Compare held-out Q4/Q8 outputs and task metrics with the source checkpoint.
3. Run native upstream vLLM image/video serving, continuous batching, memory,
   and throughput on Linux/NVIDIA.
4. Promote and publish only a candidate that passes every acceptance field.
