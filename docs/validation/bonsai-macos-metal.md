# Bonsai coding profile: Apple M4 Metal validation

Date: 2026-09-21  
Status: text and image inference passed through the pinned Prism runtime and Mica proxy on this Mac. A [task, context, image, and video benchmark](coder-profile-benchmark-macos.md) now covers the coding profile up to about 4100 actual input tokens. Linux/NVIDIA, the full context ceiling, and batching remain untested.

## Machine and limits

Mica's native `detect --output` generated a YAML system profile reporting macOS arm64, an Apple M4 with 10 logical cores, one Metal GPU, and 24 GiB of unified memory. The coding profile reserved at most 16 GiB for model admission, with a 0.5 GiB safety reserve and one resident worker. Native compilation used two jobs and its 16 GiB build policy. This is **not** a kernel-enforced process-memory limit.

## Installation and artifacts

`setup --profile mica-coder-bonsai-macos --ram-gib 16` installed the selected MLX packages and built the isolated Prism `llama-server` at commit `9a9394a895b96003ca842a6041cb28ac49a108f7` (build 10709) with Metal enabled. No stock llama.cpp runtime was selected for Bonsai.

The first server start downloaded the pinned Bonsai `PQ2_0` GGUF (7,206,168,928 bytes) and BF16 projector (931,145,856 bytes). Mica verified both declared sizes and SHA-256 hashes before moving them into `~/.mica/models/gguf/ternary-bonsai-2-27b/` and recording completion. Spark MLX Q4 and MiniCPM-V Thinking MLX Q4 were also downloaded because schema-3 prewarm fetches every selected profile artifact, including on-demand workers.

## Real inference

The direct Prism worker used the profile's 8192-token context, one slot, Q8 K/V cache, 99 GPU layers, and BF16 projector. It passed:

| Request | Visible result | Outcome |
| --- | --- | --- |
| `12 × 13`, number only | `156` | Correct; 28.57 prompt tokens/s, 10.24 generated tokens/s. |
| Image with blue upper-left and red lower-right squares | `Upper-left: blue. Lower-right: red.` | Correct colors; 10.30 generated tokens/s. |
| Exact center text in the same image | `MICA 31415` | Correct OCR; 10.27 generated tokens/s. |

The loaded direct worker was observed at about 8.0 GiB RSS. This is a process snapshot, not peak unified-memory or Metal-allocation telemetry.

The Mica proxy then reached `/ready` with Bonsai warmed and `smoke_validated: true`. An unauthenticated `/v1/models` request returned 401. An authenticated listing selected Bonsai as the default chat model and reported the three exact engine/artifact variants. Proxy requests passed:

| Route/model | Visible result | Outcome |
| --- | --- | --- |
| Chat / Bonsai, `17 × 19` | `323` | Correct; HTTP 200, 10.32 generated tokens/s. |
| Image chat / Bonsai, center text | `MICA 31415` | Correct, `finish_reason: stop` with a 160-token cap. |
| Chat / Spark after Bonsai | `SPARK_OK` | Correct; Bonsai was evicted, Spark generated at 33.39 tokens/s. |
| Image chat / MiniCPM after Spark | `MICA 31415`, blue left, red right | Correct; Spark was evicted, MiniCPM generated at 73.98 tokens/s. |
| Chat / Bonsai after MiniCPM, `9²` | `81` | Correct; MiniCPM was evicted and Bonsai reloaded from cache. |

`/admin/models` showed one resident Bonsai worker, a 12 GiB RAM reservation, zero dedicated VRAM reservation (Metal uses unified memory), and a 15.5 GiB effective admission budget. Its process RSS was about 8.5 GiB after warmup and 8.7 GiB after repeated requests, while macOS reported roughly 25–33% system memory free. Mica's reservations are admission estimates, not hard RSS caps. Stopping the test server returned reported free memory to 69%.

The public chat streaming route initially buffered SSE until generation completed and exposed the worker's local model path in chunks. This was fixed during validation for both `/v1/chat/completions` and legacy `/v1/completions`. The chat retest produced 70 SSE data events plus `[DONE]`, with no local path in any event; first byte arrived in 0.0004 s and the response finished in 7.43 s. The legacy retest produced live `text` deltas and the public model ID, also with `[DONE]` and no path leak (0.0004 s to first byte, 7.85 s total). The worker lease returned to `in_flight: 0`. A cached Mica restart reached `/ready` without another model download. All 62 local regression tests passed after the change.

## Remaining limits

- This validates the Mac Metal route, not the Linux/NVIDIA or CPU placements. Their separate profiles are still experimental.
- The 8192-token context was configured but not filled; tasks up to about 4100 actual input tokens passed through the profile, while the full ceiling, batch throughput, and peak unified-memory allocation remain unmeasured.
- Prism logs recommend at least 1024 image tokens for Qwen-VL grounding tasks. These simple color/OCR fixtures passed at the current defaults; more demanding grounding should be tested before claiming broad vision quality.
- A combined color-and-text prompt returned only the colors once, while the targeted OCR prompt returned the exact text. The smoke tests establish basic functionality, not comprehensive instruction-following quality.
