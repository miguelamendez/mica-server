# Updating Mica

Update the binary **and its packaged configuration files together**. A Git pull
alone does not update the binary on PATH or a running server's registry. These
commands preserve user-owned settings, API keys, models and engines in `~/.mica`.

Current bundles include `mica-client` and its matching HTML/Lua assets as well as
`mica-server` and native Dictation. Updates stage and back up those binaries
together when supplied by the payload. Keep shared files with their matching
binary versions; a server restart does not restart separate app processes.
Client connections in `~/.mica/client` are not overwritten. See [Client](client.md).

New server defaults use port 55000, client UI 55001. Existing explicit server
ports are preserved; see [port migration and pools](ports.md).

## One-command updates

```sh
mica-server update --check
mica-server update --restart
mica-server update --channel source --restart
```

The default `release` channel selects the latest published release, including
alpha releases, and prefers its matching macOS/Linux ARM64/x86-64 archive and
SHA-256 file. If that release has no compatible binary, Mica compiles its pinned
source revision. If no release exists, it falls back to the current `main`
revision. The `source` channel always selects a pinned `main` commit instead.
A release older than your installed source build is not installed automatically.

The updater stages and validates the candidate binary **and matching manifests**
before stopping a running server. It validates archive paths and rejects links
and special files. A bad checksum aborts; it does not silently switch to source.
A binary that cannot run on the host ABI can fall back to the same source
revision. Source compilation needs CMake, a C++20 toolchain, Git and Lua
development files. No Python updater/runtime environment is created.

`--restart` preserves the active workload. Without it, stop a running Mica first.
When Mica is already stopped, updating does not start it. Previous installation
files are retained as `.mica-server.previous-TRANSACTION` beside `bin/mica-server`
and `share/mica-server`; staging lives under `~/.mica/updates/`. User settings,
credentials, model caches, engines and user-owned workloads are not replaced.
Restart failures are reported, not described as success. Close/reopen an existing
TUI after installation so it uses the new binary.
`mica-server --version`, Server's **TUI build / Server build**, and `status`
make it possible to distinguish the installed CLI, an already-open TUI and the
running server. Dashboard assets are served without browser caching.

`--prefix PATH` selects a user-writable installation prefix; no automatic sudo
or account changes are performed. `--force` explicitly permits reinstalling a
selected revision/replacing a dirty installed build. It is not needed normally.
The updater uses a separate source checkout under `~/.mica/updates/`; it never
resets or modifies your development checkout.

In the TUI, **Server → Updates** or **Settings → Check for updates** checks the
repository. Settings has separate release/source install actions with confirmation.
Checks are explicit, not hidden startup network traffic. The cached check is
shown with its timestamp/current revision; source builds also show their memory
admission result.

## Build memory safety

Both self-update source builds and native inference-engine compilation use the
same C++ guard. It uses live memory, not just the machine's physical capacity:

```text
system reserve = max(2 GiB, 20% of usable total RAM)
build cap = min(machine build limit, 16 GiB, 50% of total RAM,
                available memory - system reserve - 0.5 GiB margin)
```

Builds are refused below a 2 GiB cap or at macOS warning/critical pressure.
Parallel jobs are bounded by machine policy, half the CPU cores, two jobs, and
a 2 GiB/job allowance. The guard rechecks current system headroom and compiler
process-group RSS every 250 ms and terminates the compiler group if either
budget or pressure threshold is exceeded. Ctrl-C/termination is forwarded to
the compiler group. This is a sampling safeguard, not an OS-enforced hard cap.

Linux uses `MemAvailable`, narrowed by detected cgroup-v2 limits. macOS uses a
conservative free/reclaimable-page estimate and the unified-memory pressure
indicator, so GPU allocations are not counted as a second independent pool.
"Used or unavailable" is total minus that availability estimate, not the sum
of app RSS. Low headroom should be resolved by freeing other applications/models
and retrying, not by bypassing the guard.

## Bootstrap older versions / developer source builds

Older binaries without `update` need this one-time upgrade before they can use
the commands above. The following example uses the existing Linux checkout at
`~/repos/mica`. Regression tests are a developer check, not part of normal
release installation.

Commit or preserve local changes first; do not reset the worktree.

```sh
cd "$HOME/repos/mica"
mica-server stop
git status --short
git pull --ff-only origin main
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
python3 scripts/run_memory_limited.py --limit-gib 8 -- cmake --build build --parallel 1
ctest --test-dir build --output-on-failure --parallel 1
cmake --install build --prefix "$HOME/.local"
export PATH="$HOME/.local/bin:$PATH"
hash -r
mica-server --version
mica-server workload list
mica-server start
mica-server status
mica-server tui
```

The Python watchdog is an offline build tool, not a GGUF serving dependency.
It monitors aggregate process-tree RSS; sampling is a safeguard, not an
OS-enforced hard memory boundary. The build uses one compiler job. For missing
development packages see [getting started](getting-started.md#build-from-source).
`git pull --ff-only` refuses divergence rather than overwriting local work.
Starting reconciles the configured default workload with current manifests.

To select the CUDA Qwen27B coding workload explicitly:

```sh
mica-server stop
mica-server setup --profile gpu_16g_coder --ram-gib 16
mica-server start --workload gpu_16g_coder --host 0.0.0.0 --port 55000
```

This workload also requires 15.5 GiB of dedicated GPU allocation and the CUDA
device enabled in `machine.yaml`. `--ram-gib 16` neither enables GPU use nor sets
its allocation. Inspect `mica-server config show` and
`mica-server plan --profile gpu_16g_coder` first. Use a trusted LAN/firewall when
listening on all interfaces.

## Refresh inference engines

Updating Mica does not automatically rebuild a cached engine on every start.
An engine's `revision: latest` is resolved at installation or explicit refresh:

```sh
mica-server stop
mica-server setup --profile gpu_16g_coder --ram-gib 16 --refresh
mica-server start --workload gpu_16g_coder
```

Refresh may rebuild engines and recheck downloads; it is not a cache deletion
command. User workloads in `~/.mica/config/profiles/` override packaged examples
with the same ID and are not overwritten by distribution installation.

## Change settings and restart

```sh
mica-server config set --port 55000 --restart
```

When running, `--restart` validates proposed values, stops Mica, saves them and
restarts the same active workload. A changed default workload applies to later
ordinary starts. When stopped, the command only saves settings. The TUI uses
this operation after confirmation, including API-key import/rotation.

Invalid values do not interrupt a running server. If the active workload cannot
restart under the new limits, Mica reports that settings were saved; correct
them and start again. Changes cause brief downtime and may interrupt requests.
Clients must update credentials after key rotation.

## Tool-calling discovery

Tool support belongs to the model manifest, not individual workloads:

```yaml
abilities: [text_generation, instruction_following, tool_calling]
supported_tasks: [chat, coding, tool_calling]
tool_call_formats: [openai]
tool_call_parsers: {vllm: qwen3_coder}
```

The model's tool-aware chat template and an engine that converts its output to
OpenAI `tool_calls` are both needed. Template syntax alone is not a quality
guarantee. Mica adds `--jinja` for tool-capable llama.cpp workers. vLLM uses the
declared parser with `--enable-auto-tool-choice`; MLX-VLM infers its parser from
the model's processor/template.

After restart, `/v1/models` exposes `tool_call_formats`, parser metadata and
effective `supports_tools` for the selected model/engine pair.
`endpoint_contracts[].engine_supports_tools` reports the engine contract
separately. Pi should use its `openai-completions` provider API with a base URL
ending in `/v1`, not an unsupported Responses API. Updating Mica does not
change Pi's accounts, models or authentication settings.
