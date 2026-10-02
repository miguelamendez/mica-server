# MiniCPM-V 4.6 drafter audit

Date: 2026-09-16

Status: no compatible smaller sibling and no extractable native MTP weights.

The official MiniCPM-V 4.6 collection publishes the 1.3B Instruct and Thinking
checkpoints plus quantized forms, but no smaller MiniCPM-V 4.6 sibling. The
vision model already uses a Qwen3.5-0.8B-sized text core. Standalone
Qwen3.5-0.8B is not smaller, and it is not tokenizer-compatible: its configured
vocabulary is 248320 while MiniCPM-V 4.6 uses 248094. MiniCPM5-1B is a separate
architecture/tokenizer family and is not a valid draft either.

Both official MiniCPM-V 4.6 configs contain `mtp_num_hidden_layers: 1`.
However, a remote safetensors-header audit of both the Instruct revision
`36f34a661a4bd35d0dc2294cb044d2584646c7d3` and Thinking revision
`93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a` found 779 tensors and none whose
name contains `mtp` or `nextn`; the language-model keys stop at layer 23, which
matches `num_hidden_layers: 24`. Therefore the configuration field is not
evidence of a published, separable MTP head and `mlx_extract_mtp` must remain
disabled.

Sources:

- <https://huggingface.co/collections/openbmb/minicpm-v-46>
- <https://huggingface.co/openbmb/MiniCPM-V-4.6/blob/main/config.json>
- <https://huggingface.co/Qwen/Qwen3.5-0.8B/blob/main/config.json>
