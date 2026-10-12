"""Client/MCP/native dictation integration with a mock remote Mica. No recording."""
import json
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
import unittest

BINARY, CONFIG = map(Path, sys.argv[1:3])
sys.argv = sys.argv[:1]
KEY = "test_inference_key_not_real_12345"


class Clients(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mica-client-test-")
        self.root = Path(self.temp.name)
        self.observed = []
        observed = self.observed
        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_): pass
            def reply(self, value):
                data = json.dumps(value).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers(); self.wfile.write(data)
            def do_GET(self):
                observed.append((self.path, self.headers.get("Authorization"), self.headers.get("X-Mica-Workload")))
                self.reply({"data": [{"id": "asr", "supported_interactions": [{"operation": "audio.transcribe"}], "endpoint_contracts": [{"operation": "audio.transcribe"}]}]})
            def do_POST(self):
                observed.append((self.path, self.headers.get("Authorization"), self.headers.get("X-Mica-Workload")))
                self.rfile.read(int(self.headers.get("Content-Length", "0")))
                self.reply({"text": "hello world"} if self.path.endswith("transcriptions") else
                           {"choices": [{"message": {"content": "Hello, world."}}]} if self.path.endswith("completions") else
                           {"session_id": "test-session", "answer": "Assistant OK"})
        self.server = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.server.serve_forever, daemon=True); self.thread.start()
        self.key_file = self.root / "inference-key"; self.key_file.write_text(KEY); self.key_file.chmod(0o600)
        self.config = self.root / "config.json"
        self.config.write_text(json.dumps({"ui": {"mica_url": f"http://127.0.0.1:{self.server.server_port}"}, "api_key_file": str(self.key_file)}))
        self.wav = self.root / "sample.wav"; self.wav.write_bytes(b"RIFF" + bytes(4) + b"WAVE" + bytes(32))

    def tearDown(self):
        self.server.shutdown(); self.server.server_close(); self.thread.join(); self.temp.cleanup()

    def test_native_asr_cleanup_remote_no_local_server(self):
        command = [str(BINARY), "--config", str(self.config), "--root", str(self.root), "--workload", "remote-dictation", "--transcribe", str(self.wav), "--cleanup", "--json"]
        result = subprocess.run(command, capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads(result.stdout), {"text": "Hello, world."})
        self.assertEqual([x[0] for x in self.observed], ["/v1/audio/transcriptions", "/v1/chat/completions"])
        self.assertTrue(all(x[1:] == ("Bearer " + KEY, "remote-dictation") for x in self.observed))
        self.assertNotIn(KEY, result.stdout + result.stderr + " ".join(command))
        self.assertFalse((self.root / "models").exists())

    def mcp(self, app, messages, allow=False):
        command = [sys.executable, str(CONFIG.parent / "apps" / app / "mcp.py"), "--config", str(self.config), "--root", str(self.root), "--workload", "remote-dictation"]
        if allow: command += ["--allow-directory", str(self.root)]
        result = subprocess.run(command, input="\n".join(json.dumps(x) for x in messages) + "\n", capture_output=True, text=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertNotIn(KEY, result.stdout + result.stderr)
        return [json.loads(x) for x in result.stdout.splitlines()]

    def test_dictation_mcp_legacy_and_modern_tools(self):
        metadata = {"io.modelcontextprotocol/protocolVersion": "2026-07-28", "io.modelcontextprotocol/clientCapabilities": {}}
        responses = self.mcp("dictation", [
            {"jsonrpc": "2.0", "id": 1, "method": "initialize", "params": {"protocolVersion": "2025-11-25"}},
            {"jsonrpc": "2.0", "method": "notifications/initialized"},
            {"jsonrpc": "2.0", "id": 2, "method": "server/discover", "params": {"_meta": metadata}},
            {"jsonrpc": "2.0", "id": 3, "method": "tools/list", "params": {"_meta": metadata}},
            {"jsonrpc": "2.0", "id": 4, "method": "tools/call", "params": {"name": "transcribe", "arguments": {"audio_path": str(self.wav), "cleanup": True}, "_meta": metadata}},
        ], allow=True)
        self.assertEqual(len(responses), 4)
        self.assertEqual(responses[0]["result"]["protocolVersion"], "2025-11-25")
        self.assertIn("2026-07-28", responses[1]["result"]["supportedVersions"])
        self.assertEqual({x["name"] for x in responses[2]["result"]["tools"]}, {"models", "transcribe", "cleanup"})
        self.assertFalse(responses[3]["result"]["isError"])
        self.assertEqual(json.loads(responses[3]["result"]["content"][0]["text"])["text"], "Hello, world.")

    def test_mcp_file_upload_requires_explicit_allow_directory(self):
        result = self.mcp("dictation", [{"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "transcribe", "arguments": {"audio_path": str(self.wav)}}}])
        self.assertTrue(result[0]["result"]["isError"])
        self.assertFalse(self.observed)

    def test_assistant_mcp_agent_session_and_workload(self):
        result = self.mcp("assistant", [{"jsonrpc": "2.0", "id": 1, "method": "tools/call", "params": {"name": "assistant_chat", "arguments": {"text": "Hello", "session_id": "test-session"}}}])
        self.assertFalse(result[0]["result"]["isError"])
        self.assertEqual(json.loads(result[0]["result"]["content"][0]["text"])["answer"], "Assistant OK")
        self.assertEqual(self.observed, [("/v1/agent/chat", "Bearer " + KEY, "remote-dictation")])


if __name__ == "__main__": unittest.main()
