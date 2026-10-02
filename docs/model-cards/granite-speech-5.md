---
language:
- en
library_name: transformers
pipeline_tag: automatic-speech-recognition
license: apache-2.0
base_model: ibm-granite/granite-speech-5.0-470m-turboctc
tags:
- mica-server
- automatic-speech-recognition
- audio
- mlx
- gguf
- commercial-use
mica:
  schema: 1
  model_id: granite-speech-5
  mica_server_repo: https://github.com/miguelamendez/mica-server
  curated_repo: https://huggingface.co/miguelamendez/mica-granite-speech-5
  source_repo: ibm-granite/granite-speech-5.0-470m-turboctc
  source_revision: 6c14d3d052a602d850f1bc4aa017f25f4adf6aa0
  parameters: 470000000
  context:
    unit: audio
    architecture_max_sequence: null
    training_max_sequence: null
    model_max_output: null
    semantics: ctc-non-autoregressive
  reasoning:
    supported: false
---

# Granite Speech 5.0 470M TurboCTC — Mica Q4/Q8

Mica runtime artifacts for
[`ibm-granite/granite-speech-5.0-470m-turboctc`](https://huggingface.co/ibm-granite/granite-speech-5.0-470m-turboctc),
pinned to revision `6c14d3d052a602d850f1bc4aa017f25f4adf6aa0`.
The selected checkpoint is Apache-2.0; it is not the separate non-commercial
`-nc` checkpoint.

The runtime configuration, conversion policy, and validation evidence are
maintained in the [Mica Server repository](https://github.com/miguelamendez/mica-server).
Validated artifacts are published together in the
[`miguelamendez/mica-granite-speech-5`](https://huggingface.co/miguelamendez/mica-granite-speech-5)
repository, with manifests recording their source, size, and digest.

## Model facts and provenance

Granite Speech is a 470M-parameter English CTC ASR model. It has 16 conformer
blocks, 1,024 hidden dimensions, eight attention heads, 128-frame block
attention, temporal subsampling from 100 Hz to 12.5 Hz, and a 16,384-unit BPE
output head. Greedy CTC decoding is non-autoregressive.

Upstream reports approximately 60,000 hours of English training audio from
public datasets plus disclosed synthetic mixtures. It does not publish a single
maximum training utterance duration or supported input-audio duration. The
encoder configuration's 128-frame attention block and 512 positional field are
architecture internals, not a claim that audio must end at either value.

Token context, generated-output limits, KV-cache precision, and thinking levels
are therefore not applicable. Granite profiles use maximum audio duration,
chunk duration, overlap, batch size, and measured workspace instead.

## Mica artifacts and protected layers

```text
mlx/q4/    selective MLX affine Q4, group size 64
mlx/q8/    selective MLX affine Q8, group size 64
gguf/      Q4_K and Q8_0
vllm/      not published; structural candidates failed the production gate
```

The MLX conversion retains these modules at source precision:

- `encoder.input_linear`, the sole learned acoustic-feature input boundary;
- `encoder.out`, shared by midpoint and final CTC vocabulary logits;
- `encoder.out_mid`, which feeds midpoint CTC predictions back into the encoder.
- `ctc_head`, the tied public output head required by the Transformers loader.

Uniform Q4 corrupted the controlled transcript. Selective Q4 and Q8 matched the
BF16 transcript exactly on the fixed 8.608-second quality clip.

GPTQ W4A16 and W8A16 group-128 structural candidates now calibrate from real
audio manifests. Both reload and match the BF16 fixture transcript in batch-2
Transformers inference. They remain unpublishable: the calibration set has one
record, and vLLM core still has no native `granite_speech5_ctc` transcription
loader.

## Mica validation boundary

On an Apple M4, selective MLX Q4 and Q8 passed real HTTP transcription. Five
warm runs after warmup had median model times of 0.0637 and 0.0635 seconds for a
3.505-second clip, about 135x real time. GGUF Q4_K and Q8_0 also passed real
Metal inference. These short-clip results do not certify unlimited streaming
duration, multi-hour files, or concurrent batching.

Validation records:

- `docs/validation/granite-speech-5-mlx-q4.md`
- `docs/validation/granite-speech-5-mlx-q8.md`
- `docs/validation/gguf-runtime-macos-metal.md`
- `docs/validation/granite-vllm-quantization-macos.md`

## Profile validation requirements

A Granite profile must define `max_audio_seconds`, `chunk_seconds`, overlap,
maximum concurrent streams, and a certified memory peak. Values beyond the
longest Mica-tested clip warn in experimental mode and fail certified mode.
