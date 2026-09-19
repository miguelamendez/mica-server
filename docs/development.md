# Development, validation, and releases

Mica is a research prototype under active deslopification. A passing unit suite
does not certify model quality, latency, memory behavior, or an engine on
hardware that was not actually tested.

## Build and test

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel 2
ctest --test-dir build --output-on-failure -j2
```

The two-job limit is deliberate. Native inference runtimes have large
translation units, and Mica caps compilation at 16 GiB to avoid making a
consumer laptop unusable.

The test matrix covers profile validation, hardware-derived engine flags,
quantization plans, memory admission, API authentication, model IDs, and
backend-specific paths. Real model acceptance additionally requires smoke
inference through the public route.

## Endpoint acceptance

After starting a configured server:

```sh
python3 scripts/mica_endpoint_acceptance.py \
  --api-key-file "$HOME/.mica/secrets/api-key" \
  --text-model spark-x25-4b \
  --output /tmp/mica-endpoint-acceptance.json
```

The browser playground is itself an integration client: text chat, voice,
attachments, streaming, TTS, sessions, import/export, and Markdown all use the
public server API.

## Benchmark matrix

`scripts/benchmark_matrix.py` covers reasoning, coding, general knowledge,
creativity, and summarization at 512, 1K, 2K, 4K, 8K, and 16K input lengths,
with configurable output length and concurrency.

```sh
python3 scripts/benchmark_matrix.py --help
python3 scripts/openai_batch_smoke.py --help
```

Measured evidence is stored under [`artifacts/benchmarks`](../artifacts/benchmarks)
and explained in [`docs/validation`](validation/). Do not generalize Apple M4
results to CUDA, ROCm, XPU, TPU, or CPU hosts.

## Release binaries

Create and test a static-Lua build locally:

```sh
lua_prefix=$(brew --prefix lua)
cmake -S . -B build-release \
  -DCMAKE_BUILD_TYPE=Release \
  -DMICA_LUA_INCLUDE_DIR="$lua_prefix/include/lua" \
  -DMICA_LUA_LIBRARY="$lua_prefix/lib/liblua.a"
cmake --build build-release --parallel 2
ctest --test-dir build-release --output-on-failure -j2
./scripts/package_release.sh build-release v0.1.0 dist
```

Pushing a `v*` tag triggers
[`release.yml`](../.github/workflows/release.yml), which builds and tests:

- macOS ARM64;
- Linux x86-64;
- Linux ARM64.

Each GitHub Release receives a tarball and SHA-256 file for every successful
platform. A platform artifact is not published if its build or tests fail.

## Troubleshooting

### Readiness does not complete

Inspect `~/.mica/logs/` and `GET /admin/models`. First launch may still be
downloading or smoke-testing. A failed required smoke test keeps the server
unready.

### 401 Unauthorized

Use the key produced by the same application home used for setup and serve.
The default is `~/.mica/secrets/api-key`.

### Model or quantization unavailable

Inspect `GET /v1/models`. Only variants selected by the active profile can be
requested.

### Memory-budget error

Choose Q4, select a smaller profile, or increase `--ram-gib` and run setup
again. Mica never evicts an in-flight worker. The reservation is a deterministic
admission limit, not an OS cgroup; transient allocations can exceed estimates.

### Download failure

Confirm the curated repository exists and authenticate through the isolated
`~/.mica/environments/tools/bin/hf` executable for private repositories.

## Security notes

- Mica binds to loopback by default. Do not expose it publicly without TLS,
  network controls, rate limits, and proper secret management.
- The generated API key is mode `0600` and is never printed by setup.
- Session-media routes reject paths outside the selected session.
- Imported archives reject traversal paths, symlinks, duplicates, malformed
  manifests, and oversized payloads.
- Attachments are untrusted model input.
- Never commit tokens, `~/.mica`, private conversations, or model weights.

See [architecture decisions](design/architecture-decisions.md) for the design
rationale and current implementation gaps.
