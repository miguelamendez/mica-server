# GGUF quantization audit

| File | GiB | Tensors | Tensor types |
| --- | ---: | ---: | --- |
| `granite-speech-5.0-470m-turboctc-q4_k.gguf` | 0.253 | 550 | BF16: 340, F32: 32, I64: 16, Q4_K: 162 |
| `granite-speech-5.0-470m-turboctc-q8_0.gguf` | 0.470 | 550 | BF16: 323, F32: 32, I64: 16, Q8_0: 179 |
| `audio8-tts-preview-0.6b-q4_0.gguf` | 1.018 | 681 | BF16: 82, F16: 13, F32: 337, Q4_0: 249 |
| `audio8-tts-preview-0.6b-q8_0.gguf` | 1.331 | 681 | BF16: 82, F16: 13, F32: 337, Q8_0: 249 |
| `MiniCPM-V-4_6-Thinking-Q4_K_M.gguf` | 0.493 | 320 | F32: 133, Q4_K: 162, Q6_K: 25 |
| `MiniCPM-V-4_6-Thinking-Q8_0.gguf` | 0.756 | 320 | F32: 133, Q8_0: 187 |
| `mmproj-model-f16.gguf` | 1.033 | 459 | F16: 171, F32: 288 |
| `Spark-X2.5-4B-Q4_K_M.gguf` | 2.422 | 290 | F32: 73, Q4_K: 180, Q6_K: 37 |
| `Spark-X2.5-4B-Q8_0.gguf` | 4.075 | 290 | F32: 73, Q8_0: 217 |

## Precision policy by tensor family

### `granite-speech-5.0-470m-turboctc-q4_k.gguf`

| Family | Tensor types |
| --- | --- |
| audio_or_codec | BF16: 48, Q4_K: 32 |
| normalization | BF16: 192, F32: 32, I64: 16 |
| other | BF16: 100, Q4_K: 130 |

### `granite-speech-5.0-470m-turboctc-q8_0.gguf`

| Family | Tensor types |
| --- | --- |
| audio_or_codec | BF16: 48, Q8_0: 32 |
| normalization | BF16: 192, F32: 32, I64: 16 |
| other | BF16: 83, Q8_0: 147 |

### `audio8-tts-preview-0.6b-q4_0.gguf`

| Family | Tensor types |
| --- | --- |
| audio_or_codec | F16: 10, F32: 306, Q4_0: 108 |
| normalization | BF16: 58, F32: 31 |
| other | BF16: 24, F16: 3, Q4_0: 141 |

### `audio8-tts-preview-0.6b-q8_0.gguf`

| Family | Tensor types |
| --- | --- |
| audio_or_codec | F16: 10, F32: 306, Q8_0: 108 |
| normalization | BF16: 58, F32: 31 |
| other | BF16: 24, F16: 3, Q8_0: 141 |

### `MiniCPM-V-4_6-Thinking-Q4_K_M.gguf`

| Family | Tensor types |
| --- | --- |
| attention_output | Q4_K: 6 |
| attention_qkv | Q4_K: 24, Q6_K: 12 |
| audio_or_codec | F32: 18 |
| ffn_down | Q4_K: 12, Q6_K: 12 |
| ffn_gate_up | Q4_K: 48 |
| normalization | F32: 79 |
| other | F32: 36, Q4_K: 72 |
| token_embedding | Q6_K: 1 |

### `MiniCPM-V-4_6-Thinking-Q8_0.gguf`

| Family | Tensor types |
| --- | --- |
| attention_output | Q8_0: 6 |
| attention_qkv | Q8_0: 36 |
| audio_or_codec | F32: 18 |
| ffn_down | Q8_0: 24 |
| ffn_gate_up | Q8_0: 48 |
| normalization | F32: 79 |
| other | F32: 36, Q8_0: 72 |
| token_embedding | Q8_0: 1 |

### `mmproj-model-f16.gguf`

| Family | Tensor types |
| --- | --- |
| attention_qkv | F16: 84, F32: 84 |
| ffn_down | F16: 28, F32: 28 |
| ffn_gate_up | F16: 28, F32: 28 |
| normalization | F32: 2 |
| other | F16: 30, F32: 145 |
| vision_or_projector | F16: 1, F32: 1 |

### `Spark-X2.5-4B-Q4_K_M.gguf`

| Family | Tensor types |
| --- | --- |
| attention_output | Q4_K: 36 |
| attention_qkv | Q4_K: 18, Q6_K: 18 |
| ffn_down | Q4_K: 18, Q6_K: 18 |
| ffn_gate_up | Q4_K: 72 |
| normalization | F32: 73 |
| other | Q4_K: 36 |
| token_embedding | Q6_K: 1 |

### `Spark-X2.5-4B-Q8_0.gguf`

| Family | Tensor types |
| --- | --- |
| attention_output | Q8_0: 36 |
| attention_qkv | Q8_0: 36 |
| ffn_down | Q8_0: 36 |
| ffn_gate_up | Q8_0: 72 |
| normalization | F32: 73 |
| other | Q8_0: 36 |
| token_embedding | Q8_0: 1 |

## Pairwise comparison

| Left | Right | Names | Shapes | Precision map |
| --- | --- | --- | --- | --- |
| `granite-speech-5.0-470m-turboctc-q4_k.gguf` | `granite-speech-5.0-470m-turboctc-q8_0.gguf` | same | same | different |
| `granite-speech-5.0-470m-turboctc-q8_0.gguf` | `audio8-tts-preview-0.6b-q4_0.gguf` | different | same | different |
| `audio8-tts-preview-0.6b-q4_0.gguf` | `audio8-tts-preview-0.6b-q8_0.gguf` | same | same | different |
| `audio8-tts-preview-0.6b-q8_0.gguf` | `MiniCPM-V-4_6-Thinking-Q4_K_M.gguf` | different | same | different |
| `MiniCPM-V-4_6-Thinking-Q4_K_M.gguf` | `MiniCPM-V-4_6-Thinking-Q8_0.gguf` | same | same | different |
| `MiniCPM-V-4_6-Thinking-Q8_0.gguf` | `mmproj-model-f16.gguf` | different | same | different |
| `mmproj-model-f16.gguf` | `Spark-X2.5-4B-Q4_K_M.gguf` | different | same | different |
| `Spark-X2.5-4B-Q4_K_M.gguf` | `Spark-X2.5-4B-Q8_0.gguf` | same | same | different |

