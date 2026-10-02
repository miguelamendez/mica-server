#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 3 ]]; then
  echo "usage: test_profile_swap.sh MICA_SERVER CONFIG_DIR FIXTURE_DIR" >&2
  exit 2
fi

server_binary=$1
config_directory=$2
fixture_directory=$3
test_root=$(mktemp -d "${TMPDIR:-/tmp}/mica-swap-test.XXXXXX")
server_pid=""
cleanup() {
  if [[ -n "$server_pid" ]]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT

mkdir -p "$test_root/state" "$test_root/config/profiles" \
  "$test_root/runtimes/llama.cpp/build-mica/bin" \
  "$test_root/models/gguf/spark-x25-4b"
cp "$fixture_directory/swap-a.yaml" "$fixture_directory/swap-b.yaml" \
  "$test_root/config/profiles/"
: > "$test_root/runtimes/llama.cpp/build-mica/bin/llama-server"
: > "$test_root/models/gguf/spark-x25-4b/Spark-X2.5-4B-Q4_K_M.gguf"
printf '%s\n%s\n' 'miguelamendez/mica-spark-x25-4b' \
  '6c242d2945f11bb3587c437a1bf1088666ca6e2f' > \
  "$test_root/models/gguf/spark-x25-4b/.mica-complete-q4"

cat > "$test_root/state/runtime.json" <<'JSON'
{
  "schema": 6,
  "installed_backends": ["gguf"],
  "quantizations": ["q4"],
  "default_quantization": "q4",
  "profile": "swap-a",
  "profile_schema": 4,
  "profile_mode": "interactive",
  "profile_required_ram_gib": 6,
  "profile_required_vram_gib": 0,
  "profile_memory_safety_reserve_gib": 0.25,
  "profile_maximum_resident_workers": 1,
  "configured_models": {
    "spark-x25-4b": {
      "enabled": true,
      "variants": {"gguf": ["q4"]},
      "engine": "llama-cpp",
      "execution": "",
      "residency": "on-demand",
      "priority": 100,
      "startup": false,
      "idle_seconds": 60,
      "max_input_tokens": 7168,
      "max_output_tokens": 1024,
      "max_total_tokens": 8192,
      "max_concurrent_requests": 1,
      "kv_cache_precision": "q8",
      "placement_mode": "auto",
      "device": "auto",
      "gpu_layers": -1,
      "ram_reservation_gib": -1,
      "vram_reservation_gib": -1
    }
  },
  "max_ram_gib": 8,
  "max_vram_gib": 0,
  "gguf_target": "cpu",
  "audio_target": "cpu",
  "vllm_device": "cpu",
  "api_key_file": "/intentionally/unused/by-override",
  "downloads": {}
}
JSON

port=$((22000 + ($$ % 20000)))
cat > "$test_root/server.json" <<JSON
{"host":"127.0.0.1","port":$port,"api_key":"mica_swap_test_token_0123456789"}
JSON
"$server_binary" serve --root "$test_root" --config-dir "$config_directory" \
  --server-config "$test_root/server.json" > "$test_root/server.log" 2>&1 &
server_pid=$!

ready=false
for _ in {1..100}; do
  if curl --silent --fail "http://127.0.0.1:$port/ready" >/dev/null; then
    ready=true
    break
  fi
  if ! kill -0 "$server_pid" 2>/dev/null; then
    cat "$test_root/server.log" >&2
    exit 1
  fi
  sleep 0.05
done
[[ "$ready" == true ]]

response=$(curl --silent --show-error --fail \
  -H 'Authorization: Bearer mica_swap_test_token_0123456789' \
  -H 'Content-Type: application/json' \
  -d '{"profile":"swap-b"}' \
  "http://127.0.0.1:$port/admin/profile/activate")
[[ "$response" == *'"profile":"swap-b"'* ]]
[[ "$response" == *'"previous_profile":"swap-a"'* ]]

ready=false
for _ in {1..100}; do
  if curl --silent --fail "http://127.0.0.1:$port/ready" >/dev/null; then
    ready=true
    break
  fi
  sleep 0.05
done
[[ "$ready" == true ]]
admin=$(curl --silent --show-error --fail \
  -H 'Authorization: Bearer mica_swap_test_token_0123456789' \
  "http://127.0.0.1:$port/admin/models")
[[ "$admin" == *'"name":"swap-b"'* ]]
[[ "$(< "$test_root/state/runtime.json")" == *'"profile": "swap-b"'* ]]
echo "Live task activation and persisted profile passed"
