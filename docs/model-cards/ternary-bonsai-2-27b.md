# Ternary Bonsai 2 27B

Status: experimental integration candidate; real inference is not yet certified
by Mica.

## Identity and license

- Capability: image and text to text
- Upstream artifact repository:
  [`prism-ml/Ternary-Bonsai-2-27B-gguf`](https://huggingface.co/prism-ml/Ternary-Bonsai-2-27B-gguf)
- Pinned artifact revision:
  `6ed5e12bf84b7a63069882c91dd9e9218647d17b`
- License: Apache-2.0
- Nominal parameter class: 27B

## Context contract

The upstream architecture advertises a 262,144-token context ceiling. Mica
does not treat that architectural ceiling as a locally certified operating
point. The initial execution profile is deliberately limited to:

- maximum input: 6,144 tokens;
- maximum output: 2,048 tokens;
- maximum total: 8,192 tokens;
- concurrency: one request.

The context length used during upstream training is not documented in the
metadata reviewed for this integration. Mica therefore does not claim that the
full architectural ceiling preserves quality. Longer profiles require separate
quality and memory evidence.

## Exact artifact

| Component | Packing | Bytes | SHA-256 |
| --- | --- | ---: | --- |
| Language model | `PQ2_0` | 7,206,168,928 | `3907dc1658db1f78a9826bf8d5bcb8dc65db0d466388937af57f2294fae62ec1` |
| Vision projector | BF16 | 931,145,856 | `e287342d92332fa3577ed1d42e921dac9370c08da58ba9337fa450f6cc76cfd7` |

The registry variant ID is `pq2_0`; the exact upstream packing remains
`PQ2_0`. Mica does not relabel it Q4, Q8, or native. The two files total
8,137,314,784 bytes (about 7.58 GiB) before runtime workspace and KV cache.

## Required engine

These rotated ternary weights require Prism's activation transform. They must
run with the separate `prism-llama-cpp` engine and must not fall back to stock
llama.cpp.

- Source: [`PrismML-Eng/llama.cpp`](https://github.com/PrismML-Eng/llama.cpp)
- Upstream release: `prism-b10709-9a9394a`
- Resolved immutable commit:
  `9a9394a895b96003ca842a6041cb28ac49a108f7`
- Isolated runtime directory: `~/.mica/runtimes/prism-llama.cpp`
- Built target: `llama-server`

The runtime directory and build are independent from
`~/.mica/runtimes/llama.cpp`; libraries from the two trees must not be mixed.

## Provisional memory profiles

The initial CPU profile reserves 12 GiB of RAM. The initial full-offload GPU
profile reserves 0.75 GiB of host RAM and 12 GiB of VRAM. These are conservative
admission values, not measured peak-memory claims. They must be replaced or
confirmed using process RSS and accelerator telemetry during Linux/NVIDIA
validation.

## Required acceptance tests

Before the model is marked certified:

1. compile the pinned Prism engine from a clean source installation;
2. verify both downloaded files against their pinned sizes and SHA-256 values;
3. prove stock llama.cpp is never selected;
4. run deterministic text inference;
5. run image-grounded inference through the BF16 projector;
6. measure CPU and full-GPU-offload memory, cold start, prompt processing, and
   decoding speed; and
7. test unload/reload behavior under Mica's RAM and VRAM admission limits.
