"""Native CLI lifecycle and TUI diagnostics; offline fixtures, no inference claims."""
import json
from pathlib import Path
import shutil
import socket
import subprocess
import sys
import tempfile
import unittest
import urllib.error
import urllib.request

BINARY, CONFIG = map(Path, sys.argv[1:3])
sys.argv = sys.argv[:1]


class LifecycleTests(unittest.TestCase):
    def test_server_configuration_contract_and_nested_key_redaction(self):
        with tempfile.TemporaryDirectory(prefix="mica-config-contract-") as temporary:
            root = Path(temporary)
            path = root / "config/server.json"
            path.parent.mkdir(parents=True)
            secret, ui_secret = "fixture-secret-not-a-real-key", "fixture-ui-secret-not-a-real-key"
            valid = {"host": "127.0.0.1", "port": 8092, "default_workload": "mac_coder",
                     "api_key": secret, "ui": {"port": 8090, "mica_url": "http://127.0.0.1:8092", "api_key": ui_secret}}
            path.write_text(json.dumps(valid))
            def run(*args):
                return subprocess.run([str(BINARY), *args, "--root", str(root)], capture_output=True, text=True, timeout=15)
            for args in (("config", "show"), ("tui", "--snapshot", "--config-dir", str(CONFIG))):
                result = run(*args)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertNotIn(secret, result.stdout)
                self.assertNotIn(ui_secret, result.stdout)
            result = run("config", "set", "--port", "8093")
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(json.loads(path.read_text())["schema"], 1)
            for invalid in ({"schema": 2}, {"schema": 1.0}, {"port": 65536}, {"port": 1.5},
                            {"port": "8080"}, {"host": ""}, {"host": "local host"},
                            {"ram_gib": 8}, {"default_workload": "../bad"},
                            {"ui": {"mica_url": "file:///tmp/private"}}, {"ui": {"unknown": True}}):
                path.write_text(json.dumps(invalid))
                result = run("config", "show")
                self.assertNotEqual(result.returncode, 0, invalid)
                self.assertEqual(json.loads(path.read_text()), invalid)
            self.assertFalse((root / "state/runtime.json").exists())

    def test_lifecycle_settings_and_workload_aliases(self):
        with tempfile.TemporaryDirectory(prefix="mica-lifecycle-") as temporary:
            root = Path(temporary)
            profile_dir = root / "config/profiles"
            profile_dir.mkdir(parents=True)
            source = CONFIG.parent / "tests/fixtures/profiles"
            for name in ("swap-a", "swap-b"):
                shutil.copyfile(source / (name + ".yaml"), profile_dir / (name + ".yaml"))
            engine = root / "runtimes/llama.cpp/build-mica/bin/llama-server"
            engine.parent.mkdir(parents=True)
            engine.write_text("#!/bin/sh\necho 'offline lifecycle fixture'\n")
            engine.chmod(0o700)
            shutil.copyfile(engine, engine.with_name("llama-quantize"))
            model = root / "models/gguf/spark-x25-4b"
            model.mkdir(parents=True)
            (model / "Spark-X2.5-4B-Q4_K_M.gguf").touch()
            (model / ".mica-complete-q4").write_text(
                "miguelamendez/mica-spark-x25-4b\n6c242d2945f11bb3587c437a1bf1088666ca6e2f\n")
            (root / "config/machine.yaml").write_text(
                "schema: 1\nallowed_devices: [cpu]\nlimits:\n  inference: {ram_gib: 8}\n  build: {ram_gib: 8, parallel_jobs: 1}\n")
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
            def run(*args, success=True):
                result = subprocess.run([str(BINARY), *args, "--root", str(root)],
                                        capture_output=True, text=True, timeout=50)
                if success:
                    self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                else:
                    self.assertNotEqual(result.returncode, 0, result.stdout + result.stderr)
                return result
            try:
                missing = run("start", "--workload", "missing-workload-test",
                              "--config-dir", str(CONFIG), success=False)
                self.assertIn("workload 'missing-workload-test' was not found", missing.stderr)
                self.assertNotIn("map::at", missing.stderr)
                self.assertFalse((root / "state/runtime.json").exists())
                run("config", "set", "--port", str(port), "--default-workload", "swap-a", "--rotate-api-key")
                token = (root / "secrets/api-key").read_text().strip()
                self.assertEqual((root / "secrets/api-key").stat().st_mode & 0o777, 0o600)
                self.assertNotIn(token, run("config", "show").stdout)
                run("workload", "install", "swap-a", "--config-dir", str(CONFIG))
                runtime = json.loads((root / "state/runtime.json").read_text())
                self.assertEqual(runtime["max_ram_gib"], 8)
                run("start", "--config-dir", str(CONFIG))
                state = json.loads(run("status").stdout)
                self.assertIn(state["status"], ("ready", "warming"))
                pid = state["pid"]
                def workloads(key=token):
                    request = urllib.request.Request(f"http://127.0.0.1:{port}/v1/workloads",
                        headers={"Authorization": "Bearer " + key})
                    return json.load(urllib.request.urlopen(request, timeout=3))
                with self.assertRaises(urllib.error.HTTPError) as rejected_workloads:
                    workloads("invalid-token")
                self.assertEqual(rejected_workloads.exception.code, 401)
                catalog = workloads()
                self.assertEqual(catalog["active_workload"], "swap-a")
                listed_ids = {item["id"] for item in catalog["data"]}
                self.assertNotIn("vllm-control", listed_ids)
                self.assertNotIn("qwen27b-modes", listed_ids)
                self.assertIn("gpu_16g_coder", listed_ids)
                swap_b = next(item for item in catalog["data"] if item["id"] == "swap-b")
                self.assertTrue(swap_b["can_activate"])
                self.assertTrue(swap_b["description"])
                self.assertNotIn(token, json.dumps(catalog))
                self.assertEqual(json.loads(run("start").stdout)["pid"], pid)
                run("config", "set", "--port", "8099", success=False)
                run("workload", "install", "swap-b", "--config-dir", str(CONFIG), success=False)
                request = urllib.request.Request(f"http://127.0.0.1:{port}/admin/server/stop", data=b"{}")
                with self.assertRaises(urllib.error.HTTPError) as rejected:
                    urllib.request.urlopen(request, timeout=3)
                self.assertEqual(rejected.exception.code, 401)
                run("workload", "activate", "swap-b")
                self.assertEqual(json.loads(run("status").stdout)["profile"]["name"], "swap-b")
                self.assertEqual(workloads()["active_workload"], "swap-b")
                snap = run("tui", "--snapshot", "--config-dir", str(CONFIG)).stdout
                document = json.loads(snap)
                self.assertEqual(len(document["sections"]), 6)
                self.assertNotIn("machine", [section["id"] for section in document["sections"]])
                self.assertIn("Machine", document["server_subsections"])
                self.assertIn("models", document["disk_usage_bytes"])
                self.assertTrue(all("availability" in engine for engine in document["engines"]))
                groups = {model["group"] for model in document["models"]["data"]}
                self.assertTrue({"LLM", "VLM", "ASR", "TTS", "Embeddings"} <= groups)
                self.assertNotIn("balanced-all", {entry["id"] for entry in document["workloads"]})
                current = next(item for item in document["workloads"] if item["id"] == "swap-b")
                self.assertTrue(current["allocation_fits"])
                self.assertTrue(all("example" in route for route in document["endpoints"]["endpoints"]))
                self.assertNotIn(token, snap)
                run("stop")
                self.assertEqual(json.loads(run("status").stdout)["status"], "stopped")
                run("stop")
                run("config", "set", "--rotate-api-key")
                self.assertNotEqual(token, (root / "secrets/api-key").read_text().strip())
                run("config", "set", "--ram-gib", "99999", success=False)
                run("config", "set", "--port", "99999", success=False)
            finally:
                subprocess.run([str(BINARY), "stop", "--root", str(root)], capture_output=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
