"""Offline runner configuration tests; no model/GPU dependency."""
import importlib.util
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import Mock, patch


spec = importlib.util.spec_from_file_location('context_sweep',
    Path(__file__).resolve().parents[1] / 'scripts/benchmark_context_sweep.py')
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)


class ContextSweepTests(unittest.TestCase):
    def setUp(self):
        self.args = SimpleNamespace(binary=Path('/test/server'), model=Path('/test/target'),
            projector=Path('/test/projector'), dflash=Path('/test/dflash'), mtp=Path('/test/mtp'),
            port=8108, context=139264, text_layers=56, vision_layers=48)

    def test_text_pair_has_identical_target_placement_and_precision(self):
        baseline = sweep.command(self.args, 'text-none')
        drafted = sweep.command(self.args, 'text-dflash')
        for flag in ['-m', '-c', '-ngl', '-ctk', '-ctv', '--threads', '--batch-size', '--ubatch-size']:
            self.assertEqual(baseline[baseline.index(flag) + 1], drafted[drafted.index(flag) + 1])
        self.assertNotIn('--mmproj', drafted)
        self.assertNotIn(str(self.args.mtp), drafted)
        self.assertEqual(drafted[drafted.index('--spec-draft-model') + 1], str(self.args.dflash))

    def test_vision_pair_has_identical_projector_placement(self):
        for mode in ['vision-none', 'vision-mtp']:
            cmd = sweep.command(self.args, mode)
            self.assertIn('--no-mmproj-offload', cmd)
            self.assertEqual(cmd[cmd.index('-ngl') + 1], '48')
            self.assertEqual(cmd[cmd.index('--mmproj') + 1], str(self.args.projector))
            self.assertNotIn(str(self.args.dflash), cmd)
        cmd = sweep.command(self.args, 'vision-mtp')
        self.assertEqual(cmd[cmd.index('--spec-draft-model') + 1], str(self.args.mtp))

    def test_baseline_explicitly_disables_speculation(self):
        for mode in ['text-none', 'vision-none']:
            cmd = sweep.command(self.args, mode)
            self.assertEqual(cmd[cmd.index('--spec-type') + 1], 'none')
            self.assertNotIn('--spec-draft-model', cmd)

    def test_existing_gpu_profile_batching_and_projector_are_preserved(self):
        self.args.vision_layers = 99
        self.args.text_layers = 99
        self.args.batch = 512
        self.args.ubatch = 128
        self.args.projector_on_gpu = True
        self.args.require_all_gpu = True
        cmd = sweep.command(self.args, 'vision-mtp')
        for flag, value in [('-ngl', '99'), ('--batch-size', '512'),
                            ('--ubatch-size', '128'), ('--device', 'CUDA0')]:
            self.assertEqual(cmd[cmd.index(flag) + 1], value)
        self.assertNotIn('--no-mmproj-offload', cmd)
        self.assertEqual(cmd[cmd.index('--fit') + 1], 'off')

    def test_atomic_result_save_replaces_previous_result(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / 'result.json'
            sweep.save(path, {'complete': False})
            sweep.save(path, {'complete': True})
            self.assertEqual(json.loads(path.read_text()), {'complete': True})
            self.assertFalse(path.with_suffix('.json.tmp').exists())

    def test_growing_requests_reuse_prefix_not_previous_answers(self):
        bodies = []
        process = Mock()
        process.poll.return_value = None
        process.returncode = 0

        def fake_request(url, path, body=None, timeout=3600):
            if path == '/health':
                return {}
            if path == '/tokenize':
                return {'tokens': [1] * (10000 if len(body['content']) > 1000 else 10)}
            if path == '/detokenize':
                return {'content': 'token ' * len(body['tokens'])}
            if body['max_tokens'] == 32:
                return {'choices': [{'message': {'content': '391'}}]}
            bodies.append(body)
            actual = [4000, 8000][len(bodies) - 1]
            return {'choices': [{'message': {'content': 'MODEL ANSWER NOT REUSED'}}],
                    'usage': {'prompt_tokens': actual, 'completion_tokens': 2304},
                    'timings': {'predicted_n': 2304, 'predicted_ms': 115150,
                                'predicted_per_second': 20,
                                'cache_n': 0 if len(bodies) == 1 else 3000}}

        with tempfile.TemporaryDirectory() as directory:
            self.args.output_dir = Path(directory)
            self.args.inputs = [4096, 8192]
            self.args.output_tokens = 2304
            self.args.minimum_gpu_free_mib = 768
            with patch.object(sweep, 'request', side_effect=fake_request), \
                    patch.object(sweep.subprocess, 'Popen', return_value=process):
                result = sweep.run_mode(self.args, 'text-none')
            self.assertTrue(result['complete'])
            self.assertTrue(all(stage['smoke_passed'] for stage in result['stages']))
            self.assertFalse(result['stages'][0]['cache_reused'])
            self.assertTrue(result['stages'][1]['cache_reused'])
            first, second = [body['messages'][0]['content'] for body in bodies]
            self.assertTrue(second.startswith(first))
            self.assertNotIn('MODEL ANSWER NOT REUSED', second)
            self.assertTrue(all(body['cache_prompt'] for body in bodies))
            self.assertTrue(all(not body['chat_template_kwargs']['enable_thinking'] for body in bodies))
            self.assertTrue((self.args.output_dir / 'text-none-8192.response.json').is_file())

    def test_gpu_only_run_rejects_unverified_placement_before_inference(self):
        process = Mock()
        process.poll.return_value = None
        process.returncode = 0
        with tempfile.TemporaryDirectory() as directory:
            self.args.output_dir = Path(directory)
            self.args.require_all_gpu = True
            self.args.text_layers = 99
            self.args.minimum_gpu_free_mib = 768
            with patch.object(sweep, 'request', return_value={}) as api, \
                    patch.object(sweep.subprocess, 'Popen', return_value=process):
                result = sweep.run_mode(self.args, 'text-none')
            self.assertFalse(result['all_gpu_verified'])
            self.assertIn('no CPU fallback permitted', result['error'])
            self.assertEqual([call.args[1] for call in api.call_args_list], ['/health'])


if __name__ == '__main__':
    unittest.main()
