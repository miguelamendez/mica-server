# Memory estimates and residency strategies

Mica's native C++ estimator does not load weights or run inference. Model
manifests describe device-independent component storage in decimal **MB**
(1,000,000 bytes). Budgets use **GiB** (1,073,741,824 bytes). Placement is
resolved separately against machine policy.

```sh
mica-server memory model spark-x25-4b
mica-server memory workload mac_coder --budget-gib 16 --global-limit-gib 16
```

The model report includes 8K, 16K, 32K, **68K**, and 128K total-token
checkpoints (K = 1024), and independent Q4/Q8 K/V combinations. Unsupported
contexts have no cache estimate. Short-context models also include their
native checkpoint. ASR/diarization without a decoder report no decoder KV;
their working memory depends on audio length and batching instead.

## Model data

Each artifact file has `size_bytes` and `estimated_memory_mb`. The latter is
currently a storage-based resident-weight proxy, **not a measured allocation**.
MLX bundles can contain vision/codec weights in the same directory; they
cannot be separated without tensor inventories. GGUF projector and external
drafter files are listed separately. Tokenizer/config/license files are not
counted as resident tensor weights.

Artifacts have `memory_estimate` with provenance, `kv_status`, cache groups,
fixed recurrent state, caveats and generated checkpoint tables. Groups specify
caching layer count and per-layer K/V row widths, not parameter count or
query-head count. Zero V width represents an MLA latent cache. Sliding groups
can specify a verified storage capacity; otherwise they conservatively reserve
the full window. Native interleaved SWA groups can use `sliding_window_tokens`:
allocation is the window plus the configured micro-batch, capped by context
and padded to 256. Generic model checkpoint tables assume a 512-token
micro-batch; workload reports use the worker's configured size. A group's fixed
precision handles protected layers.

For ordinary attention, each width is `KV heads × head dimension`:

```text
KV bytes = sum over groups:
  allocated positions × sequences × layers × (K row bytes + V row bytes)
```

llama.cpp Q4_0 uses 18 bytes per 32 values; Q8_0 uses 34 bytes per 32,
including scales. F16/BF16 uses two bytes per value. Rows and positions are
rounded conservatively. Requested precision is not engine compatibility
certification: protected layers, recurrent state and padding may differ by
engine version. Current MLX predictions conservatively reserve full sliding
windows; verified rotating-cache allocation can be represented as a capacity.

The estimator adds component storage, live KV, fixed state and a conservative
workspace floor derived from the existing artifact reservation. Workloads add
host prompt-cache storage and their safety reserve. Unknown geometries are
marked provisional. Reports are recalculated from source metadata; saved
checkpoint tables are display snapshots, not admission inputs.

## Workload policy

Any workload can select the same reusable strategy without changing its
collection of models:

```yaml
memory:
  limit_gib: 16
  strategy: balanced
  keep_resident: [spark-x25-4b]
  safety_reserve_gib: 0.5
```

| Strategy | Estimated requirement | Runtime behavior |
| --- | --- | --- |
| `sequential` | Largest configured worker + safety | One resident worker; unload before loading another |
| `all` | Sum of all workers + safety | All workers start and remain resident |
| `balanced` | Retained set + largest remaining worker + safety | Retained workers stay pinned; priority and idle policy manage the others |

`keep_resident` defines the balanced retained set. Without it, explicitly
pinned models define that set. Priority controls warmup and eviction; it does
not itself mean “never unload.” Balanced may opportunistically keep more
optional workers if there is room. Its reported minimum can serve every model
while respecting the retained set. Empty retained sets allow fully dynamic
priority-based swapping and have the same minimum as sequential.

Reports show **all three** estimates and which fit. A workload's `limit_gib`
must not exceed global allocation. Setup, startup and hot-swap reject an
oversized explicit strategy; no silent budget increase or strategy change.
Separate machine RAM/device limits still apply in addition to the workload's
combined worker ceiling. Unified memory is counted once. Legacy workloads
without a strategy retain their existing policy. Optional legacy
`required_ram_gib`/`required_vram_gib` remain explicit additional requirements;
new strategy-based workloads can omit them and use calculated admission.

Independent workers are counted separately even when they reference identical
files: OS page sharing is not guaranteed. Actual engine-provided KV sharing
must be represented explicitly in cache groups. Speculative method `none`
excludes drafter components; a drafter with unverified cache layout remains
provisional. Context includes templates, media tokens, history and reasoning,
not just the newest user text, plus the reserved output budget.

Estimates are not OS-enforced peak caps. Runtime monitoring and
[real component measurements](model-memory-profiling.md) remain necessary for
transient peaks, media preprocessing and speculative decoding validation.

Non-causal embedding models have no persistent generation KV cache, but their
attention and media workspace still depends on input size and batching. The
absence of a KV cache does not make 128K inputs valid: EmbeddingGemma-2's
publisher budget is 8192 shared input tokens. Its larger GGUF architectural
position limit is not used as a supported inference ceiling.

## References

- [llama.cpp cache allocation](https://github.com/ggml-org/llama.cpp/blob/master/src/llama-kv-cache.cpp)
- [GGML quantization block sizes](https://github.com/ggml-org/llama.cpp/blob/master/ggml/src/ggml-common.h)
- [Recurrent-state dimensions](https://github.com/ggml-org/llama.cpp/blob/master/src/llama-hparams.cpp)
