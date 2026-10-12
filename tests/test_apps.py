"""Isolated native package lifecycle tests: no model downloads or inference."""
import hashlib
import fcntl
import importlib.util
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import unittest
import urllib.error
import urllib.request
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BINARY, CONFIG = map(lambda value: Path(value).resolve(), sys.argv[1:3])
sys.argv = sys.argv[:1]


class AppsTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="mica-app-tests-")
        self.root = Path(self.temporary.name) / "home"

    def tearDown(self):
        self.temporary.cleanup()

    def command(self, *args, config=CONFIG, success=True):
        result = subprocess.run([str(BINARY), "app", *args, "--root", str(self.root),
                                 "--config-dir", str(config)], text=True, capture_output=True, timeout=30)
        if success:
            self.assertEqual(result.returncode, 0, result.stderr + result.stdout)
        else:
            self.assertNotEqual(result.returncode, 0, result.stdout)
        return result

    def test_bundled_install_preservation_and_recovery(self):
        catalogue = json.loads(self.command("list").stdout)
        self.assertEqual(catalogue["sources"][0]["id"], "mica-examples")
        self.assertEqual(len(catalogue["apps"]), 2)
        self.assertFalse(self.root.exists(), "Listing must not install/create app directories")
        self.assertFalse(json.loads(self.command("show", "mica-dictation").stdout)["installed"])
        self.command("install", "mica-dictation", success=False)
        self.command("install", "mica-dictation", "--accept-code")
        registered = self.root / "config/profiles/mica-dictation--mac.yaml"
        self.assertTrue(registered.is_file())
        self.assertFalse((self.root / "models").exists())
        shown = json.loads(self.command("show", "mica-dictation").stdout)
        self.assertEqual(shown["permissions"], ["microphone", "clipboard"])
        self.command("start", "mica-dictation", success=False)
        result = self.command("start", "mica-dictation", "--accept-code", success=False)
        self.assertIn("start Mica", result.stderr)
        self.command("install", "mica-dictation", "--accept-code", success=False)
        registered.write_text(registered.read_text() + "\n# User customization\n")
        self.command("uninstall", "mica-dictation")
        self.assertTrue(registered.exists(), "Edited workload must survive uninstall")
        self.assertFalse((self.root / "config/profiles/mica-dictation--cuda.yaml").exists())
        self.assertTrue(list((self.root / "apps").glob(".removed-mica-dictation-*")))
        self.assertFalse((self.root / "apps/mica-dictation").exists())

    def test_bad_checksum_and_traversal_abort_without_registration(self):
        fixture = Path(self.temporary.name) / "package"
        shutil.copytree(CONFIG.parent / "apps", fixture / "apps", ignore=shutil.ignore_patterns("__pycache__"))
        (fixture / "config").mkdir()
        (fixture / "apps/dictation/index.html").write_text("tampered")
        result = self.command("install", "mica-dictation", "--accept-code", config=fixture / "config", success=False)
        self.assertIn("SHA-256 mismatch", result.stderr)
        self.assertFalse((self.root / "config/profiles").exists())
        self.assertFalse((self.root / "apps/.staging-mica-dictation").exists())
        self.command("source-add", "bad", "http://example.com/app.yaml", success=False)
        self.command("source-add", "../escape", "https://example.com/app.yaml", success=False)

    def test_headless_manifest_and_unsafe_package_rejection(self):
        fixture = Path(self.temporary.name) / "package"
        shutil.copytree(CONFIG.parent / "apps", fixture / "apps", ignore=shutil.ignore_patterns("__pycache__"))
        (fixture / "config").mkdir()
        manifest = fixture / "apps/dictation/app.yaml"
        index = fixture / "apps/apps.yaml"
        original_manifest = manifest.read_text()
        original_hash = hashlib.sha256(manifest.read_bytes()).hexdigest()
        original_index = index.read_text()

        def update(contents):
            manifest.write_text(contents)
            index.write_text(original_index.replace(original_hash, hashlib.sha256(manifest.read_bytes()).hexdigest()))

        update(original_manifest.replace("  port: 8091\n", "").replace("  path: /\n", ""))
        shown = json.loads(self.command("show", "mica-dictation", config=fixture / "config").stdout)
        self.assertNotIn("port", shown["launch"])
        for bad in (original_manifest.replace("path: app.py", "path: ../escape.py"),
                    original_manifest.replace("executable: python3", "executable: '{app_dir}/bin/unverified'"),
                    original_manifest.replace("sha256:", "not_a_hash:")):
            update(bad)
            self.command("show", "mica-dictation", config=fixture / "config", success=False)

    def test_assistant_assets_and_tui_snapshot(self):
        self.command("install", "mica-assistant", "--accept-code")
        self.assertTrue((self.root / "apps/mica-assistant/runtime/mica_playground.py").is_file())
        result = subprocess.run([str(BINARY), "tui", "--snapshot", "--root", str(self.root),
                                 "--config-dir", str(CONFIG)], text=True, capture_output=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        snapshot = json.loads(result.stdout)
        self.assertEqual(snapshot["sections"][-1]["id"], "apps")
        self.assertTrue(snapshot["apps"]["apps"][0]["installed"])

    def test_native_app_start_stop_against_mock_model_server(self):
        # Actual app process and HTTP client, but deliberately no model inference.
        fixture = Path(self.temporary.name) / "package"
        shutil.copytree(CONFIG, fixture / "config")
        shutil.copytree(CONFIG.parent / "apps", fixture / "apps", ignore=shutil.ignore_patterns("__pycache__"))
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            app_port = probe.getsockname()[1]
        manifest = fixture / "apps/dictation/app.yaml"
        old_hash = hashlib.sha256(manifest.read_bytes()).hexdigest()
        manifest.write_text(manifest.read_text().replace("port: 8091", f"port: {app_port}"))
        index = fixture / "apps/apps.yaml"
        index.write_text(index.read_text().replace(old_hash, hashlib.sha256(manifest.read_bytes()).hexdigest()))
        self.command("install", "mica-dictation", "--accept-code", config=fixture / "config")

        class ModelServer(BaseHTTPRequestHandler):
            operation = "audio.transcribe"

            def do_GET(self):
                self.send_response(200)
                self.end_headers()
                payload = {"data": [{"id": "fixture-asr", "supported_interactions": [{"operation": self.operation}],
                                     "endpoint_contracts": [{"operation": "audio.transcribe"}]}]}
                self.wfile.write(json.dumps(payload).encode())

            def log_message(self, *_):
                pass

        backend = ThreadingHTTPServer(("127.0.0.1", 0), ModelServer)
        thread = threading.Thread(target=backend.serve_forever, daemon=True)
        thread.start()
        (self.root / "run").mkdir()
        (self.root / "secrets").mkdir()
        key = self.root / "secrets/api-key"
        key.write_text("test-secret-not-real")
        (self.root / "run/server.json").write_text(json.dumps({"host": "127.0.0.1", "port": backend.server_port, "api_key_file": str(key)}))
        (self.root / "config/server.json").write_text(json.dumps({"port": backend.server_port, "api_key_file": str(key)}))
        with (self.root / "run/server.lock").open("w") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX)
            try:
                self.command("start", "mica-dictation", "--accept-code", config=fixture / "config")
                deadline = time.monotonic() + 5
                while True:
                    try:
                        with urllib.request.urlopen(f"http://127.0.0.1:{app_port}/", timeout=1) as response:
                            self.assertIn(b"Dictation", response.read())
                        break
                    except OSError:
                        if time.monotonic() >= deadline:
                            raise
                        time.sleep(0.05)
                self.assertTrue(json.loads(self.command("show", "mica-dictation", config=fixture / "config").stdout)["running"])
                self.command("stop", "mica-dictation", config=fixture / "config")
                ModelServer.operation = "chat.generate"  # Engine alone is not enough.
                denied = self.command("start", "mica-dictation", "--accept-code", config=fixture / "config", success=False)
                self.assertIn("lacks required operation", denied.stderr)
            finally:
                self.command("stop", "mica-dictation", config=fixture / "config")
                backend.shutdown()
                backend.server_close()
                thread.join()

    def test_dictation_bridge_origin_auth_and_routes(self):
        spec = importlib.util.spec_from_file_location("dictation_app", CONFIG.parent / "apps/dictation/app.py")
        app = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(app)
        received = []

        class Backend(BaseHTTPRequestHandler):
            def do_GET(self):
                received.append((self.path, self.headers.get("Authorization")))
                self.send_response(200)
                self.end_headers()
                self.wfile.write(b'{"data":[],"profile":"test"}')

            def log_message(self, *_):
                pass

        backend = ThreadingHTTPServer(("127.0.0.1", 0), Backend)
        bridge = ThreadingHTTPServer(("127.0.0.1", 0), app.Handler)
        configuration = Path(self.temporary.name) / "server.json"
        configuration.write_text(json.dumps({"host": "127.0.0.1", "port": backend.server_port, "api_key": "test-key-only"}))
        bridge.config = configuration
        threads = [threading.Thread(target=server.serve_forever, daemon=True) for server in (backend, bridge)]
        for thread in threads:
            thread.start()
        try:
            url = f"http://127.0.0.1:{bridge.server_port}"
            with urllib.request.urlopen(url + "/") as response:
                self.assertIn(b"Dictation", response.read())
            with urllib.request.urlopen(url + "/api/models") as response:
                self.assertEqual(json.load(response)["profile"], "test")
            self.assertEqual(received, [("/v1/models", "Bearer test-key-only")])
            request = urllib.request.Request(url + "/api/models", headers={"Origin": "https://untrusted.example"})
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(request)
            self.assertEqual(error.exception.code, 403)
            with self.assertRaises(urllib.error.HTTPError) as error:
                urllib.request.urlopen(url + "/api/arbitrary")
            self.assertEqual(error.exception.code, 404)
        finally:
            for server in (bridge, backend):
                server.shutdown()
                server.server_close()
            for thread in threads:
                thread.join()


if __name__ == "__main__":
    unittest.main()
