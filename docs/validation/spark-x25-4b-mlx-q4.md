# Spark-X2.5-4B MLX Q4 validation

Date: 2026-09-15

Status: conversion and real inference passed; publishing not started.

## Provenance

- Original repository: <https://huggingface.co/XHToken/Spark-X2.5-4B>
- Immutable revision: `0bcb35678590218655dff3765b9e61c83b35e9c4`
- License reported by the source repository: Apache-2.0
- Full local source snapshot: 8,239,643,608 bytes
- Output: MLX affine Q4, group size 64
- Output checkpoint weights: 2,313,406,696 bytes

The source snapshot remains under the revisioned staging directory. It must not
be removed until Q4 and Q8 are validated, uploaded, and cleanly redownloaded.

## Real inference

Direct generation returned exactly `SPARK_Q4_OK`.

An `mlx_vlm.server` process then served the artifact on loopback. A real
`POST /v1/chat/completions` request returned HTTP 200 and exactly
`MICA_OK_31415`.

- Prompt: 29 tokens
- Completion: 10 tokens
- Prefill: 161.4 tokens/second
- Decode: 36.8 tokens/second
- MLX server-reported peak memory: 2.403 GiB
- Draft kind: null

The result is a baseline autoregressive decode, not a speculative decode.

## MTP and DFlash audit

The pinned config declares neither `mtp_num_hidden_layers` nor
`num_nextn_predict_layers`. Its complete 290-entry safetensors index contains
no key matching `mtp`, `draft`, or `nextn`.

Running the installed `mlx_vlm.split_mtp` against the complete source snapshot
failed with `No native MTP tensors / registered splitter`. The live XHToken
Hugging Face inventory exposes 4B and 1.7B base/instruct, FP8, INT8, and GGUF
repositories, but no MTP or DFlash drafter. The installed MLX drafter registry
also has no Spark family.

The 1.7B sibling is not evidence of MTP or DFlash. It may only be considered as
a classic model-based speculative candidate in a runtime that supports that
method, after measuring acceptance rate, total memory, and end-to-end speedup.

Sources:

- <https://github.com/XHToken/Spark-X2.5>
- <https://huggingface.co/XHToken/Spark-X2.5-4B>
- <https://huggingface.co/XHToken/Spark-X2.5-1.7B>
