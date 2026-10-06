import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
from mlx_worker import check_budget, configure_memory


class GuardTests(unittest.TestCase):
    def test_exact_input_and_generation_limits(self):
        check_budget(100, 20, 100, 20, 120)
        for prompt, output in ((101, 19), (90, 21), (100, 20)):
            with self.assertRaises(ValueError):
                check_budget(prompt, output, 100, 20, 119)

    def test_upstream_cannot_expand_allocator_limits(self):
        class FakeMX:
            def set_memory_limit(self, value): self.memory = value
            def set_wired_limit(self, value): self.wired = value
            def set_cache_limit(self, value): self.cache = value
        mx = FakeMX()
        configure_memory(mx, 1)
        mx.set_memory_limit(2 * 1024**3)
        mx.set_wired_limit(2 * 1024**3)
        mx.set_cache_limit(2 * 1024**3)
        self.assertEqual(mx.memory, 1024**3)
        self.assertEqual(mx.wired, 1024**3)
        self.assertEqual(mx.cache, 128 * 1024**2)


if __name__ == "__main__":
    unittest.main()
