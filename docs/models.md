# Models, quantization, and publishing

Mica separates a logical model from its engine-specific artifacts. One model
entry may describe MLX Q4/Q8 and GGUF Q4/Q8 variants, while an inference
profile chooses exactly one engine and artifact for a running worker.

An artifact is the complete immutable payload required for one model variant,
not necessarily one file or one repository. For example, a vision GGUF
artifact includes both the language weights and its matching `mmproj`. Other
artifacts may include an MTP or DFlash drafter, tokenizer, processor, codec, or
adapter files. Each component either inherits the artifact source or pins its
own repository and immutable revision.

## Curated model ledger

The built-in ledger records each public model's description, modality,
commercial-use license, source repository, immutable source revision,
available engines, artifact format, quantization type, artifact size, and
conservative memory reservation.

```sh
mica-server registry list
mica-server registry list --modality asr
mica-server registry list --modality video-text-to-text --backend mlx
mica-server registry ping --modality tts
```

`registry ping` checks remote repository availability without downloading
weights. The authenticated HTTP equivalent is `GET /v1/catalog`; the active
profile subset is `GET /v1/models`.

Model manifests may also include optional `references[]` entries, each with
`kind`, `title`, `url`, and an optional `description`. Supported kinds are
`paper`, `code`, `model-card`, `reproducibility`, and `documentation`.
References are validated and exposed by `registry list` and `/v1/catalog`;
they are informational links, not dependencies, download instructions, or
claims that Mica has reproduced a quantization pipeline. Pinned artifact
revisions, file hashes, compatibility, and reservations remain separate.
The GSQ manifest links both methods' papers and code, its pinned release card,
the IQ3_XXS tensor allocation, and the calibration importance matrix.

Internal route-validation fixtures are excluded from the public ledger and
normal profile listings.

## Context and generation limits

Model manifests use three flat, nullable values. Sources and training
disclosures belong in `references[]`; there are no per-task context options.

| Field | Meaning |
| --- | --- |
| `native_context_tokens` | Native total input-plus-output capacity. For Audio8 these are packed text/audio positions, not response-text tokens. Null when unknown or inapplicable. |
| `recommended_context_tokens` | One explicitly sourced recommended total window, no larger than native capacity. Null when no separate recommendation was verified. |
| `max_output_tokens` | A published supported generation ceiling or verified training-output limit, with provenance in references. Null when unpublished; recommendations, benchmark budgets, and examples do not establish this ceiling. |

The loader rejects zero/negative, fractional, boolean, or oversized limits,
and rejects recommendations/output ceilings above the native total. Workload
input plus reserved output must fit native capacity, unless the explicit
context-limit override is used. A published output ceiling remains enforced.
`setup --dry-run` warns when a workload's **total** exceeds a known recommended
window. Unknown values are not zero and do not impose an invented limit.

The model-manifest field `declared_context_tokens` and the previous nested
training/useful/output claims are no longer accepted in the active model
schema. Upgrade the binary and packaged manifests together. The separate
external-model shorthand in workload files still uses its existing
`declared_context_tokens` contract; it is not a model manifest.

### Upstream audit: 2026-10-07

All 15 packaged manifests were reviewed, including the hidden control and
the four component-specific Qwen27B bundles. No workload budgets or memory
reservations were increased. These are publisher/configuration observations,
not certification that maximum context fits a machine or retains quality.

| Model(s) | Native total | Training or publisher generation evidence |
| --- | ---: | --- |
| [Spark-X2.5-4B](https://huggingface.co/XHToken/Spark-X2.5-4B) | 1,048,576 | Publisher reports long-context training reaching 1M tokens. The 131,072 output request is an example, not a maximum. |
| [Ling-3.0-Tiny](https://huggingface.co/inclusionAI/Ling-3.0-tiny) | 131,072 | Publisher benchmark uses 32K output with a 256K **YaRN extension**; exact training lengths are unverified. |
| [Qwen3.5-4B](https://huggingface.co/Qwen/Qwen3.5-4B) and [9B](https://huggingface.co/Qwen/Qwen3.5-9B) | 262,144 | General recommendation is 32,768 generated tokens, not a hard or trained-output ceiling. Exact training lengths are unverified. |
| [Qwen3.8-27B](https://huggingface.co/Qwen/Qwen3.8-27B), GSQ-RCO / text / DFlash / vision-MTP | 262,144 | Base architecture capacity is inherited. Large separate reasoning/final-output recommendations assume extended 1M context; they are not defaults for the native bundles. Training lengths and a hard output maximum are unverified. |
| [Ternary Bonsai 2 27B](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf) | 262,144 | Unchanged Qwen3.8 architecture according to publisher. CLI example requests 16,384 generated tokens, not a hard ceiling. Training lengths are unverified. |
| [MiniCPM-V 4.6 Thinking](https://huggingface.co/openbmb/MiniCPM-V-4.6-Thinking) | 262,144 | Architecture positions verified. Image/video examples request 512/2,048 generated tokens; neither establishes a ceiling or training length. |
| [Xing4.0-29B-A4B](https://huggingface.co/XingChen-AGI/Xing4.0-29B-A4B) | 262,144 | Native 256K, extensible to 512K. Output budgets vary by benchmark; no single general recommendation or exact training-length ceiling was verified. |
| [Qwen3 0.6B control](https://huggingface.co/Qwen/Qwen3-0.6B) (hidden) | 40,960 config positions | Publisher lists 32,768 context, retained as the conservative recommended total. Its general output recommendation cannot be reserved in full alongside nonempty input at that total. |
| [Audio8](https://huggingface.co/Edge0/Audio8-TTS-Preview-0.6b) | 2,048 packed text/audio positions | Text, reference audio, and acoustic output share capacity. Generation-config default 512 is not a maximum. Canonical upstream moved from Audio8 to Edge0; artifact pins are unchanged. |
| [Granite Speech TurboCTC](https://huggingface.co/ibm-granite/granite-speech-5.0-470m-turboctc) | Not applicable | Non-autoregressive CTC ASR. The 16,384 BPE units are vocabulary size, not context. Training uses about 60,000 hours of English audio. |
| [Nemotron 3 Diarization](https://huggingface.co/nvidia/Nemotron-3-Diarization) | Not applicable | Audio-frame speaker classification. Publisher allows recordings without fixed duration when chunked; eight speaker channels are not tokens. Adapter and memory constraints still apply. |

Except for the control's documented 32,768 total window, separate recommended
totals remain `null`. All independent hard output ceilings remain `null` in
this audit. References preserve guidance without mislabeling it as a maximum.
For any chosen output reserve, available input is `total - reserve`, including
system prompts, history, tool definitions and media positions. Reasoning and
visible response share the generated-output budget. Subtraction does not
create a training-backed input recommendation.

Reproduce the metadata-only check with:

```sh
uv run --with pyyaml scripts/audit_model_limits.py --output audit.json
```

The [audit report](../artifacts/model-limits-audit-2026-10-07.json) records source
URLs, configuration hashes, reference descriptions, architecture comparisons,
and generation defaults explicitly labeled as **not hard limits**. GGUF-only
repositories are compared with an explicitly referenced base configuration;
this is not a check of every tensor or the cached GGUF header. No weights are
downloaded, no inference is run, and no server is restarted by this audit.

## Nemotron 3 diarization

`nemotron-3-diarization` is a 99.3M-parameter speaker diarization model, not ASR.
It labels who spoke when with anonymous speaker IDs; it does not generate a
transcript or identify a person by name.

| Artifact | Engine | Weight bytes | Public Mica operation |
| --- | --- | ---: | --- |
| [MLX Q8](https://huggingface.co/mlx-community/Nemotron-3-Diarization-8bit) | `mlx-audio-diarization` (mlx-audio library + small HTTP adapter) | 106,613,900 | `audio.diarize` |
| [GGUF Q8 mixed](https://huggingface.co/audio-cpp/Nemotron-3-Diarization-GGUF) | `audio-cpp`, not llama.cpp | 106,675,136 | `audio.diarize` |
| GGUF BF16, same repository | `audio-cpp` | 198,720,928 | `audio.diarize` |

No Q4 artifact is registered because neither referenced repository publishes
one. GGUF Q8 mixes precisions and can change boundaries/turn counts; its
publisher recommends BF16 when preserving original outputs matters. That is
an upstream claim, not a local DER measurement. The license is
[OpenMDW-1.1](https://openmdw.ai/license/1-1/), which allows commercial use but
is neither MIT nor Apache. Retain its license and notices when redistributing.

The public route is `POST /v1/audio/diarizations`. Native audio.cpp's plain
transcriptions route requires transcript text, so Mica calls
`/v1/audio/transcriptions/details` and converts sample offsets to seconds.
The MLX library has no standard diarization HTTP route; Mica supplies one.
Streaming/library feed and native batch/live support exist upstream, but this
Mica adapter only advertises the offline endpoint. Neither streaming nor
vLLM diarization is certified by this integration.

Start with `mica-server setup --profile diarization --ram-gib 3`, then
`mica-server serve`. The generic profile uses installed-engine preference;
pin `engine: audio-cpp` and `artifact: {id: bf16}` to select native BF16.
For reproducible API tests, use `scripts/smoke_diarization.py`. This verifies
nonempty turns, timestamp bounds, and output shape, not labeled quality.

Local smoke measurements on Apple M4 / 24 GiB unified memory (2026-10-05),
using one 28-second, 16 kHz mono recording through the warmed public endpoint:

| Precision/engine | Request wall time | Real-time factor | Returned turns / speaker IDs |
| --- | ---: | ---: | ---: |
| MLX Q8 | 0.540 s | 0.0193 | 7 / 4 |
| audio.cpp Metal Q8 | 0.156 s | 0.00555 | 6 / 4 |

These are single-request integration observations, including HTTP overhead,
not medians or standardized quality benchmarks. Boundaries and turn splitting
are not identical between engines. BF16 is registered but not locally tested.

## Artifact bundles

### GSQ-RCO comparison candidate

`qwen38-27b-gsq-rco` registers the publisher's IQ3_XXS GGUF (10,094,357,632
bytes) and BF16 projector (931,146,528 bytes), both pinned to revision
`d562806dbafae37109975e970aae91b43e73b440` with per-file SHA-256 checks.
It uses the existing `llama-cpp` engine, not Bonsai's specialized Prism fork.
The source [model card](https://huggingface.co/ISTA-DASLab/Qwen3.8-27B-GSQ-RCO-GGUF)
declares Apache-2.0 and describes per-tensor mixed-precision allocation.

The [comparison workload](../config/workloads/qwen38-gsq-bonsai-comparison.yaml)
keeps one worker resident at a time and uses matching 8192-token contexts,
Q8 KV cache, and a 16-GiB machine allocation. Its 13-GiB GSQ reservation is a
planning allowance, not measured peak memory. This is a comparison candidate,
not a replacement for the default coding workload. MTP variants are not part
of this test. Publisher benchmark results from different evaluation protocols
do not establish a controlled GSQ-versus-Bonsai quality ranking.

Local validation downloaded and SHA-256-verified this IQ3_XXS bundle and
completed real text inference through Mica. Longer runs triggered macOS
memory-pressure warnings and were stopped; this is **not** certified for
long-context use within 16 GiB. The default coding workload is unchanged.
See the [local comparison record](validation/manifest-audit.md#gsq-rco-versus-bonsai-local-comparison)
for timings, raw outputs, and the incomplete tests.

### Bundle contents

Every artifact belongs to one logical model and records:

- immutable repository and revision;
- container/layout format and exact quantization or packing;
- every required file with a semantic role;
- byte size and SHA-256 where a single-file checksum is available;
- conservative resident-memory reservation;
- compatible engine IDs and required engine features;
- a minimum native engine commit when architecture support was introduced;
- validation and provenance evidence.

Semantic file roles include `model`, `vision-projector`,
`mtp-drafter`, `dflash-drafter`, `tokenizer`, `processor`, `codec`, and
`adapter`. The role tells the engine adapter how a component is used; its
format tells the downloader and validator how it is stored. An MTP or DFlash
drafter can therefore be sourced from a different Hugging Face repository in
the same way that an `mmproj` can be sourced separately from its primary GGUF.

An artifact-level `repository` and `revision` are defaults. A file may override
them with a `source` block:

```yaml
artifacts:
  q8_with_mtp:
    repository: mica-ai/spark-x2.5-4b-gguf
    revision: <immutable-primary-commit>
    format: gguf
    quantization: q8_0
    files:
      - role: model
        path: spark-x2.5-4b-q8_0.gguf
        size_bytes: <exact-size>
        sha256: <sha256>
      - role: mtp-drafter
        source:
          repository: upstream-or-mica/spark-x2.5-mtp
          revision: <immutable-drafter-commit>
          license: apache-2.0
        path: spark-x2.5-mtp-q8_0.gguf
        size_bytes: <exact-size>
        sha256: <sha256>
    compatibility:
      target_model: spark-x25-4b
      target_revision: <compatible-target-commit>
      tokenizer_sha256: <tokenizer-fingerprint>
      vocabulary_size: <exact-vocabulary-size>
      drafter_protocol: mtp
      engines: [llama-cpp]
      required_features: [mtp-speculative-decoding]
```

The example is a design sketch, not an available Spark drafter. Before an
MTP/DFlash variant can run, its manifest and engine adapter must validate the
target model revision, tokenizer fingerprint, vocabulary, architecture, and
drafter protocol. The current runtime rejects drafter launch explicitly.

The active [Bonsai model manifest](../config/model-manifests/ternary-bonsai-2-27b.yaml)
illustrates a multi-file artifact:

```yaml
schema: 2
id: ternary-bonsai-2-27b
description: Ternary Bonsai for local coding, chat and visual document analysis with the Prism runtime.
supported_tasks: [chat, coding, ocr, visual_question_answering, structured_extraction]
input_modalities: [text, image]
output_modalities: [text]
abilities: [text_generation, instruction_following, reasoning, image_understanding]
supported_interactions:
  - {operation: chat.generate, required_inputs: [text], outputs: [text]}
  - {operation: chat.generate, required_inputs: [text, image], outputs: [text]}
license: apache-2.0
artifacts:
  - id: pq2_0
    compatible_engines: [prism-llama-cpp]
    repository: prism-ml/Ternary-Bonsai-2-27B-gguf
    revision: 6ed5e12bf84b7a63069882c91dd9e9218647d17b
    format: gguf
    quantization_type: PQ2_0
    reservation_gib: 12
    files:
      - role: model
        path: Ternary-Bonsai-2-27B-PQ2_0.gguf
        size_bytes: 7206168928
        sha256: 3907dc1658db1f78a9826bf8d5bcb8dc65db0d466388937af57f2294fae62ec1
      - role: vision-projector
        path: Ternary-Bonsai-2-27B-mmproj-BF16.gguf
        size_bytes: 931145856
        sha256: e287342d92332fa3577ed1d42e921dac9370c08da58ba9337fa450f6cc76cfd7
    required_compatibility: [prism-activation-transform, vision-projector]
```

For GGUF file bundles, Mica stages every file, checks its declared byte size
and SHA-256, and writes the completion marker only after all files pass. A
missing or invalid required projector is not an installed artifact. Each file
can pin a separate Hugging Face repository and revision. MLX directory
artifacts currently use their pinned repository revision and the existing
single-directory downloader; cross-repository MLX components are rejected
until an adapter can validate and launch them.

Required and optional components are distinct. A projector required for a VLM
belongs to every runnable visual variant. A speculative drafter may instead be
represented by a separate accelerated variant, such as `q8` and
`q8_with_mtp`, so the base artifact remains loadable when the selected engine
does not implement that speculative-decoding protocol.

An engine's supported formats are only the first compatibility gate. Stock and
Prism llama.cpp both read GGUF, but only Prism implements the transform required
by Bonsai. Artifact requirements and engine features must match before setup or
loading. See [System, engine, model, and inference
profiles](engines-and-profiles.md).

## Curated assistant models

Model manifests also declare intended uses through `supported_tasks`, separately
from modalities and abilities. Find models with `mica-server registry list --task
coding` or search tasks in the TUI. These labels do not promise benchmark quality
or enable endpoints. The selected model interaction and engine's implemented
operation determine routing. See the [task index](schema-vocabulary.md#descriptions-and-task-index).

| Model | Modality | MLX | GGUF | License |
| --- | --- | --- | --- | --- |
| Spark-X2.5-4B | Text generation | Q4, Q8 | Q4_K_M, Q8_0 | Apache-2.0 |
| Granite Speech 5.0 470M TurboCTC | ASR | Q4, Q8 | Q4_K, Q8_0 | Apache-2.0 |
| Audio8 TTS Preview 0.6B | TTS and voice cloning | Q4, Q8 | Q4_0, Q8_0 | Apache-2.0 |
| MiniCPM-V 4.6 Thinking | Image/video to text | Q4, Q8 | Q4_K_M, Q8_0 | Apache-2.0 |
| Ternary Bonsai 2 27B | Image/text to text | — | PQ2_0 + BF16 projector (Prism fork) | Apache-2.0 |
| [Ling 3.0 Tiny](model-cards/ling-3-tiny.md) | Text reasoning/coding | Community Q4 requires an unregistered `rapid-mlx` engine | Publisher Q4_K_M, Q8_0 on current llama.cpp; inference pending | MIT |

Full provenance, context/training limits, protected layers, quality findings,
and benchmark links live in [model cards](model-cards/).

## Add a model from Hugging Face

Supported modalities are `tts`, `asr`, `text-to-text`, and visual aliases
including `img-text-to-text`, `image-text-to-text`, and
`video-text-to-text`.

```sh
mica-server add-model \
  --url https://huggingface.co/owner/model \
  --modality text-to-text \
  --id my-model \
  --description "Short catalog description"
```

Registration rejects unknown or non-commercial licenses. The current
allowlist is Apache-2.0, MIT, BSD-2-Clause, BSD-3-Clause, BSD, and ISC. Custom
definitions are stored in `~/.mica/config/custom-models.json`.

A shareable schema-5 YAML workload profile can reference another Hugging Face model directly,
but must pin an immutable revision and declare its license, modality, engine,
artifact path/format, context limits, and memory reservation. Remote code is
not trusted. See [Profiles](profiles.md).

## Quantize

Register and convert in one operation:

```sh
mica-server add-model \
  --url https://huggingface.co/owner/model \
  --modality image-text-to-text \
  --backend mlx \
  --quant q4
```

Or quantize an existing model explicitly:

```sh
mica-server quantize --model spark-x25-4b --backend mlx --quant q8
mica-server quantize --model spark-x25-4b --backend gguf --quant q4
```

MLX uses affine Q4/Q8 conversion and model-specific protection profiles for
audio/vision components whose quantization caused quality loss. GGUF uses the
family-specific converter and quant type recorded in the model card. Original
full-precision downloads remain in `~/.mica/staging` until conversion and real
smoke inference succeed.

Artifact variant IDs are extensible strings. The conversion command currently
produces only `q4` and `q8`; exact upstream packings such as Bonsai's `pq2_0`
are downloaded from an immutable revision and retain their exact packing name,
size, and checksums.

vLLM candidates require calibration data, a supported accelerator, and native
quality validation before promotion. See [vLLM quantization](vllm-quantization.md).

## Publish validated artifacts

Publication creates one Hugging Face repository per logical model, keeping all
MLX and GGUF quantizations in that repository rather than creating one repo per
format.

Authenticate interactively:

```sh
"$HOME/.mica/environments/tools/bin/hf" auth login
```

Review, publish, build modality collections, and verify:

```sh
python3 scripts/publish_huggingface.py --dry-run
python3 scripts/publish_huggingface.py

"$HOME/.mica/environments/tools/bin/python" \
  scripts/publish_huggingface_collections.py
"$HOME/.mica/environments/tools/bin/python" \
  scripts/verify_huggingface_publish.py
```

Log out after using a temporary token:

```sh
"$HOME/.mica/environments/tools/bin/hf" auth logout
```

The publisher refuses artifacts that are not marked validated in
[`config/huggingface_publish.json`](../config/huggingface_publish.json), uploads
one artifact at a time, and never prints the token. Current intended repos are:

- `miguelamendez/mica-spark-x25-4b`
- `miguelamendez/mica-granite-speech-5`
- `miguelamendez/mica-audio8-tts-06b`
- `miguelamendez/mica-minicpm-v46-thinking`

Do not claim an upload is complete until remote hashes and model-card metadata
have been independently verified.
