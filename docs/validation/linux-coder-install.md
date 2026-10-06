# Linux coding-workload installation

## Chat interface validation (2026-10-06)

The loopback-only API and optional stdlib chat UI run as separate user services,
`mica-server.service` and `mica-chat.service`. The UI listens on port 8090 and
forwards to API port 8092, reading the private key file rather than embedding
credentials in HTML. Remote browser access uses an SSH localhost tunnel.
Services are started in the background; reboot autostart is not enabled.

Workload-aware controls use `/v1/models` model interactions and selected-engine
endpoint contracts, refresh after swaps, and revalidate before sending. The
coding workload enables text, image/document, and video input, but disables ASR
recording and TTS controls. Model presence does not mean all capabilities are
usable: attachment tools also require a tool-capable main assistant and engine.

Both Mac and Linux regression suites passed 77/77. The frontend capability matrix
also passed on Mac (Node is optional for tests, not a runtime dependency).
The Linux incremental core build used one job and peaked at 1.14 GiB RSS under
a 4 GiB monitor. Real requests through the UI bridge succeeded: text completed
in 0.94 seconds; image tool inspection plus streamed final answer took 13.64
seconds including on-demand swaps. These are smoke checks, not benchmark rates.
The image observation correctly identified the two colored squares and read
the visible text; the final assistant summarized the squares but omitted the
requested text. No new video/voice quality certification is claimed here.

## Scope and status

The requested workload is `mica-coder-qwen-gguf`, not the older Bonsai/Ling
workload. It uses stock llama.cpp for all models. Prism is installed separately
for future Bonsai use; it is not required by this workload.

The target is Ubuntu 24.04 on a Ryzen 5 9600X, with approximately 60.5 GiB host
RAM and an RTX 5060 Ti reporting 15.93 GiB dedicated memory. Detection is
performed by Mica, not a separate Python hardware detector.

Machine policy permits CPU and CUDA device 0, allocates 16 GiB host RAM and
15.5 GiB VRAM to inference, and limits builds to one compiler job / 8 GiB.
The test harness additionally monitors build-process-tree RSS. These are not
claims that every backend has an OS-enforced GPU memory limit.

CUDA compiler 12.8.93 and development libraries were installed without
upgrading the working NVIDIA driver. Native Blackwell compilation is enabled.
The detector previously left physical CPU cores at zero on Linux. The fix
counts unique online (socket, core) pairs from `lscpu`, without counting SMT
threads twice. Its parser has tests for SMT, multiple sockets, and unavailable
topology. The rebuilt detector correctly reports six physical cores remotely.

The first live image test detected correct colors but hallucinated the large
printed text. Retrying without prompt-cache reuse did not fix OCR. The native
Qwen loader warns that at least 1024 image tokens may be needed for reliable
grounding; the default image was represented by only 300 visual tokens.
The artifact now requests `image_min_tokens: 1024`, passed to native
`llama-server --image-min-tokens`. The same image retest correctly reads
`MICA 31415` and identifies both colored squares. This is a fixture-level
improvement, not a claim that the quantization preserves all visual accuracy.

The acceptance client also exposed a startup race: systemd may return before
Mica binds the API socket. Readiness polling now retries connection-refused
and timeout errors within its deadline instead of misreporting a setup failure.

Raw-output review found another test weakness: checking for any occurrence of
`9` accepted a Qwen3.8 answer that started with `8` and then explained nine.
That non-thinking output is inconsistent, not a correct answer. The reasoning
case now enables thinking explicitly and requires the final answer to start
with `9`; the separate reasoning-enabled probe returned the coherent answer
`9 — “all but 9” means 9 sheep did not run away.` Vision tests keep thinking
disabled. The video task grades color order only, not time localization; the
earlier unconstrained response invented timestamps. Its prompt now requests
only color names, and timestamp accuracy is not certified.

## Workload

| Model | Artifact | KV cache | Startup | Context allocation |
| --- | --- | --- | --- | --- |
| Spark-X2.5-4B | Q4_K_M | Q8 | Default text model, warm first | 65,536 input + 16,384 output |
| Qwen3.8 27B GSQ-RCO | IQ3_XXS + BF16 projector | Q4 | On demand | 65,536 input + 16,384 output |
| Qwen3.5 9B | Q4_K_M + BF16 projector | Q8 | On demand; default image/video model | 65,536 input + 16,384 output |

Maximum residency is one worker. Mica must evict an idle model before loading
another, rather than trying to keep all three in VRAM. The large coder uses
Q4 KV cache because the conservative Q8 reservation at 81,920 total tokens
would exceed the allocated VRAM after the safety margin. Admission estimates
are not measured peak usage or long-context quality certification.

The workload downloads pinned, existing quantizations; no requantization is
performed. The Qwen bundles include their BF16 projectors. Model manifests
record byte sizes, SHA-256 checksums, revision IDs, and original sources.

## Reproduction

From a fresh checkout and installed Mica core:

```sh
export CUDACXX=/usr/local/cuda-12.8/bin/nvcc
export CUDAToolkit_ROOT=/usr/local/cuda-12.8
export PATH=/usr/local/cuda-12.8/bin:$PATH
mica-server plan --profile mica-coder-qwen-gguf --ram-gib 16 --vram-gib 15.5
mica-server setup --profile mica-coder-qwen-gguf --ram-gib 16 --vram-gib 15.5
mica-server serve --host 127.0.0.1 --port 8092
```

An existing `~/.mica/config/machine.yaml` must permit these allocations.
Setup does not silently raise machine limits. Models are downloaded on first
serve, before startup warmup. Authentication uses the private API-key file;
never commit it or embed its value in example commands.

## Acceptance checklist

- Core source build and native hardware detection: passed, including physical-core detection.
- Core test suite: 77/77 passed on macOS and on Linux after the final native change.
- New manifest/schema checks: passed locally (59 documents).
- Prism CUDA build: passed; binary detects the RTX 5060 Ti. Bonsai inference was not part of this workload.
- Stock llama.cpp CUDA build: passed; actual inference runs on CUDA device 0.
- New workload setup and model downloads: passed, 20.23 GB across five files;
  Qwen bundle SHA-256 checks passed, and Spark's pinned file-size check passed.
- Real Spark and Qwen coder inference: passed; Qwen reasoning uses thinking enabled.
- Real Qwen3.5 image/video inference and text streaming: passed.
- API authentication, default routing, and one-worker swaps: passed.
- Final live acceptance suite: 56/56 checks passed.
- Full 64K-input / 16K-generation quality and peak-memory certification: not tested.

The final raw prompts, outputs, timing data, swap results, and worker snapshot
are stored in
[`artifacts/linux-coder-acceptance-2026-10-05.json`](../../artifacts/linux-coder-acceptance-2026-10-05.json).
Older, weaker checks are not used as final acceptance evidence. Non-thinking
Qwen reasoning and video time localization retain the limitations noted above.

## Installed runtime and service

Stock llama.cpp resolved `latest` to
`5e03bdd8700948b9c41c54dd1b00f28a2aebc03f`; Prism uses
`9a9394a895b96003ca842a6041cb28ac49a108f7`. Both builds use CUDA 12.8,
`GGML_CUDA=ON`, and native `sm_120a` kernels. Combined engine-build process-tree
RSS peaked at 3.43 GiB under the 8-GiB build guard, with one compiler job.

The final active workload is `mica-coder-qwen-gguf`. Spark is the default and
resident worker; Qwen models load on demand. Model caches, machine policy,
hardware facts, API key, logs, and state live under `~/.mica`. This GGUF-only
setup did not create a Python environment. Python was used only by optional
build monitoring and acceptance clients.

The background server listens on **Linux loopback** `127.0.0.1:8092`, not a
public network interface. Its user-systemd unit is linked from
`~/.mica/state/systemd/mica-server.service`; logs are in
`~/.mica/logs/server.log`. The unit applies a 16-GiB cgroup host-memory ceiling
and disables swap for the service. Cgroup usage includes filesystem cache and
can transiently overshoot its accounting ceiling; it is not a VRAM cap. Mica's
VRAM allocation remains 15.5 GiB with reservation-based admission.

```sh
systemctl --user status mica-server.service
systemctl --user stop mica-server.service
systemctl --user start mica-server.service
curl http://127.0.0.1:8092/ready
```

The service stays running independently of the SSH session. It has not been
enabled for automatic reboot startup.

## Live workload-swap test

The authenticated switch route is `POST /admin/profile/activate`, with
`{"profile":"coder-qwen-swap-test"}` as its body. The active workload and
worker PIDs are visible through authenticated `GET /admin/models`.

The two-model fixture is
[`tests/fixtures/coder-qwen-swap.yaml`](../../tests/fixtures/coder-qwen-swap.yaml).
It has exactly the same engine, artifact, context, KV, and placement settings
for Spark and Qwen3.5 as the main three-model workload. Install it **before**
starting the server so the running registry knows the workload:

```sh
mica-server profile install-file tests/fixtures/coder-qwen-swap.yaml
```

After startup, run the optional standard-library acceptance client:

```sh
python3 scripts/coder_workload_acceptance.py \
  --api-key-file "$HOME/.mica/secrets/api-key" \
  --image artifacts/test-assets/minicpm-scene.png \
  --video artifacts/test-assets/minicpm-sequence.mp4 \
  --output "$HOME/.mica/state/coder-acceptance.json"
```

It compares retained worker PIDs across workload changes, verifies that the
larger Qwen coder is unloaded/excluded in the smaller workload, checks image
and video default routing, and exercises streamed text generation. It restores
the original workload on successful completion. If a check fails, inspect
the checkpoint and current active workload before resuming normal use.
