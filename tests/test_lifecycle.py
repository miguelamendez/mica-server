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
                self.assertEqual(len(document["sections"]), 7)
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
