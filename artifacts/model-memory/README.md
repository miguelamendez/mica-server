# Model memory evidence

The three model manifests reference this directory. Reports contain bytes and
GiB for component disk sizes, declared source revisions and measurement status.
Disk sizes are **not** RAM requirements. Never use a `guard_stopped` row as an
actual memory measurement.

## Current workload settings

The coding workload currently chooses these policies; they are not inherent
model capabilities or verified maximum-memory figures:

| Model / selected artifact | Input budget | Output reserve | Total | Requested KV |
| --- | ---: | ---: | ---: | --- |
| Spark-X2.5-4B / MLX Q4 | 65,536 | 16,384 | 81,920 | Q8 |
| Ling-3.0-Tiny / GGUF Q4_K_M | 65,536 | 16,384 | 81,920 | K Q8_0 / V Q8_0 |
| Qwen3.5-4B / GGUF Q4_K_M | 65,536 | 16,384 | 81,920 | K Q8_0 / V Q8_0 |

## Native-context profiling targets

The first native-limit attempts use a 16,384-token explicit output reserve,
not a verified trained-output claim. No models were loaded: macOS reported
warning memory pressure (level 2), and every Q4/Q8 run stopped at admission.

| Model | Declared total context | Prefill target | Output reserve | Q4/Q8 measurements |
| --- | ---: | ---: | ---: | --- |
| Spark-X2.5-4B | 1,048,576 | 1,032,192 | 16,384 | Pending: pressure safeguard |
| Ling-3.0-Tiny | 131,072 | 114,688 | 16,384 | Pending: pressure safeguard |
| Qwen3.5-4B | 262,144 | 245,760 | 16,384 | Pending: pressure safeguard |

A 4,096-input/256-output smoke attempt also stopped before model loading.
There are therefore **no measured loaded-model, full-cache or projector RAM
values yet**. Qwen's selected bundle contains a separate F16 vision projector;
the three selected artifacts contain no standalone drafter files.

After memory pressure returns to normal, validate a small context first, then
the workload's 81,920-token capacity. Attempt native maxima only after checking
the measured growth and memory headroom. Do not bypass the safeguard merely to
fill this table. See [the profiler guide](../../docs/model-memory-profiling.md).
