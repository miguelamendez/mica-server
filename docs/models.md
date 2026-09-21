# Models, quantization, and publishing

Mica separates a logical model from its engine-specific artifacts. One model
entry may describe MLX Q4/Q8 and GGUF Q4/Q8 variants, while a profile chooses
exactly one engine and artifact for a running task.

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

Internal route-validation fixtures are excluded from the public ledger and
normal profile listings.

## Curated assistant models

| Model | Modality | MLX | GGUF | License |
| --- | --- | --- | --- | --- |
| Spark-X2.5-4B | Text generation | Q4, Q8 | Q4_K_M, Q8_0 | Apache-2.0 |
| Granite Speech 5.0 470M TurboCTC | ASR | Q4, Q8 | Q4_K, Q8_0 | Apache-2.0 |
| Audio8 TTS Preview 0.6B | TTS and voice cloning | Q4, Q8 | Q4_0, Q8_0 | Apache-2.0 |
| MiniCPM-V 4.6 Thinking | Image/video to text | Q4, Q8 | Q4_K_M, Q8_0 | Apache-2.0 |
| Ternary Bonsai 2 27B | Image/text to text | — | PQ2_0 + BF16 projector (Prism fork) | Apache-2.0 |

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

A shareable schema-3 YAML profile can reference another Hugging Face model directly,
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
