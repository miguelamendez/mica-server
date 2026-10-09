# Model-component memory measurements

For metadata-only planning, use the native
[memory estimator and residency strategies](memory-estimation.md). No inference
is needed for its context/cache tables; this offline profiler validates those
estimates against actual allocations.

`scripts/profile_model_memory.py` is an offline developer tool, not part of
Mica's native serving dependency chain. It measures one model artifact at a
time using an already installed engine and cached weights. It never downloads
models, installs engines, swaps workloads, or changes admission reservations.

Memory evidence belongs to the **model + artifact + engine version + hardware**.
The model manifest's optional `memory_profile` points to the script and report
directory. Context, placement and residency selected for a running workload
remain workload policy. Different configurations generate different evidence;
there is no universal "this model uses X GiB" number.

## Run

Use an existing Python environment with PyYAML (MLX's environment already has
it), or `uv run --with pyyaml`. GGUF serving itself still needs no Python.

```sh
uv run --with pyyaml scripts/profile_model_memory.py \
  --manifest config/model-manifests/spark-x25-4b.yaml \
  --manifest config/model-manifests/ling-3-tiny.yaml \
  --manifest config/model-manifests/qwen35-4b.yaml \
  --artifact q4 --kv-types q4 q8 \
  --input-tokens 65536 --output-tokens 16384 \
  --limit-gib 12 --output-dir artifacts/model-memory
```

This only creates a plan. Add `--execute` to measure, sequentially. The default
cache root is `~/.mica`; default output is
`~/.mica/measurements/model-memory`. Override `--cache-root`, `--mlx-python`,
`--llama-binary` or `--engine` when appropriate. Spark chooses its first Q4
artifact (MLX), Ling and Qwen choose GGUF. `--engine llama-cpp` selects Spark's
GGUF artifact instead, if cached.

Omit `--input-tokens` to use **recommended total context minus output reserve**,
falling back to native total context when no recommendation was verified.
If the manifest has no verified `max_output_tokens`, the output reserve
must be explicitly supplied. A measurement override is not a claim about the
model's trained output length. Native maxima can be expensive; start with
`--input-tokens 4096 --output-tokens 256` and inspect failures/peaks before
attempting a long-context run. Explicit input above a known recommended
window is reported as such and must still fit the native total context.
Reports at different budgets should be written
to separate output directories (otherwise the latest replaces the prior run).

## What is measured

1. Load the primary weights, then record process RSS and physical footprint on
   macOS, process VRAM on NVIDIA/Linux when available, and MLX device allocation.
2. Actually prefill the requested number of tokenizer IDs, not characters.
3. Fill the output-reserve positions using **teacher-forced prefill** to
   measure cache occupation at capacity. GGUF fills `total - 1` prompt positions
   and requests one prediction; its report states actual prompt occupation.
4. GGUF optionally performs a short real autoregressive decode. This does not
   establish the peak for generation of the entire reserved output budget.
5. For a GGUF projector, repeat with the same primary model and projector
   attached; report the incremental loaded memory, engine buffers, and image
   processing using a deterministic 768×768 PNG and a declared visual token
   ceiling. This is a paired component measurement, not a projector-only
   process or an image-at-full-context measurement. It does not cover video
   frame-count peaks; those require a separate workload validation.
6. Drafters listed in the artifact are attempted independently. A drafter that
   cannot run as a standalone model is reported as failed/unsupported, never
   assigned a fabricated resident size. Joint speculative decoding needs a
   separate mode-level validation. These three current artifacts contain no
   standalone drafter files.

MLX defaults to the `mlx-vlm` server's batch-cache builder with one sequence.
Use `--mlx-cache-path stream` to test the single-stream cache path separately;
their capabilities can differ (uniform rotating-cache quantization can be
unsupported on the single-stream path). MLX uses the installed engine's
quantization policy and reports the cache
class, actual bits, bytes and occupation for every layer. Sliding-window,
recurrent or protected layers may remain unquantized. Requested Q8 does not
mean every layer is Q8. GGUF records requested K and V types and the native
engine's allocation logs. Recurrent state is not automatically Q4/Q8 KV.

## Safety and interpretation

- One worker at a time. Engine children are terminated after each measurement.
  Existing servers are not stopped. Idle any resident models before testing;
  unrelated memory use can cause the pressure safeguard to abort a measurement.
- Default 12 GiB process-memory safeguard, maximum 16 GiB. MLX also receives
  allocator/wired-memory limits. A polling watchdog is **not an OS hard cap**:
  a fast allocation can briefly overshoot before termination.
- On macOS, stop on warning/critical memory pressure. On Linux, stop if less
  than 1 GiB system RAM remains. Monitoring errors fail closed. GPU metrics
  other than NVIDIA process VRAM or macOS physical footprint are not yet
  available; do not claim an enforced VRAM limit on unmeasured GPU runtimes.
- Disk bytes, RSS, physical footprint, engine buffers and MLX active memory
  are different metrics. **Do not add overlapping metrics together.**
- A failed, unsupported, timed-out or guard-stopped test is not a measurement
  of maximum-context viability. All statuses and actual occupied tokens remain
  in the report.
- Component values help estimate workload totals, with shared-allocation and
  workspace allowances. Validate the composed workload's observed peak before
  treating that estimate as a reliable admission budget.

The JSON reports are diagnostic evidence. Mica exposes their repository
references in the model catalog but does not automatically turn them into
runtime policy. Raw local logs can contain filesystem paths; share reviewed
JSON reports rather than publishing the entire measurement directory blindly.
