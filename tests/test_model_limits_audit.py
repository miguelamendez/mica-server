"""Metadata-only audit tests; PyYAML developer dependency, no network/weights."""
import importlib.util
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

spec = importlib.util.spec_from_file_location("model_limits_audit",
    Path(__file__).resolve().parents[1] / "scripts/audit_model_limits.py")
audit = importlib.util.module_from_spec(spec)
spec.loader.exec_module(audit)


class ModelLimitsAuditTests(unittest.TestCase):
    def run_audit(self, document, configs):
        def fetch(repo, name):
            item = configs.get((repo, name))
            if item is None:
                return {"url": f"https://huggingface.co/{repo}/raw/main/{name}",
                        "error": "HTTP Error 404: Not Found"}
            return {"url": f"https://huggingface.co/{repo}/raw/main/{name}",
                    "sha256": "a" * 64, "document": item}
        with tempfile.TemporaryDirectory() as directory:
            manifest = Path(directory) / "model.yaml"
            manifest.write_text(audit.yaml.safe_dump(document))
            with patch.object(audit, "fetch_json", side_effect=fetch):
                return audit.audit(manifest)

    def test_generation_default_never_becomes_verified_limit(self):
        result = self.run_audit({"id": "test", "source_repository": "test/model",
            "abilities": ["text_generation"], "native_context_tokens": 8192,
            "max_output_tokens": None}, {
            ("test/model", "config.json"): {"max_position_embeddings": 8192},
            ("test/model", "generation_config.json"): {"max_tokens": 8192}})
        self.assertEqual(result["status"], "matches-architecture-config")
        self.assertIsNone(result["max_output_tokens"])
        self.assertEqual(result["generation_defaults_not_hard_limits"]["max_tokens"], 8192)

    def test_gguf_uses_only_explicit_base_config_reference(self):
        document = {"id": "test", "source_repository": "test/gguf",
            "abilities": ["text_generation"], "native_context_tokens": 8192,
            "references": [{"url": "https://huggingface.co/base/model/blob/main/config.json"}]}
        configs = {("base/model", "config.json"):
                   {"text_config": {"max_position_embeddings": 8192}}}
        result = self.run_audit(document, configs)
        self.assertEqual(result["status"], "matches-architecture-config")
        self.assertEqual(result["architecture_source"]["kind"], "referenced-base-architecture")
        self.assertIn("error", result["config_source"])
        del document["references"]
        result = self.run_audit(document, configs)
        self.assertEqual(result["status"], "needs-model-card-or-artifact-verification")

    def test_audio8_packed_positions_are_not_text_output_tokens(self):
        result = self.run_audit({"id": "audio", "source_repository": "test/audio",
            "abilities": ["speech_synthesis"], "native_context_tokens": 2048}, {
            ("test/audio", "config.json"): {"max_seq_len": 2048},
            ("test/audio", "generation_config.json"): {"max_new_tokens": 512}})
        self.assertEqual(result["context_units"], "packed-text-audio-positions")
        self.assertEqual(result["status"], "matches-architecture-config")
        self.assertIsNone(result["max_output_tokens"])

    def test_ctc_vocabulary_is_not_a_context_limit(self):
        result = self.run_audit({"id": "asr", "source_repository": "test/asr",
            "abilities": ["speech_recognition"], "native_context_tokens": None}, {
            ("test/asr", "config.json"): {"vocab_size": 16384}})
        self.assertEqual(result["context_units"], "not-applicable")
        self.assertIsNone(result["upstream_architecture_context_tokens"])
        self.assertEqual(result["status"], "not-a-causal-text-context-limit")


if __name__ == "__main__":
    unittest.main()
