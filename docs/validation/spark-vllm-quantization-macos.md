# Spark vLLM candidate generation on Apple M4

Date: 2026-09-17  
Host: Apple M4, 24 GiB unified memory  
Status: GPTQ Q4/Q8 structural and batch smoke passed; production calibration
and Linux/NVIDIA vLLM validation pending

## Resource boundary

Every conversion and inference run used two compute threads and a 16-GiB
process-tree RSS watchdog. The temporary checkpoints were reloaded through
Transformers, which decompresses packed weights; these timings are not native
vLLM performance.

## Algorithm selection

AutoRound 0.14.2 cannot calibrate this Spark revision. Its block pipeline gives
Spark's custom attention a three-dimensional mask where the architecture
requires four dimensions. The failed run peaked at 8.36 GiB and produced no
artifact. GPTQ is therefore the active Q4 and Q8 path.

Spark uses its checkpoint-compatible isolated toolchain: Transformers 4.57.1,
LLM Compressor 0.11.0, compressed-tensors 0.16.0, PyTorch 2.11.0, and datasets
4.8.5. Explicit CPU selection also disables LLM Compressor's otherwise
incorrect MPS sequential-onload choice.

## Structural matrix

Both group-128 candidates calibrated all 36 decoder layers from two local chat
records. The embeddings and tied `lm_head` remained at source precision. The
local JSONL path is supported directly and is hashed into future candidate
manifests, so offline structural checks do not require a large dataset fetch.

| Candidate | Checkpoint | Conversion peak | Batch-2 generation | Inference peak |
|---|---:|---:|---:|---:|
| W4A16 | 2.4 GiB | 11.14 GiB | 16 tokens/stream in 15.50 s | 10.71 GiB |
| W8A16 | 4.2 GiB | 9.75 GiB | 16 tokens/stream in 15.90 s | 12.14 GiB |

Both batch streams produced identical coherent thinking preambles for `Return
exactly: MICA_BATCH_OK`, but neither reached the requested literal answer in 16
tokens. This proves load, serialization, and batching only. Q4 also showed much
higher per-layer calibration error than Q8 on the tiny fixture.

## Remaining gates

1. Run the configured 512-sample, 2,048-token production calibration on the
   deferred CUDA host.
2. Compare held-out quality, perplexity/KL, and long-context behavior against
   BF16.
3. Add a maintained Spark architecture adapter to upstream vLLM and run native
   serving, continuous batching, throughput, and memory tests.
4. Keep both candidates unpublishable until every acceptance field passes.
