#!/usr/bin/env bash
set -euo pipefail

if [[ $# -ne 2 ]]; then
  echo "usage: test_api_auth.sh MICA_SERVER CONFIG_DIR" >&2
  exit 2
fi

server_binary=$1
config_directory=$2
test_root=$(mktemp -d "${TMPDIR:-/tmp}/mica-auth-test.XXXXXX")
server_pid=""

cleanup() {
  if [[ -n "$server_pid" ]]; then
    kill "$server_pid" 2>/dev/null || true
    wait "$server_pid" 2>/dev/null || true
  fi
  rm -rf "$test_root"
}
trap cleanup EXIT

mkdir -p "$test_root/mica-server"
token_file="$test_root/input-token"
printf '%s\n' 'mica_test_token_0123456789abcdef' > "$token_file"
chmod 600 "$token_file"

cat > "$test_root/mica-server/runtime.json" <<'JSON'
{
  "schema": 4,
  "installed_backends": ["gguf"],
  "quantizations": ["q4"],
  "default_quantization": "q4",
  "profile": "gguf-low-memory",
  "configured_models": {
    "spark-x25-4b": {"enabled": true, "variants": {"gguf": ["q4"]}},
    "granite-speech-5": {"enabled": true, "variants": {"gguf": ["q4"]}},
    "audio8-tts-06b": {"enabled": true, "variants": {"gguf": ["q4"]}},
    "minicpm-v46-thinking": {"enabled": true, "variants": {"gguf": ["q4"]}}
  },
  "max_ram_gib": 6,
  "max_vram_gib": 0,
  "vllm_device": "cpu",
  "api_key_file": "/intentionally/unused/by-override",
  "downloads": {}
}
JSON

# The authentication route does not need inference. Empty cache fixtures keep
# the background prewarmer offline and make this QA deterministic.
mkdir -p \
  "$test_root/checkpoints/gguf/spark-x25-4b" \
  "$test_root/checkpoints/gguf/granite-speech-5" \
  "$test_root/checkpoints/gguf/audio8-tts-06b" \
  "$test_root/checkpoints/gguf/minicpm-v46-thinking"
: > "$test_root/checkpoints/gguf/spark-x25-4b/Spark-X2.5-4B-Q4_K_M.gguf"
: > "$test_root/checkpoints/gguf/granite-speech-5/granite-speech-5.0-470m-turboctc-q4_k.gguf"
: > "$test_root/checkpoints/gguf/audio8-tts-06b/audio8-tts-preview-0.6b-q4_0.gguf"
: > "$test_root/checkpoints/gguf/minicpm-v46-thinking/MiniCPM-V-4_6-Thinking-Q4_K_M.gguf"
: > "$test_root/checkpoints/gguf/minicpm-v46-thinking/mmproj-model-f16.gguf"
for model in spark-x25-4b granite-speech-5 audio8-tts-06b minicpm-v46-thinking; do
  : > "$test_root/checkpoints/gguf/$model/.mica-complete-q4"
done

port=$((22000 + ($$ % 20000)))
"$server_binary" serve \
  --root "$test_root" \
  --config-dir "$config_directory" \
  --api-key-file "$token_file" \
  --host 127.0.0.1 \
  --port "$port" > "$test_root/server.log" 2>&1 &
server_pid=$!

healthy=false
for _ in {1..80}; do
  if curl --silent --fail "http://127.0.0.1:$port/health" >/dev/null; then
    healthy=true
    break
  fi
  if ! kill -0 "$server_pid" 2>/dev/null; then
    cat "$test_root/server.log" >&2
    exit 1
  fi
  sleep 0.05
done
if [[ "$healthy" != true ]]; then
  cat "$test_root/server.log" >&2
  exit 1
fi

status=$(curl --silent --output /dev/null --write-out '%{http_code}' \
  "http://127.0.0.1:$port/admin/models")
[[ "$status" == "401" ]]

status=$(curl --silent --output /dev/null --write-out '%{http_code}' \
  -H 'Authorization: Bearer wrong_token_0123456789' \
  "http://127.0.0.1:$port/admin/models")
[[ "$status" == "401" ]]

status=$(curl --silent --output /dev/null --write-out '%{http_code}' \
  -H 'Authorization: Bearer mica_test_token_0123456789abcdef' \
  "http://127.0.0.1:$port/admin/models")
[[ "$status" == "200" ]]

echo "API bearer-token authentication passed"
