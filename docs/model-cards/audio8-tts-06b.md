---
language:
- yue
- zh
- nl
- en
- fr
- de
- it
- ja
- ko
- pl
- es
library_name: transformers
pipeline_tag: text-to-speech
license: apache-2.0
base_model: Audio8/Audio8-TTS-Preview-0.6b
tags:
- mica-server
- text-to-speech
- voice-cloning
- mlx
- gguf
- commercial-use
mica:
  schema: 1
  model_id: audio8-tts-06b
  mica_server_repo: https://github.com/miguelamendez/mica-server
  curated_repo: https://huggingface.co/miguelamendez/mica-audio8-tts-06b
  source_repo: Audio8/Audio8-TTS-Preview-0.6b
  source_revision: f07040f3d151f1ba0253bfb92cb2f5dd38b44594
  parameters: 601159424
  context:
    unit: packed-text-audio-positions
    architecture_max_sequence: 2048
    training_max_sequence: null
    training_max_input: null
    model_max_output: null
    checkpoint_default_output: 512
    semantics: packed-input-plus-generated-audio
  reasoning:
    supported: false
---

# Audio8 TTS Preview 0.6B — Mica Q4/Q8

Mica runtime artifacts for
[`Audio8/Audio8-TTS-Preview-0.6b`](https://huggingface.co/Audio8/Audio8-TTS-Preview-0.6b),
pinned to revision `f07040f3d151f1ba0253bfb92cb2f5dd38b44594`.
The model and derivatives are Apache-2.0 licensed.

The runtime configuration, conversion policy, and validation evidence are
maintained in the [Mica Server repository](https://github.com/miguelamendez/mica-server).
Validated artifacts are published together in the
[`miguelamendez/mica-audio8-tts-06b`](https://huggingface.co/miguelamendez/mica-audio8-tts-06b)
repository, with manifests recording their source, size, and digest.

## Model facts and provenance

Audio8 is a 601,159,424-parameter DualAR TTS model, excluding its bundled
44.1-kHz codec. A 24-layer slow autoregressive transformer predicts semantic
audio frames and a four-layer fast transformer predicts ten codec codebooks.
It supports generation with no reference and zero-shot voice cloning.

The architecture supports up to 2,048 packed text/audio positions. The pinned
checkpoint's generation configuration defaults to 512 new tokens, while the
upstream example requests 1,024. Neither value is documented as a separate
hard maximum output length. Training datasets, maximum training sequence, and
the input/output length distribution are not disclosed.

The upstream preview recommends Cantonese, Chinese, Dutch, English, French,
German, Italian, Japanese, Korean, Polish, and Spanish. A voice-cloning
reference transcript must match the reference audio.

## Mica artifacts and protected layers

```text
mlx/q4/    selective MLX affine Q4, group size 64
mlx/q8/    selective MLX affine Q8, group size 64
gguf/      Q4_0 and Q8_0
vllm/      not published; structural candidates failed the production gate
```

The MLX conversion retains the semantic/text embeddings, acoustic codebook
embeddings, fast-decoder input/output boundaries, and codec at source
precision. These tensors directly cross discrete text, semantic, codebook, and
waveform boundaries.

GPTQ W4A16 and W8A16 group-128 structural candidates now exercise plain and
voice-reference prompts. The embeddings, codebook embeddings, fast-decoder
boundaries, and waveform decoder remain at source precision. Both candidates
reload and generate batch-2 WAV output, but the two-sample Hessians fall back
to round-to-nearest in many layers. A full calibration and native vLLM-Omni
quality run are mandatory before promotion.

For GGUF, Q4_0 was selected instead of Q4_K because Q4_K block alignment left
too many codec matrices unquantized, produced a larger resident model, and had
no speed advantage.

## Mica validation boundary

GGUF Q4_0 and Q8_0 passed seeded plain speech, zero-shot voice cloning, and
exact Granite ASR round-trip on the Apple M4. Q4 is an accepted memory/quality
compromise; Q8 was preferred in listening validation. The MLX peak-memory
investigation and standardized batch suite remain open, so no MLX batch or hard
memory certification is claimed here.

A matched, unseeded MLX long-form generation on the Apple M4 measured the first
and final five seconds of each waveform. Q4 declined from -18.7 dB mean volume
to -27.7 dB (9.0 dB) and compressed the passage into 23.78 seconds. Q8 declined
from -18.3 dB to -21.0 dB (2.7 dB) and produced 34.04 seconds. Because the
endpoint does not expose Audio8's sampling seed, this is a controlled prompt
comparison rather than a deterministic multi-seed evaluation. Mica therefore
defaults interactive TTS to Q8 and presents Q4 as a memory/quality tradeoff.

Validation records:

- `docs/validation/gguf-runtime-macos-metal.md`
- `docs/validation/audio8-vllm-quantization-macos.md`
- `artifacts/benchmarks/audio8-mlx-long-form-volume.json`

## Profile validation requirements

Audio8 profiles must state maximum packed positions, requested semantic-token
output, reference-audio duration, concurrent utterances, and whether cloning is
enabled. The proxy must account for the slow AR cache, fast AR fixed cache,
codec workspace, and reference encoder rather than applying text-LLM KV flags.
Thinking settings are invalid for this model.
