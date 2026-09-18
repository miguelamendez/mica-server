# GGUF local reproduction audit

Date: 2026-09-16  
Host: Apple M4, 24 GiB unified memory  
Resource policy: at most two quantization threads for the accepted reproduction
runs and a 512 MiB maximum tensor buffer; no inference workers were active.

This audit answers whether the adopted Spark-X2.5 and MiniCPM-V GGUFs contain a
special tensor-precision recipe that mica-server should reproduce. The local
comparison artifacts were disposable and were removed after the audit. The
accepted files under `~/models/checkpoints/gguf` were not modified.

## Tool and source pins

- llama.cpp quantizer: build 11009, commit
  `fb27a525d28381a16a4bb038858a10e4927381ca`
- MiniCPM-V 4.6 Thinking source:
  `openbmb/MiniCPM-V-4.6-Thinking@93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a`
- MiniCPM official GGUF:
  `openbmb/MiniCPM-V-4.6-Thinking-gguf@2e49fbd34ad9df11f0e93c93c2c5a80ba7cdc5e5`
- Spark source:
  `XHToken/Spark-X2.5-4B@0bcb35678590218655dff3765b9e61c83b35e9c4`
- Spark community GGUF:
  `abenzerps/Spark-X2.5-4B-GGUF@f186f3d265c583a49d5e9d19bff23150b002202b`

The Spark tensor blobs at source revision `0bcb356...` match the quantizer's
documented source revision `ea14618...`; the intervening source changes were
documentation-only.

## Reproduction commands

The paths below show the exact procedure. The `comparisons/gguf-local` outputs
no longer exist.

```sh
CONVERT=~/models/runtime/llama.cpp/convert_hf_to_gguf.py
QUANTIZE=~/models/runtime/llama.cpp/build-mica/bin/llama-quantize
PYTHON=~/models/environment-mlx/bin/python

# MiniCPM text tower and full-precision multimodal projector
$PYTHON $CONVERT \
  ~/models/staging/minicpm-v46-thinking/93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a/source \
  --outfile ~/models/comparisons/gguf-local/minicpm-v46-thinking/model-f16.gguf \
  --outtype f16 --use-temp-file
$PYTHON $CONVERT \
  ~/models/staging/minicpm-v46-thinking/93d8f4b60ad5d1f763442cf4c19f2a71fa95af4a/source \
  --outfile ~/models/comparisons/gguf-local/minicpm-v46-thinking/projector-f16.gguf \
  --outtype f16 --mmproj --use-temp-file
$QUANTIZE --max-buffer-size 512 \
  ~/models/comparisons/gguf-local/minicpm-v46-thinking/model-f16.gguf \
  ~/models/comparisons/gguf-local/minicpm-v46-thinking/model-local-Q4_K_M.gguf \
  Q4_K_M 2
$QUANTIZE --max-buffer-size 512 \
  ~/models/comparisons/gguf-local/minicpm-v46-thinking/model-f16.gguf \
  ~/models/comparisons/gguf-local/minicpm-v46-thinking/model-local-Q8_0.gguf \
  Q8_0 2

# Spark
$PYTHON $CONVERT \
  ~/models/staging/spark-x25-4b/0bcb35678590218655dff3765b9e61c83b35e9c4/source \
  --outfile ~/models/comparisons/gguf-local/spark-x25-4b/model-f16.gguf \
  --outtype f16 --use-temp-file
$QUANTIZE --max-buffer-size 512 \
  ~/models/comparisons/gguf-local/spark-x25-4b/model-f16.gguf \
  ~/models/comparisons/gguf-local/spark-x25-4b/model-local-Q4_K_M.gguf \
  Q4_K_M 2
$QUANTIZE --max-buffer-size 512 \
  ~/models/comparisons/gguf-local/spark-x25-4b/model-f16.gguf \
  ~/models/comparisons/gguf-local/spark-x25-4b/model-local-Q8_0.gguf \
  Q8_0 2
```

`scripts/audit_gguf_quantization.py` records tensor names, shapes, precision
types, and architecture-neutral tensor families. SHA-256 was also computed for
each tensor payload independently of GGUF metadata and header ordering.

## Results

| Model/artifact | Names and shapes | Precision map | Tensor payloads | Decision |
|---|---|---|---|---|
| MiniCPM Q4_K_M | exact match | exact match | byte-identical | Keep official GGUF |
| MiniCPM Q8_0 | exact match | exact match | byte-identical | Keep official GGUF |
| MiniCPM F16 projector | exact match | exact match | byte-identical | Keep official projector |
| Spark Q8_0 | exact match | exact match | byte-identical | Keep trusted GGUF |
| Spark Q4_K_M | exact match | exact match | 208 quantized tensors differ | Keep tested trusted GGUF; retain local recipe only |

MiniCPM Q4_K_M contains 133 F32, 162 Q4_K, and 25 Q6_K tensors. Its token
embedding, twelve selected attention tensors, and twelve feed-forward down
projections are Q6_K. The separate vision/projector file remains F16/F32.

Spark Q4_K_M contains 73 F32, 180 Q4_K, and 37 Q6_K tensors. Its token
embedding, eighteen attention QKV tensors, and eighteen feed-forward down
projections are Q6_K. This is standard llama.cpp Q4_K_M selection rather than
a disclosed Spark-specific mixed-precision recipe. The trusted file contains
no `quantize.imatrix.*` provenance keys. Its different packed Q4 values may
come from the quantizer build, conversion precision, or invocation details;
there is no evidence they are a calibrated quality improvement.

The MiniCPM source config declares one MTP layer, but llama.cpp's MiniCPM-V 4.6
converter explicitly filters `mtp.*` because that head is not used for this
architecture's inference path. The reproduced and official GGUFs are therefore
trunk-only despite retaining NextN-related metadata. They must not be advertised
as containing a validated MTP drafter.

## Publication decision

Use the already tested trusted Spark and official MiniCPM artifacts. Do not
publish duplicate local copies. A future Spark Q4 candidate is eligible only if
it beats the accepted file on the same reasoning, coding, JSON, long-context,
memory, and throughput suite. Granite Speech and Audio8 remain local,
model-specific conversions because their audio-sensitive precision policies
were validated separately.
