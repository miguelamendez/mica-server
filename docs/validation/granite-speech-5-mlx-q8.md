# Granite Speech 5 TurboCTC MLX Q8 validation

Date: 2026-09-15

Status: conversion and real HTTP inference passed; publishing not started.

## Provenance and format

- Original repository: <https://huggingface.co/ibm-granite/granite-speech-5.0-470m-turboctc>
- Immutable revision: `6c14d3d052a602d850f1bc4aa017f25f4adf6aa0`
- Source license: Apache-2.0
- Output: MLX affine Q8, group size 64
- Mica profile: `granite-speech5-quality`
- Output directory allocation: 525,920 KiB

The Q8 conversion reused the complete pinned original snapshot, never another
quantized artifact. As with Q4, the profile leaves
`encoder.input_linear`, `encoder.out`, and `encoder.out_mid` at source
precision.

## Real HTTP inference

An `mlx_audio.server` process loaded the Q8 artifact. A real multipart
`POST /v1/audio/transcriptions` request with the fixed 8.608-second clip
returned HTTP 200 and exactly:

> the quick brown fox jumps over the lazy dog please schedule a meeting for
> tuesday at 330 local models protect private data on this laptop

This matches the source BF16 checkpoint and the selective Q4 checkpoint on the
controlled clip. Broader ASR dataset evaluation remains required before
publication.
