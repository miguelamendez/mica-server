# Granite Speech 5 TurboCTC MLX Q4 validation

Date: 2026-09-15

Status: selective conversion and real inference passed; publishing not started.

## Provenance and format

- Original repository: <https://huggingface.co/ibm-granite/granite-speech-5.0-470m-turboctc>
- Immutable revision: `6c14d3d052a602d850f1bc4aa017f25f4adf6aa0`
- Source license: Apache-2.0
- Output: MLX affine Q4, group size 64
- Mica profile: `granite-speech5-quality`

The profile leaves `encoder.input_linear`, `encoder.out`, and
`encoder.out_mid` at their source precision and quantizes the remaining
eligible layers. The complete original snapshot remains in revisioned staging.

## Quality-control result

A uniform Q4 conversion completed technically but failed the speech-content
check. On the fixed 8.608-second clip it returned:

> the the and choosing the3 low else to protect prime data on thistop

The source BF16 checkpoint and selective Q4 checkpoint both returned exactly:

> the quick brown fox jumps over the lazy dog please schedule a meeting for
> tuesday at 330 local models protect private data on this laptop

For this controlled clip, BF16 produced 29 tokens in 0.31 seconds at 93.605
tokens/second and reported about 1.14 GB peak memory. Selective Q4 produced the
same 29 tokens in 0.08 seconds at 352.763 tokens/second and reported about 0.49
GB peak memory. These are single-run functional measurements, not a general
quality or throughput benchmark.

The uniform artifact is rejected. Broader dataset evaluation remains required
before publication, but the selective checkpoint passes the current real-audio
smoke and exact-transcript regression.

An `mlx_audio.server` process also loaded the selective Q4 directory. A real
multipart `POST /v1/audio/transcriptions` request returned HTTP 200 and the
same exact reference transcript, validating the serving path in addition to
direct model inference.
