# Spark-X2.5-4B MLX Q8 validation

Date: 2026-09-15

Status: conversion and real inference passed; publishing not started.

## Provenance and format

- Original repository: <https://huggingface.co/XHToken/Spark-X2.5-4B>
- Immutable revision: `0bcb35678590218655dff3765b9e61c83b35e9c4`
- Source license: Apache-2.0
- Output: MLX affine Q8, group size 64
- Output directory size: approximately 4.1 GiB
- Config records `bits: 8`, `group_size: 64`, and `mode: affine`

The Q8 conversion reused the complete retained original snapshot; it did not
quantize from Q4 or another derivative.

## Real inference

Direct generation returned exactly `SPARK_Q8_OK` and reported a peak process
footprint of approximately 4.73 GB.

An `mlx_vlm.server` process then served the artifact on loopback. A real
`POST /v1/chat/completions` request returned HTTP 200 and exactly
`MICA_OK_31415`.

- Prompt: 29 tokens
- Completion: 10 tokens
- Prefill: 172.2 tokens/second
- Decode: 21.4 tokens/second as reported in the JSON timing object
- MLX server-reported peak memory: 4.453 GiB
- Draft kind: null

This short smoke run is evidence of functional inference, not a statistically
valid performance or quality benchmark. Q8 used about 2.05 GiB more peak model
memory than Q4. Q4 happened to decode faster on this tiny response, which must
be rechecked with warm repeated prompts before drawing a throughput conclusion.
