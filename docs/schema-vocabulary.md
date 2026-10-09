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

Context metadata is flat: `native_context_tokens`,
`recommended_context_tokens`, and `max_output_tokens`, each a positive
integer or `null`. Recommended context and verified maximum output cannot
exceed native total capacity. References record evidence; output examples,
recommendations, and generation defaults must not be encoded as hard limits.
See [context and generation limits](models.md#context-and-generation-limits).

| Layer | Controlled values | Meaning |
| --- | --- | --- |
| Modality | `text`, `image`, `video`, `audio`, `embedding` | Physical input/output data; `embedding` is a vector output, not a media input. An interaction lists required and optional inputs separately. |
| Model ability | `text_generation`, `instruction_following`, `tool_calling`, `reasoning`, `structured_output`, `image_understanding`, `video_understanding`, `audio_understanding`, `speech_recognition`, `speech_translation`, `speaker_diarization`, `speech_synthesis`, `voice_conditioning`, `general_audio_generation`, `music_generation`, `audio_transformation`, `image_generation`, `image_editing`, `video_generation`, `video_editing` | Properties of the weights, backed by a model card or validation. Coding and creativity are evaluation domains, not binary abilities. |
| Supported task | `supported_tasks[]`; see the task index below | Declared intended uses for discovery and filtering, not a quality guarantee, an endpoint URL or a routing permission. |
| Operation | `text.generate`, `chat.generate`, `embedding.generate`, `decisions.score`, `audio.transcribe`, `audio.translate`, `audio.diarize`, `audio.synthesize_speech`, `audio.generate`, `audio.transform`, `image.generate`, `image.edit`, `video.generate`, `video.edit` | A normalized callable contract. The engine must expose a real adapter call for it. Agent execution is a composition of operations, not a model operation. |
| Engine interface | `endpoint_contracts[]` with operation, transport, adapter call, input/output shape, and optional streaming/tools flags | Describes **actual callable surfaces**, not hypothetical features inferred from the model type. Current adapters support HTTP POST only; unsupported transports and calls are rejected. |
| Artifact compatibility | `compatible_engines[]`, `required_compatibility[]` | Loadability constraints such as architecture support or a vision projector. They do not imply an API operation. |
| Workload profile | `models[]`, optional `selection.defaults[]` | A collection of model IDs and per-model policy. No fixed groups or roles. Priority selects among eligible models unless the caller specifies a model or an operation/input-specific default is set. |

## Descriptions and task index

Models, engines and workloads require a nonblank natural-language `description`.
Use it to explain the intended use and relevant limitations. `purpose` is not a
manifest field. Descriptions are separate from controlled identifiers, and are
never parsed to enable inference.

Every packaged model declares a nonempty, unique `supported_tasks` list. Schema
and native validation require the associated abilities and a matching interaction
with the necessary inputs. The task index is:

| Task identifiers | Required abilities | Compatible operation(s) | Additional input |
| --- | --- | --- | --- |
| `chat` | `text_generation` | `chat.generate` | Text |
| `coding`, `structured_extraction` | `text_generation` | `chat.generate` or `text.generate` | Text |
| `ocr`, `visual_question_answering` | `text_generation`, `image_understanding` | `chat.generate` | Image |
| `video_question_answering` | `text_generation`, `video_understanding` | `chat.generate` | Video |
| `audio_question_answering` | `text_generation`, `audio_understanding` | `chat.generate` | Audio |
| `tool_calling` | `tool_calling` | `chat.generate` | Text |
| `decision_scoring` | `text_generation` | `decisions.score` | Text |
| `transcription` | `speech_recognition` | `audio.transcribe` or `chat.generate` | Audio |
| `speech_translation` | `speech_translation` | `audio.translate` or `chat.generate` | Audio |
| `speaker_diarization` | `speaker_diarization` | `audio.diarize` | Audio |
| `text_to_speech` | `speech_synthesis` | `audio.synthesize_speech` | Text |
| `voice_cloning` | `speech_synthesis`, `voice_conditioning` | `audio.synthesize_speech` | Text and reference audio |
| `embedding`, `retrieval` | `embedding_generation` | `embedding.generate` | Inputs declared by that interaction |
| `image_generation` | `image_generation` | `image.generate` | Text |
| `image_editing` | `image_editing` | `image.edit` | Image |
| `video_generation` | `video_generation` | `video.generate` | Text |
| `video_editing` | `video_editing` | `video.edit` | Video |
| `audio_generation` | `general_audio_generation` | `audio.generate` | Text |
| `music_generation` | `music_generation` | `audio.generate` | Text |
| `audio_transformation` | `audio_transformation` | `audio.transform` | Audio |

These are intended-use categories, not benchmark certification. In particular,
`structured_extraction` does not promise grammar-enforced JSON; enforcement
depends on the selected engine and request. `retrieval` describes embeddings
usable for search, not a complete indexing or RAG service. Agent execution is a
server orchestration feature using tool calls, not a separate model type.
Future task/operation identifiers do not create new Mica routes or adapters.

```yaml
description: A vision-language model for chat, coding and visual document analysis.
supported_tasks: [chat, coding, ocr, visual_question_answering]
input_modalities: [text, image]
output_modalities: [text]
abilities: [text_generation, image_understanding]
supported_interactions:
  - operation: chat.generate
    required_inputs: [text]
    optional_inputs: [image]
    outputs: [text]
```

Endpoints are operation-specific, not necessarily task-specific: chat, coding,
OCR and visual Q&A can all use `/v1/chat/completions`. An omni-model can also
declare transcription only when it supports speech recognition and a compatible
audio interaction. Audio understanding alone is insufficient. A dedicated
transcription endpoint additionally needs an `audio.transcribe` engine adapter;
chat-based transcription does not automatically enable that endpoint.

```text
Model: modalities + abilities + declared tasks + supported interactions
                                 ↓
Selected artifact + engine's implemented operation/input contract
                                 ↓
Mica's public endpoint, within the active workload
```

Inspect declared tasks through `mica-server registry list --task ocr` (combinable
with `--engine`, `--backend` and `--modality`), or search a task in the TUI Models
view. The registry and `/v1/models` expose `supported_tasks`; endpoint eligibility
continues to be resolved from interactions, not these labels. Filter results are
manifest declarations, not proof that a selected engine implements every task.
Externally added models without task metadata remain undeclared rather than
having tasks inferred from a broad type label.

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
`chat.generate` (`/v1/chat/completions`), `embedding.generate`
(`/v1/embeddings`, ability `embedding_generation`), `audio.transcribe`
(`/v1/audio/transcriptions`), `audio.diarize` (`/v1/audio/diarizations`), and `audio.synthesize_speech`
(`/v1/audio/speech`). `/v1/agent/chat` composes these calls. New image/audio
generation operations need adapters and public routes before they are usable;
the schemas alone do not add those endpoints.

Embedding workloads set `max_output_tokens: 0`: their outputs are vectors,
not decoded tokens. Generative workloads must still reserve a positive output
budget. Multimodal embeddings use the same content-part types as native chat,
but belong to a separate operation and cannot be routed to chat generation.
