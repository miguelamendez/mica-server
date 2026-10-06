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

mkdir -p "$test_root/state"
token_file="$test_root/input-token"
printf '%s\n' 'mica_test_token_0123456789abcdef' > "$token_file"
chmod 600 "$token_file"

cat > "$test_root/state/runtime.json" <<'JSON'
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

# The authentication route does not need inference. Minimal cache fixtures
# carry repository/revision markers so the background prewarmer stays offline.
mkdir -p \
  "$test_root/models/gguf/spark-x25-4b" \
  "$test_root/models/gguf/granite-speech-5" \
  "$test_root/models/gguf/audio8-tts-06b" \
  "$test_root/models/gguf/minicpm-v46-thinking"
: > "$test_root/models/gguf/spark-x25-4b/Spark-X2.5-4B-Q4_K_M.gguf"
: > "$test_root/models/gguf/granite-speech-5/granite-speech-5.0-470m-turboctc-q4_k.gguf"
: > "$test_root/models/gguf/audio8-tts-06b/audio8-tts-preview-0.6b-q4_0.gguf"
: > "$test_root/models/gguf/minicpm-v46-thinking/MiniCPM-V-4_6-Thinking-Q4_K_M.gguf"
: > "$test_root/models/gguf/minicpm-v46-thinking/mmproj-model-f16.gguf"
# These markers must match the immutable revisions in model-manifests. The
# authentication test must stay offline even when the registry is updated.
printf '%s\n%s\n' 'miguelamendez/mica-spark-x25-4b' '6c242d2945f11bb3587c437a1bf1088666ca6e2f' > \
  "$test_root/models/gguf/spark-x25-4b/.mica-complete-q4"
printf '%s\n%s\n' 'miguelamendez/mica-granite-speech-5' '1bd2ab6e8b006062f6336a0ab15d4707295f932d' > \
  "$test_root/models/gguf/granite-speech-5/.mica-complete-q4"
printf '%s\n%s\n' 'miguelamendez/mica-audio8-tts-06b' '610e0202c26f581678e5544aae2be2ef3f71adb9' > \
  "$test_root/models/gguf/audio8-tts-06b/.mica-complete-q4"
printf '%s\n%s\n' 'miguelamendez/mica-minicpm-v46-thinking' 'dfad66c29f50dc9b0545baf9e9ec3a67c1c8bd7d' > \
  "$test_root/models/gguf/minicpm-v46-thinking/.mica-complete-q4"

port=$((22000 + ($$ % 20000)))
server_config="$test_root/server.json"
cat > "$server_config" <<JSON
{
  "host": "127.0.0.1",
  "port": $port,
  "api_key": "mica_test_token_0123456789abcdef"
}
JSON
"$server_binary" serve \
  --root "$test_root" \
  --config-dir "$config_directory" \
  --server-config "$server_config" > "$test_root/server.log" 2>&1 &
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

curl --silent --fail \
  -H 'Authorization: Bearer mica_test_token_0123456789abcdef' \
  "http://127.0.0.1:$port/v1/models" > "$test_root/models.json"
python3 - "$test_root/models.json" <<'PY'
import json, sys
document = json.load(open(sys.argv[1]))
assert document["data"], "Active workload must expose its models"
for model in document["data"]:
    assert model["endpoint_contracts"], "Expose the selected engine's contracts"
    for contract in model["endpoint_contracts"]:
        assert isinstance(contract["streaming"], bool)
        assert isinstance(contract["supports_tools"], bool)
spark = next(model for model in document["data"] if model["id"].startswith("spark-x25-4b"))
assert any(c["operation"] == "chat.generate" and c["supports_tools"]
           for c in spark["endpoint_contracts"])
PY

status=$(curl --silent --output /dev/null --write-out '%{http_code}' \
  -H 'Content-Type: application/json' \
  -d '{"profile":"gguf-low-memory"}' \
  "http://127.0.0.1:$port/admin/profile/activate")
[[ "$status" == "401" ]]

kill "$server_pid"
wait "$server_pid" 2>/dev/null || true
server_pid=""

# An explicit argument overrides a configured key. This is supported for
# automation, although a key file is safer for normal use.
port=$((port + 1))
cat > "$server_config" <<JSON
{
  "host": "127.0.0.1",
  "port": $port,
  "api_key": "configured_but_overridden_0123456789"
}
JSON
"$server_binary" serve \
  --root "$test_root" \
  --config-dir "$config_directory" \
  --server-config "$server_config" \
  --api-key mica_test_token_0123456789abcdef > "$test_root/server-argument.log" 2>&1 &
server_pid=$!

healthy=false
for _ in {1..80}; do
  if curl --silent --fail "http://127.0.0.1:$port/health" >/dev/null; then
    healthy=true
    break
  fi
  if ! kill -0 "$server_pid" 2>/dev/null; then
    cat "$test_root/server-argument.log" >&2
    exit 1
  fi
  sleep 0.05
done
[[ "$healthy" == true ]]

status=$(curl --silent --output /dev/null --write-out '%{http_code}' \
  -H 'Authorization: Bearer mica_test_token_0123456789abcdef' \
  "http://127.0.0.1:$port/admin/models")
[[ "$status" == "200" ]]

echo "API bearer-token authentication passed (config and argument sources)"
