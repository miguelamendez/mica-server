# GGUF quantization audit

| File | GiB | Tensors | Tensor types |
| --- | ---: | ---: | --- |
| `MiniCPM-V-4_6-Thinking-Q4_K_M.gguf` | 0.493 | 320 | F32: 133, Q4_K: 162, Q6_K: 25 |
| `model-local-Q4_K_M.gguf` | 0.493 | 320 | F32: 133, Q4_K: 162, Q6_K: 25 |
| `MiniCPM-V-4_6-Thinking-Q8_0.gguf` | 0.756 | 320 | F32: 133, Q8_0: 187 |
| `model-local-Q8_0.gguf` | 0.756 | 320 | F32: 133, Q8_0: 187 |

## Precision policy by tensor family

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

### `model-local-Q4_K_M.gguf`

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

### `model-local-Q8_0.gguf`

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

## Pairwise comparison

| Left | Right | Names | Shapes | Precision map |
| --- | --- | --- | --- | --- |
| `MiniCPM-V-4_6-Thinking-Q4_K_M.gguf` | `model-local-Q4_K_M.gguf` | same | same | same |
| `model-local-Q4_K_M.gguf` | `MiniCPM-V-4_6-Thinking-Q8_0.gguf` | same | same | different |
| `MiniCPM-V-4_6-Thinking-Q8_0.gguf` | `model-local-Q8_0.gguf` | same | same | same |

