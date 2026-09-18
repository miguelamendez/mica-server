# GGUF quantization audit

| File | GiB | Tensors | Tensor types |
| --- | ---: | ---: | --- |
| `mmproj-model-f16.gguf` | 1.033 | 459 | F16: 171, F32: 288 |
| `projector-f16.gguf` | 1.033 | 459 | F16: 171, F32: 288 |

## Precision policy by tensor family

### `mmproj-model-f16.gguf`

| Family | Tensor types |
| --- | --- |
| attention_qkv | F16: 84, F32: 84 |
| ffn_down | F16: 28, F32: 28 |
| ffn_gate_up | F16: 28, F32: 28 |
| normalization | F32: 2 |
| other | F16: 30, F32: 145 |
| vision_or_projector | F16: 1, F32: 1 |

### `projector-f16.gguf`

| Family | Tensor types |
| --- | --- |
| attention_qkv | F16: 84, F32: 84 |
| ffn_down | F16: 28, F32: 28 |
| ffn_gate_up | F16: 28, F32: 28 |
| normalization | F32: 2 |
| other | F16: 30, F32: 145 |
| vision_or_projector | F16: 1, F32: 1 |

## Pairwise comparison

| Left | Right | Names | Shapes | Precision map |
| --- | --- | --- | --- | --- |
| `mmproj-model-f16.gguf` | `projector-f16.gguf` | same | same | same |

