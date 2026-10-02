# GGUF quantization audit

| File | GiB | Tensors | Tensor types |
| --- | ---: | ---: | --- |
| `Spark-X2.5-4B-Q4_K_M.gguf` | 2.422 | 290 | F32: 73, Q4_K: 180, Q6_K: 37 |
| `model-local-Q4_K_M.gguf` | 2.422 | 290 | F32: 73, Q4_K: 180, Q6_K: 37 |
| `Spark-X2.5-4B-Q8_0.gguf` | 4.075 | 290 | F32: 73, Q8_0: 217 |
| `model-local-Q8_0.gguf` | 4.075 | 290 | F32: 73, Q8_0: 217 |

## Precision policy by tensor family

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

### `model-local-Q4_K_M.gguf`

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

### `model-local-Q8_0.gguf`

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
| `Spark-X2.5-4B-Q4_K_M.gguf` | `model-local-Q4_K_M.gguf` | same | same | same |
| `model-local-Q4_K_M.gguf` | `Spark-X2.5-4B-Q8_0.gguf` | same | same | different |
| `Spark-X2.5-4B-Q8_0.gguf` | `model-local-Q8_0.gguf` | same | same | same |

