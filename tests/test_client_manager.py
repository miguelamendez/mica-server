"""Native device manager acceptance. Mock inference only; no models/recording."""
import http.client
import hashlib
import fcntl
import os
import pty
import select
import struct
import termios
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import unittest

BINARY, CONFIG = map(Path, sys.argv[1:3])
sys.argv = sys.argv[:1]
KEY = "test-only-inference-key-1234567890"


class Client(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory(prefix="mica-manager-")
        self.root = Path(self.temp.name) / "client"
        self.server_root = Path(self.temp.name) / "server"
        self.key_file = Path(self.temp.name) / "key"
        self.key_file.write_text(KEY)
        self.seen = []
        seen = self.seen

        class Handler(BaseHTTPRequestHandler):
            def log_message(self, *_): pass
            def do_GET(self):
                seen.append((self.path, self.headers.get("Authorization")))
                result = {"status": "ok", "version": "test"}
                status = 200
                if self.path.startswith("/v1/models"):
                    status = 200 if self.headers.get("Authorization") == "Bearer " + KEY else 401
                    result = {"data": [{"supported_interactions": [{"operation": "chat.generate"}],
                                         "endpoint_contracts": [{"operation": "chat.generate"}]}]}
                body = json.dumps(result).encode()
                self.send_response(status); self.send_header("Content-Length", str(len(body)))
                self.end_headers(); self.wfile.write(body)
        self.mock = ThreadingHTTPServer(("127.0.0.1", 0), Handler)
        self.thread = threading.Thread(target=self.mock.serve_forever, daemon=True); self.thread.start()
        self.url = f"http://127.0.0.1:{self.mock.server_port}"
        self.ui = None

    def tearDown(self):
        self.command("stop", "mica-assistant", success=None)
        if self.ui:
            self.ui.terminate(); self.ui.wait(timeout=5)
        self.mock.shutdown(); self.mock.server_close(); self.thread.join(); self.temp.cleanup()

    def command(self, *args, success=True, config=CONFIG):
        result = subprocess.run([str(BINARY), *args, "--root", str(self.root), "--server-root", str(self.server_root), "--config-dir", str(config)], capture_output=True, text=True, timeout=30)
        if success is not None:
            self.assertEqual(result.returncode == 0, success, result.stdout + result.stderr)
        return result

    def connect(self):
        self.command("connect", "linux", "--url", self.url, "--workload", "test-workload", "--api-key-file", str(self.key_file))

    def test_help_detection_and_install_are_serverless(self):
        help_text = self.command("--help").stdout
        self.assertIn("55001", help_text); self.assertIn("55200–55399", help_text)
        self.assertFalse(self.root.exists(), "help should not create state")
        detection = json.loads(self.command("detect").stdout)
        self.assertFalse(detection["running"])
        self.assertEqual(detection["url"], "http://127.0.0.1:55000")
        self.command("install", "mica-assistant", success=False)
        self.command("install", "mica-assistant", "--accept-code", "--inference", "later")
        self.assertTrue((self.root / "apps/mica-assistant/receipt.json").is_file())
        self.assertFalse(self.server_root.exists())
        for name in ("models", "environments", "runtimes"):
            self.assertFalse((self.root / name).exists())
        self.assertEqual(len(json.loads(self.command("list").stdout)["apps"]), 1)

    def test_remote_connection_validation_and_capabilities(self):
        self.connect()
        saved = self.command("connections").stdout
        self.assertNotIn(KEY, saved); self.assertNotIn("api_key_file", saved)
        self.assertEqual((self.root / "secrets/connections/linux").stat().st_mode & 0o777, 0o600)
        self.command("install", "mica-assistant", "--accept-code", "--inference", "remote", "--connection", "linux")
        self.command("install", "mica-dictation", "--accept-code", "--inference", "later")
        result = self.command("bind", "mica-dictation", "--connection", "linux", success=False)
        self.assertIn("audio.transcribe", result.stderr)
        self.assertTrue(any(a == "Bearer " + KEY for _, a in self.seen))
        self.command("connect", "bad", "--url", self.url + "/path", "--workload", "test", "--api-key-file", str(self.key_file), success=False)
        self.command("setup-local", "mica-assistant", "--workload", "mica-assistant--mac", success=False)
        self.assertFalse((self.server_root / "models").exists())

    def test_native_client_tui_opens_and_closes_without_inference(self):
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 32, 120, 0, 0))
        process = subprocess.Popen([str(BINARY), "tui", "--root", str(self.root),
                                    "--server-root", str(self.server_root), "--config-dir", str(CONFIG)],
                                   stdin=slave, stdout=slave, stderr=slave,
                                   env={**os.environ, "TERM": "xterm-256color"})
        os.close(slave)
        output = b""
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline and b"Mica Client" not in output:
                if select.select([master], [], [], .1)[0]: output += os.read(master, 65536)
            self.assertIn(b"Mica Client", output)
            self.assertIn(b"Discover", output)
            os.write(master, b"q")
            deadline = time.monotonic() + 5
            while process.poll() is None and time.monotonic() < deadline:
                if select.select([master], [], [], .1)[0]:
                    try: os.read(master, 65536)
                    except OSError: break
            self.assertEqual(process.wait(timeout=1), 0)
            self.assertFalse((self.server_root / "run/server.json").exists())
            self.assertFalse((self.root / "models").exists())
        finally:
            if process.poll() is None: process.kill(); process.wait(timeout=5)
            os.close(master)

    def test_html_manager_auth_origin_and_web_app_routes(self):
        self.connect()
        self.command("install", "mica-assistant", "--accept-code", "--inference", "remote", "--connection", "linux")
        for port in range(55002, 55090):
            with socket.socket() as probe:
                try: probe.bind(("127.0.0.1", port)); break
                except OSError: continue
        self.port = port
        self.ui = subprocess.Popen([str(BINARY), "ui", "--root", str(self.root), "--server-root", str(self.server_root), "--config-dir", str(CONFIG), "--port", str(port)], stdout=subprocess.DEVNULL, stderr=subprocess.PIPE)
        for _ in range(100):
            try:
                if self.http("GET", "/health")[0] == 200: break
            except OSError: pass
            time.sleep(.05)
        self.assertEqual(self.http("GET", "/")[0], 200)
        self.assertEqual(self.http("GET", "/api/inventory")[0], 401)
        token = (self.root / "secrets/client-ui-key").read_text().strip()
        auth = {"Authorization": "Bearer " + token}
        self.assertEqual(self.http("GET", "/api/inventory", headers={**auth, "Origin": "http://evil.invalid"})[0], 403)
        status, _, body = self.http("GET", "/api/inventory", headers=auth)
        self.assertEqual(status, 200); self.assertNotIn(KEY.encode(), body); self.assertNotIn(token.encode(), body)
        self.assertEqual(self.http("GET", "/apps/mica-assistant/chat")[0], 401)
        status, headers, body = self.http("POST", "/api/action", {"action": "web.open", "id": "mica-assistant", "accept_code": True}, auth)
        self.assertEqual(status, 200, body)
        route = json.loads(body)["url"]; self.assertEqual(route, "/apps/mica-assistant/chat")
        cookie = headers["Set-Cookie"].split(";", 1)[0]
        for _ in range(50):
            status, _, body = self.http("GET", route, headers={"Cookie": cookie})
            if status == 200: break
            time.sleep(.1)
        self.assertEqual(status, 200, body[:500]); self.assertIn(b"Local Assistant", body)
        state = json.loads((self.root / "state/apps/mica-assistant.json").read_text())
        upstream = int(state["url"].split(":")[2].split("/")[0])
        self.assertTrue(55200 <= upstream <= 55399)
        # A static app needs neither inference nor an HTTP service/port.
        fixture = Path(self.temp.name) / "static-package"
        shutil.copytree(CONFIG, fixture / "config")
        app = fixture / "apps/static-demo"
        app.mkdir(parents=True)
        html = b"<!doctype html><title>Static app</title><p>STATIC_CLIENT_OK</p>"
        (app / "index.html").write_bytes(html)
        manifest = {"schema": 1, "id": "static-demo", "name": "Static demo", "description": "No process", "version": "1",
                    "interfaces": ["web"], "source": {"repository": "https://github.com/miguelamendez/mica-server", "path": "apps/static-demo"},
                    "distribution": {"format": "files", "files": [{"path": "index.html", "sha256": hashlib.sha256(html).hexdigest(), "size_bytes": len(html)}]},
                    "requirements": {"operations": []}, "permissions": [], "launch": {"static_entry": "index.html"}, "workloads": []}
        data = json.dumps(manifest).encode(); (app / "app.yaml").write_bytes(data)
        (fixture / "apps/apps.yaml").write_text(json.dumps({"schema": 1, "apps": [{"id": "static-demo", "name": "Static demo", "description": "No process", "manifest": "static-demo/app.yaml", "sha256": hashlib.sha256(data).hexdigest()}]}))
        self.command("install", "static-demo", "--accept-code", "--inference", "later", config=fixture / "config")
        status, headers, body = self.http("POST", "/api/action", {"action": "web.open", "id": "static-demo", "accept_code": True}, auth)
        self.assertEqual(status, 200, body)
        cookie = headers["Set-Cookie"].split(";", 1)[0]
        self.assertEqual(json.loads(body)["url"], "/apps/static-demo/")
        status, _, body = self.http("GET", "/apps/static-demo/", headers={"Cookie": cookie})
        self.assertEqual(status, 200); self.assertIn(b"STATIC_CLIENT_OK", body)
        self.assertEqual(self.http("GET", "/apps/static-demo/receipt.json", headers={"Cookie": cookie})[0], 404)
        state = json.loads((self.root / "state/apps/static-demo.json").read_text())
        self.assertEqual(state, {"static": True})
        self.command("stop", "static-demo")

    def http(self, method, path, body=None, headers=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=15)
        headers = dict(headers or {})
        if body is not None: body = json.dumps(body); headers["Content-Type"] = "application/json"
        conn.request(method, path, body, headers); response = conn.getresponse()
        result = response.status, dict(response.getheaders()), response.read(); conn.close(); return result


unittest.main()
