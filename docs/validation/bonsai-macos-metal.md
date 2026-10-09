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

## Separate DFlash2 text-only attempt (2026-10-06)

The original cached PQ2 target remains unchanged. The requested experiment
uses a separate community Bonsai-adapted DFlash2 Q8 drafter, with **no vision
projector and no MTP head**. The combined MTP target is not used for this test.

- [Community Bonsai DFlash2](https://huggingface.co/ProCreations/Ternary-Bonsai-2-27B-DFlash2), pinned revision `4cfb6ad03268fed0f60ca96c1a659c0b1c77e50b`.
- File: `Bonsai-2-27B-DFlash2-Q8_0.gguf`, 2,056,415,104 bytes;
  SHA-256 `9dd11c8adb910058faf9fb77b10d90c1c048a4f3c2887a890f592cbd882deb9a`.
- Community patched source archive SHA-256:
  `8c0f589673b25574f27f013bb3278824384eb35eb984034a2445af3c437b9d05`.
  Provenance is Prism commit `d8f26eec76da6d09bb708bcba51ef64b8cd868a3`
  plus the publisher's patches. Archive checksum, not its inherited build
  version string, identifies this test source.

The installed pinned Prism runtime lacks the DFlash2 selector implementation,
despite advertising a generic DFlash CLI flag. A separate patched runtime was
therefore built for Metal, with CUDA off, one compiler job, and a monitored
2 GiB process-tree cap. Compilation passed, peaking at **0.79 GiB RSS**.
This does not establish inference compatibility.

The planned comparison uses an 8,192-token window, Q4 target KV, one worker,
four CPU threads, temperature zero, thinking disabled, and a 2,304-token coding
generation cap. DFlash uses Q8 draft KV and three draft tokens. Baseline and
DFlash run serially against the same target and prompt.

The first **baseline** attempt was stopped during model loading when macOS
pressure reached critical (4). Observed worker-tree RSS peaked at 5,165.84 MiB;
minimum reported system free memory was 23%. These are different metrics:
RSS is not total Metal/unified memory pressure. No inference finished in
that initial attempt.

The local runner stops above 14 GiB worker-tree RSS, at critical system memory
pressure, or below 12% reported free memory. Existing unrelated System1
workers were left running. Permission was requested to pause and restore
them before retrying; the safeguard was not bypassed. Production workloads
and engine registrations were not changed. The single-model workload has not
been published or certified: the community archive source also needs an
honest installation recipe before automatic Mica setup can reproduce it.

Build log and the latest attempts' exact command/JSON are retained locally
under the ignored `build/bonsai-spec-test/` directory. The test runner is
`build/bonsai-spec-test/run_text.py`, and outputs are in its `results/` folder.

### Retry after memory pressure returned to normal

The baseline was retried without pausing unrelated servers. It passed the
arithmetic check (`391`) and generated 2,304 coding tokens with no reasoning
content. DFlash was then attempted serially, after the baseline worker exited.

| Mode | Actual input tokens | Generated tokens | Decode tokens/s | Peak worker-tree RSS GiB | Outcome |
| --- | ---: | ---: | ---: | ---: | --- |
| Original Bonsai PQ2, no drafter | 124 | 2,304 | 9.556 | 4.951 | Length/arithmetic smoke checks passed |
| Same target + separate Bonsai DFlash2 Q8 | — | — | — | 9.449 | Critical-pressure guard stopped model loading |

Baseline prompt processing took 2.791 seconds, decode took 241.000 seconds,
and the complete coding request took 243.803 seconds. Output reached its cap;
these checks do not certify a complete, correct parsing module. The 8,192-token
window was reserved, not filled.

The DFlash attempt was stopped after about three seconds, at system pressure
4 and 13% reported free memory. It did not reach readiness or generate any
tokens. Therefore **no DFlash speedup, acceptance rate, or quality comparison
has been measured**. Both runs used no projector or MTP. After cleanup, system
pressure returned to normal. The two unrelated System1 servers were still
running; permission to pause and restore them was still needed at that checkpoint for a retry with
more memory headroom. No unrelated process was stopped.

The retry supersedes the baseline `none.json`/`none.log` files; the initial
pressure-stop observation is recorded above. Exact requests and raw responses
for the successful baseline, plus `dflash.json`/`dflash.log` for the stopped
drafter attempt, remain in `build/bonsai-spec-test/results/`.

### Retry after stopping the old System1 servers

The user subsequently authorized stopping the two old local System1 workers
on ports 18158 and 18159. Both workers exited and the ports were confirmed
closed; no model files were deleted. Docker, the frontend, and the ongoing
Linux benchmark were left untouched. The old workers are not automatically
restored, because the user requested stopping them rather than pausing them.

The same DFlash test was retried with all safeguards unchanged. This time the
original PQ2 target and separate DFlash2 Q8 drafter loaded successfully. The
arithmetic check returned `391`, with three drafted tokens accepted. The
2,304-token coding generation started successfully. The previous pressure-stop JSON/log were preserved as
`first-pressure-stop-dflash.json` and `first-pressure-stop-dflash.log`.

The user then requested stopping local inference and conducting experiments
only on Linux for the remainder of the day. The DFlash worker was stopped
after 1,369 generated tokens; its last logged cumulative rate was 5.68 tokens/s.
This is a **partial, user-interrupted measurement**, not a completed 2,304-token
benchmark or a final coding-quality/acceptance result. The code request did not
return its final response. SIGTERM closed the listener but left cleanup stuck,
so the same verified worker was subsequently killed to release its allocations.
Local ports 8109 and 8092 were confirmed closed, and the local Mica launch job
was unloaded to prevent a restart. No model/cache files were deleted. The Linux
benchmark service remained active and untouched.
