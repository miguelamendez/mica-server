"""Offline tests: no MLX, model weights, hardware or network required."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch

spec = importlib.util.spec_from_file_location("memory_profile",
    Path(__file__).resolve().parents[1] / "scripts/profile_model_memory.py")
profiler = importlib.util.module_from_spec(spec)
spec.loader.exec_module(profiler)


class MemoryProfileTests(unittest.TestCase):
    def test_native_context_reserves_output_not_adds_it_twice(self):
        budget = profiler.token_budget({"native_context_tokens": 131072}, None, 16384)
        self.assertEqual(budget["input_tokens"], 114688)
        self.assertEqual(budget["total_tokens"], 131072)

    def test_unknown_output_limit_requires_explicit_override(self):
        with self.assertRaisesRegex(ValueError, "output limit unverified"):
            profiler.token_budget({"native_context_tokens": 131072, "max_output_tokens": None}, None, None)

    def test_supported_output_limit_and_context_enforced(self):
        model = {"native_context_tokens": 8192, "max_output_tokens": 1024}
        self.assertEqual(profiler.token_budget(model, None, None)["total_tokens"], 8192)
        for input_tokens, output_tokens in [(8192, 1), (1024, 2048), (0, 1), (1, -1), (1, 0)]:
            with self.assertRaises(ValueError):
                profiler.token_budget(model, input_tokens, output_tokens)

    def test_recommended_total_is_default_but_not_native_ceiling(self):
        model = {"native_context_tokens": 8192, "recommended_context_tokens": 4096,
                 "max_output_tokens": 1024}
        budget = profiler.token_budget(model, None, None)
        self.assertEqual(budget["input_tokens"], 3072)
        self.assertEqual(budget["total_tokens"], 4096)
        self.assertEqual(budget["output_limit_source"], "model")
        budget = profiler.token_budget(model, 6000, 1024)
        self.assertTrue(budget["recommended_context_exceeded"])
        self.assertEqual(budget["output_limit_source"], "cli-override")

    def test_invalid_flat_limits_are_rejected(self):
        for field in ("recommended_context_tokens", "max_output_tokens"):
            for invalid in (True, -1, 0, 8193, 1024.5, {"tokens": 1024}):
                with self.assertRaises(ValueError):
                    profiler.token_budget({"native_context_tokens": 8192, field: invalid}, 1024, 256)

    def test_artifact_selection_distinguishes_same_id_formats(self):
        model = {"artifacts": [
            {"id": "q4", "format": "mlx", "compatible_engines": ["mlx-lm"]},
            {"id": "q4", "format": "gguf", "compatible_engines": ["llama-cpp"]}]}
        self.assertEqual(profiler.select_artifact(model, "q4")[1], "mlx-lm")
        self.assertEqual(profiler.select_artifact(model, "q4", "llama-cpp")[0]["format"], "gguf")
        with self.assertRaises(ValueError):
            profiler.select_artifact(model, "q8")

    def test_cache_paths_cannot_escape_model(self):
        with self.assertRaises(ValueError):
            profiler.contained(Path("/tmp/models/foo"), "../../private")

    def test_engine_launch_pins_actual_quantization_and_no_drafter(self):
        args = SimpleNamespace(gpu_layers=99, batch_size=512, prefill_chunk=128,
            threads=4, image_tokens=1024)
        for quant in ("q4", "q8", "f16"):
            cmd = profiler.llama_command("server", "weights", quant,
                {"total_tokens": 81920}, 9999, args)
            self.assertEqual(cmd[cmd.index("-c") + 1], "81920")
            for flag in ("-ctk", "-ctv"):
                self.assertEqual(cmd[cmd.index(flag) + 1], profiler.KV_TYPES[quant])
            self.assertEqual(cmd[cmd.index("--fit") + 1], "off")
            self.assertNotIn("--mmproj", cmd)
            self.assertNotIn("--spec-draft-model", cmd)
        cmd = profiler.llama_command("server", "weights", "q8",
            {"total_tokens": 81920}, 9999, args, "projector")
        self.assertEqual(cmd[cmd.index("--mmproj") + 1], "projector")

    def test_log_buffers_keep_component_labels(self):
        records = profiler.parse_buffers(
            "model: Metal model buffer size = 2048.00 MiB\n"
            "cache: Metal KV buffer size = 1.00 GiB\n"
            "graph: Metal compute buffer size = 128.00 MiB\n")
        self.assertEqual(len(records), 3)
        self.assertEqual(records[1]["allocated_bytes"], profiler.GIB)
        self.assertIn("KV", records[1]["label"])

    def test_actual_kv_precision_is_verified_not_assumed_from_flags(self):
        self.assertEqual(profiler.reported_kv_types(
            "cache: size = 17.00 MiB, K (q4_0): 8.50 MiB, V (q8_0): 8.50 MiB"),
            [{"key": "q4_0", "value": "q8_0"}])
        self.assertEqual(profiler.reported_kv_types("-ctk q8_0 -ctv q8_0"), [])

    def test_atomic_json_write(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "model/report.json"
            profiler.save(path, {"status": "planned"})
            profiler.save(path, {"status": "measured"})
            self.assertEqual(json.loads(path.read_text())["status"], "measured")
            self.assertFalse(path.with_suffix(".json.tmp").exists())

    def test_gib_display_never_turns_unknown_memory_into_zero(self):
        displayed = profiler.with_gib({"rss_bytes": profiler.GIB, "peak_bytes": None,
                                      "files": [{"disk_bytes": profiler.GIB // 2}]})
        self.assertEqual(displayed["rss_gib"], 1.0)
        self.assertEqual(displayed["files"][0]["disk_gib"], .5)
        self.assertIsNone(displayed["peak_bytes"])
        self.assertNotIn("peak_gib", displayed)

    def test_synthetic_media_is_valid_png_signature_and_dimensions(self):
        image = profiler.fixture_image(32)
        self.assertEqual(image[:8], b"\x89PNG\r\n\x1a\n")
        self.assertEqual(profiler.struct.unpack(">II", image[16:24]), (32, 32))

    def test_watchdog_fails_closed_without_touching_other_workers(self):
        child = Mock()
        child.poll.return_value = None
        watch = profiler.Watchdog(child, 12, 10)
        with patch.object(profiler, "memory_snapshot", side_effect=OSError("monitor denied")), \
             patch.object(profiler, "stop_process") as stop:
            watch.watch()
        stop.assert_called_once_with(child)
        self.assertIn("monitor denied", watch.error)

    def test_native_runner_fills_exact_token_capacity_and_measures_projector(self):
        child = Mock()
        child.poll.return_value = None
        bodies = []
        watchdog = Mock()
        watchdog.__enter__ = Mock(return_value=watchdog)
        watchdog.__exit__ = Mock(return_value=False)
        watchdog.error = None
        watchdog.peaks = {"physical_footprint_bytes": 100}
        args = SimpleNamespace(gpu_layers=99, batch_size=512, prefill_chunk=128,
            threads=4, image_tokens=1024, limit_gib=12, timeout=100, decode_tokens=2)
        def fake_request(url, route, body=None, **unused):
            if route == "/health":
                return {}
            if route == "/tokenize":
                return {"tokens": [1, 2, 3]}
            bodies.append((route, body))
            if route == "/v1/chat/completions":
                return {"usage": {"prompt_tokens": 1040}}
            return {"tokens_evaluated": len(body["prompt"]),
                    "tokens_predicted": body["n_predict"], "timings": {}}
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "projector.log"
            def launch(*unused, **kwargs):
                kwargs["stdout"].write("cache: K (q8_0): 1.0 MiB, V (q8_0): 1.0 MiB\n")
                kwargs["stdout"].flush()
                return child
            with patch.object(profiler, "Watchdog", return_value=watchdog), \
                 patch.object(profiler, "unused_port", return_value=9999), \
                 patch.object(profiler, "memory_snapshot", return_value={"rss_bytes": 100}), \
                 patch.object(profiler, "request", side_effect=fake_request), \
                 patch.object(profiler.subprocess, "Popen", side_effect=launch):
                report = profiler.run_llama("server", "model", "q8",
                    {"input_tokens": 8, "output_reserve_tokens": 4, "total_tokens": 12},
                    args, Path(directory), "projector")
        self.assertEqual(report["status"], "measured")
        completions = [body for route, body in bodies if route == "/completion"]
        self.assertEqual([len(body["prompt"]) for body in completions], [8, 11, 8])
        self.assertEqual([body["n_predict"] for body in completions], [1, 1, 2])
        self.assertTrue(report["image_processing_measured"])
        self.assertFalse(report["image_at_full_context_measured"])
        self.assertFalse(report["autoregressive_max_output_measured"])

    def test_native_runner_rejects_inexact_context_occupation(self):
        child = Mock()
        child.poll.return_value = None
        watchdog = Mock()
        watchdog.__enter__ = Mock(return_value=watchdog)
        watchdog.__exit__ = Mock(return_value=False)
        watchdog.error = None
        watchdog.peaks = {}
        args = SimpleNamespace(gpu_layers=99, batch_size=512, prefill_chunk=128,
            threads=4, image_tokens=1024, limit_gib=12, timeout=100, decode_tokens=0)
        def api(url, route, body=None, **unused):
            if route == "/health": return {}
            if route == "/tokenize": return {"tokens": [1]}
            return {"tokens_evaluated": 7, "tokens_predicted": 1}
        with tempfile.TemporaryDirectory() as directory, \
             patch.object(profiler, "unused_port", return_value=9999), \
             patch.object(profiler, "Watchdog", return_value=watchdog), \
             patch.object(profiler, "memory_snapshot", return_value={}), \
             patch.object(profiler, "request", side_effect=api), \
             patch.object(profiler.subprocess, "Popen", return_value=child):
            report = profiler.run_llama("server", "model", "q8",
                {"input_tokens": 8, "output_reserve_tokens": 4, "total_tokens": 12},
                args, Path(directory))
        self.assertEqual(report["status"], "failed")
        self.assertIn("unverified prompt occupation", report["error"])

    def test_preflight_pressure_records_guard_stop_and_never_launches(self):
        args = SimpleNamespace(artifact="q4", engine=None, input_tokens=4096,
            output_tokens=256, limit_gib=12, kv_types=["q4", "q8"], execute=True,
            cache_root=Path("/cache"))
        document = {"id": "test-model", "native_context_tokens": 8192,
            "artifacts": [{"id": "q4", "format": "mlx", "compatible_engines": ["mlx-lm"],
                           "quantization_type": "q4", "repository": "test/model", "revision": "a" * 40}]}
        with tempfile.TemporaryDirectory() as directory:
            args.output_dir = Path(directory)
            with patch.object(profiler, "load_manifest", return_value=document), \
                 patch.object(profiler, "file_inventory", return_value=[]), \
                 patch.object(profiler, "hardware", return_value={}), \
                 patch.object(profiler, "pressure", return_value=2), \
                 patch.object(profiler.subprocess, "Popen") as launch:
                report = profiler.profile_one("model.yaml", args)
            launch.assert_not_called()
            self.assertEqual([row["status"] for row in report["cases"]], ["guard_stopped"] * 2)


if __name__ == "__main__":
    unittest.main()
