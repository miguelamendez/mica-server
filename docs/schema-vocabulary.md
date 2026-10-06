# Model, engine, and workload schema vocabulary

The active packaged-manifest schemas are
[`vocabulary-v1`](../schemas/vocabulary-v1.schema.json),
[`model-v2`](../schemas/model-v2.schema.json),
[`engine-v2`](../schemas/engine-v2.schema.json), and
[`workload-v5`](../schemas/workload-v5.schema.json). The packaged YAMLs and loader now
use these versions. An identifier in the shared vocabulary is **not** a claim
that Mica implements or has tested it. New model/engine pairs still need a
real adapter and inference smoke test before publication as loadable.

Optional model `references[]` records use `kind` (`paper`, `code`,
`model-card`, `reproducibility`, or `documentation`), `title`, an HTTP(S)
`url`, and optional `description`. They preserve research and provenance
links in the model ledger without altering runtime selection or downloads.

| Layer | Controlled values | Meaning |
| --- | --- | --- |
| Modality | `text`, `image`, `video`, `audio` | Physical input/output data. An interaction lists required and optional inputs separately. |
| Model ability | `text_generation`, `instruction_following`, `tool_calling`, `reasoning`, `structured_output`, `image_understanding`, `video_understanding`, `audio_understanding`, `speech_recognition`, `speech_translation`, `speaker_diarization`, `speech_synthesis`, `voice_conditioning`, `general_audio_generation`, `music_generation`, `audio_transformation`, `image_generation`, `image_editing`, `video_generation`, `video_editing` | Properties of the weights, backed by a model card or validation. Coding and creativity are evaluation domains, not binary abilities. |
| Operation | `text.generate`, `chat.generate`, `decisions.score`, `audio.transcribe`, `audio.translate`, `audio.diarize`, `audio.synthesize_speech`, `audio.generate`, `audio.transform`, `image.generate`, `image.edit`, `video.generate`, `video.edit` | A normalized callable contract. The engine must expose a real adapter call for it. Agent execution is a composition of operations, not a model operation. |
| Engine interface | `endpoint_contracts[]` with operation, transport, adapter call, input/output shape, and optional streaming/tools flags | Describes **actual callable surfaces**, not hypothetical features inferred from the model type. Current adapters support HTTP POST only; unsupported transports and calls are rejected. |
| Artifact compatibility | `compatible_engines[]`, `required_compatibility[]` | Loadability constraints such as architecture support or a vision projector. They do not imply an API operation. |
| Workload profile | `models[]`, optional `selection.defaults[]` | A collection of model IDs and per-model policy. No fixed groups or roles. Priority selects among eligible models unless the caller specifies a model or an operation/input-specific default is set. |

The flow describes an interaction, not a single combined modality label:

```yaml
# Text-to-image
- operation: image.generate
  required_inputs: [text]
  outputs: [image]

# Image-plus-text-to-image editing
- operation: image.edit
  required_inputs: [image, text]
  outputs: [image]

# Text-to-speech; a voice reference is optional
- operation: audio.synthesize_speech
  required_inputs: [text]
  optional_inputs: [audio]
  outputs: [audio]

# General text-to-audio: music or sound, not speech synthesis
- operation: audio.generate
  required_inputs: [text]
  outputs: [audio]

# Audio-plus-text-to-audio transformation
- operation: audio.transform
  required_inputs: [audio, text]
  outputs: [audio]
```

The same model can list several interactions. For example, a speech model may
offer both plain TTS and voice-conditioned TTS through one interaction with
optional reference audio. A music model can list text-to-audio and
audio-conditioned generation separately. A multimodal chat model can list
`chat.generate` for text-only, text-plus-image, and text-plus-video. Input and
output modality unions on the model are for discovery; the interaction list
determines valid request shapes.

An available operation is resolved from the selected model interaction, a
compatible artifact, an installed engine's endpoint contract, and a successful
model–artifact–engine validation. Neither a model claim nor an engine endpoint
alone is enough. Tool calls are an option on `chat.generate` only when both
the model and endpoint support them. Streaming is an endpoint/request behavior,
not a different model ability. `decisions.score` remains separate from normal
chat and is not yet a Mica public endpoint.

For the active profile, a request with an explicit model uses it only if it
supports the requested operation and modalities. Otherwise, a matching
`selection.defaults[]` entry wins; if none exists, Mica chooses the highest
priority eligible model (with a deterministic model-ID tie-break). The current
schema-5 resolver supports `prefer-installed` (default), `manifest-order`, and
`explicit-only`. Omitted engines use an installed compatible engine, then the
first hardware-compatible engine in model-manifest order. Omitted artifacts
prefer Q4, then Q8, then the first remaining artifact ID in sorted order. Setup records the
concrete selection; the active workload reuses it until setup runs again. The
public model-list response also exposes legacy request-category default keys.

Artifacts can list multiple `compatible_engines` IDs. Engine-qualified lookup
preserves separate variants even when engines share a format and quantization.
Image/music-generation routes still need their own adapters before becoming
runnable; a schema declaration alone does not implement an endpoint.

Current public routes map to `text.generate` (`/v1/completions`),
`chat.generate` (`/v1/chat/completions`), `audio.transcribe`
(`/v1/audio/transcriptions`), `audio.diarize` (`/v1/audio/diarizations`), and `audio.synthesize_speech`
(`/v1/audio/speech`). `/v1/agent/chat` composes these calls. New image/audio
generation operations need adapters and public routes before they are usable;
the schemas alone do not add those endpoints.
