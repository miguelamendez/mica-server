# Multimodal researcher integration

Two separate models are registered:

| Model | Artifact | Components | Operation |
| --- | --- | --- | --- |
| Gemma 4 **12B** Unified QAT | Unsloth UD-Q4_K_XL | 6716.36 MB target, 175.12 MB BF16 projector, optional 253.71 MB MTP | Chat text generation from text/image/audio/video |
| EmbeddingGemma-2 | ggml-org Q8_0 | 309.86 MB text backbone, 554.82 MB multimodal projector | Normalized 768-dimensional embeddings |

These are artifact sizes, not measured total resident memory. Model manifests
record exact revisions, sizes and SHA-256 values. No 26B substitute is used.
The embedding projector includes both image and audio encoders despite the
legacy file-role name `vision-projector`. It must stay with its matching target.

The `local-researcher-gguf` workload uses a 14 GiB ceiling and `sequential`
residency: only one model worker at a time. It resolves a compatible native
engine from the model manifest rather than prescribing a new Python runtime.
Gemma starts with 4096 total tokens and speculation disabled. This initial
validation configuration is deliberately not a maximum-context certification.

```sh
./build/mica-server setup --config-dir config --profile local-researcher-gguf --ram-gib 14 --refresh
./build/mica-server start --config-dir config --workload local-researcher-gguf
```

EmbeddingGemma-2 requires llama.cpp commit
`4fbc76dec51d0add466f0210855c0596589b60d4` or a descendant; setup verifies this.
Its model card requires BF16 or F32 activations, not FP16. Q8 weight storage
does not imply FP16 activations. Native engine correctness must still be
validated with finite, normalized vectors and retrieval ranking.

## Reproducible tests

`scripts/researcher_acceptance.py` is a standard-library-only offline client.
It tests arithmetic, image OCR/colors, video colors, audio transcription,
three-item text embedding batches, image/video/audio embeddings, semantic
retrieval ranking, authentication and invalid-request rejection. It also
returns to Gemma after embedding requests, exercising sequential eviction in
both directions. Full responses, vectors and worker snapshots are saved after
each case, including failed cases.

```sh
python3 scripts/researcher_acceptance.py \
  --base-url http://127.0.0.1:8096 \
  --api-key-file "$HOME/.mica/secrets/api-key" \
  --image artifacts/test-assets/minicpm-scene.png \
  --video artifacts/test-assets/minicpm-sequence.mp4 \
  --audio /path/to/blue-sky-green-grass.wav \
  --output artifacts/researcher/acceptance.json
```

The audio fixture must say “The sky is blue and the grass is green.” The client
rejects empty audio, clips shorter than one second, and formats other than
mono 16 kHz PCM16 WAV. On macOS it can be synthesized with `say` and converted
with FFmpeg. Verify that synthesis actually produced audio: a header-only file
is not a valid inference fixture. The existing
video fixture shows red, green and blue. Correctness checks are smoke criteria,
not benchmark scores or a guarantee of arbitrary media understanding.

Results will distinguish failures from passing tests. Maximum input memory,
long-form generation, concurrency under multiple simultaneous requests and
MTP acceptance/speed remain separate experiments; never infer those from a
short successful smoke.

## Recorded validation

Native engine revision: `79e2e74eb11022c1ba2e438df7f0ca2d4c10f8b6`
(llama.cpp 0.6.0-dev). Apple M4 Metal, one worker at a time.

EmbeddingGemma-2 Q8 passed real inference through Mica:

| Request | End-to-end time | Check |
| --- | ---: | --- |
| Three text inputs in one request | 0.06 s | Three finite, normalized 768-dimensional vectors |
| Image | 0.26 s | One finite, normalized 768-dimensional vector |
| Six-second sampled video | 4.05 s | One finite, normalized 768-dimensional vector |
| 1.90-second spoken audio | 0.21 s | One finite, normalized 768-dimensional vector |

For the text retrieval smoke, relevant-document cosine was 0.8679 versus
0.5862 for an unrelated document. Authentication, empty-input rejection,
unsupported streaming/dimensions rejection and over-budget input rejection
also passed. [Full responses and vectors](../artifacts/researcher/embedding-spoken-acceptance.json)
are retained. These timings are short warm-worker smoke measurements, not
quality benchmarks or maximum-input peak-memory certification. Valid vectors
from image/audio/video alone do not establish semantic retrieval accuracy for
those modalities. Two earlier audio runs failed because the synthesized test
file contained no samples; their failed reports remain recorded separately.

The final combined run also passed all Gemma generation cases:

| Gemma request | End-to-end time | Observed answer |
| --- | ---: | --- |
| Text arithmetic | 0.66 s | `42` |
| Image OCR/colors | 4.15 s | `MICA 31415`, blue and red |
| Six-second sampled video | 24.46 s | Red → green → blue |
| Spoken audio transcription | 1.75 s | The sky is blue and the grass is green. |

This is baseline Q4 decoding with thinking disabled in the test requests and
MTP disabled. Short-response native decode timing was approximately 13 tokens/s;
that is not a sustained coding benchmark. Video prefill dominates its elapsed
time. Returning to Gemma after embedding requests took 18.18 s including model
loading and automatic inference warmup. Every successful inference asserted
exactly one resident worker of the expected model. The final report includes
fixture hashes, complete outputs/vectors, token timings and worker snapshots:
[final acceptance](../artifacts/researcher/full-acceptance-final.json).

The corrected native estimator accounts for Gemma's 1024-token sliding windows
plus micro-batch staging, as well as full-context global layers. At this
workload's 4K generation / 8K embedding budgets, estimated requirements including
safety are 9.13 GiB sequentially and 11.63 GiB all-resident. These are estimates,
not measured peak-memory guarantees; the live inference run validated sequential
residency only. Embedding's no-generation-KV layout does not eliminate attention
or media workspace.

Final validation: 88/88 regression tests and 77 schema documents passed. The
native build used one compiler job and peaked at 1.11 GiB process-tree RSS.
The final inference server process tree peaked at 7.57 GiB RSS under its
12-GiB watchdog. RSS is not the same metric as total Metal/unified allocation,
and polling is not an OS-enforced instantaneous hard cap.
Both model bundles were downloaded and checksum verified through Mica. No
Python inference engine was added. Maximum-context peaks, long-form quality,
live all/balanced residency, and Gemma MTP inference/speed are not certified by
these smoke checks. These commands use the source build and matching repository
configuration; an older installed binary/configuration pair must be updated
before advertising this workload as installed-release functionality.

### Startup regression and repair

The original researcher test incorrectly used the user's normal application
home. Its saved active workload then pointed to a researcher manifest absent
from the older installed distribution, causing `start` to fail with `map::at`.
Future validation setup must use a separate `--root`; explicit configuration
selection alone does not isolate runtime state or the saved active workload.

The repair updated the installed binary, model manifests, native engine contract
and schemas together, while preserving the existing workload catalog, API key
and machine policy. The normal coding workload was restored with a 14 GiB
allocation and port 8092. `/health`, `/ready`, authenticated Spark inference
(`17 + 25` → `42`) and repeated `start` with the same server PID all passed.
Missing workloads now produce a named, actionable installation/configuration
error instead of an opaque map exception; the lifecycle regression checks that
failure does not write a new runtime state.

## Sources

- [Google EmbeddingGemma-2](https://huggingface.co/google/embeddinggemma-2)
- [Official embedding GGUF conversion](https://huggingface.co/ggml-org/embeddinggemma-2-GGUF)
- [Native multimodal embedding implementation](https://github.com/ggml-org/llama.cpp/commit/4fbc76dec51d0add466f0210855c0596589b60d4)
- [Google Gemma 4 12B](https://huggingface.co/google/gemma-4-12B-it)
- [Unsloth Gemma 4 12B QAT](https://huggingface.co/unsloth/gemma-4-12B-it-qat-GGUF)
- [llama.cpp embedding API](https://github.com/ggml-org/llama.cpp/blob/master/tools/server/README.md#post-v1embeddings-openai-compatible-embeddings-api)
