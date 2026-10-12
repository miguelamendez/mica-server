# Update Mica from source

Update the binary **and its packaged configuration files together**. A Git pull
alone does not update the binary on PATH or a running server's registry. These
commands preserve user-owned settings, API keys, models and engines in `~/.mica`.

## Linux checkout at `~/repos/mica`

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
mica-server start --workload gpu_16g_coder --host 0.0.0.0 --port 8092
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
mica-server config set --port 8092 --restart
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
