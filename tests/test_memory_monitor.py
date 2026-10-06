"""Offline tests: no model loading, process termination, or sysctl mutation."""
import subprocess
import sys
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "scripts"))
import run_memory_limited as monitor


class MemoryMonitorTests(unittest.TestCase):
    def test_pressure_levels(self):
        for level in (1, 2, 4):
            with self.subTest(level=level), patch.object(monitor.subprocess, "run") as run:
                run.return_value.stdout = f"{level}\n"
                self.assertEqual(monitor.macos_memory_pressure(), level)
                self.assertEqual(run.call_args.args[0],
                                 ["sysctl", "-n", "kern.memorystatus_vm_pressure_level"])
                self.assertEqual(run.call_args.kwargs["timeout"], 3)

    def test_unknown_pressure_fails_closed(self):
        for value in ("0", "3", "unknown"):
            with patch.object(monitor.subprocess, "run") as run:
                run.return_value.stdout = value
                with self.assertRaises(ValueError):
                    monitor.macos_memory_pressure()

    def test_pressure_probe_failure_propagates(self):
        with patch.object(monitor.subprocess, "run",
                          side_effect=subprocess.TimeoutExpired("sysctl", 3)):
            with self.assertRaises(subprocess.TimeoutExpired):
                monitor.macos_memory_pressure()

    def test_rss_includes_descendants_not_unrelated_processes(self):
        table = {10: (1, 100), 11: (10, 200), 12: (11, 300), 13: (1, 9000)}
        with patch.object(monitor, "process_table", return_value=table):
            self.assertEqual(monitor.tree_rss_kib(10), 600)

    def test_existing_pressure_refuses_launch(self):
        with patch.object(sys, "argv", ["monitor", "--limit-gib", "16",
                                      "--macos-pressure-limit", "2", "--", "unused"]), \
             patch.object(sys, "platform", "darwin"), \
             patch.object(monitor, "macos_memory_pressure", return_value=2), \
             patch.object(monitor.subprocess, "Popen") as launch:
            self.assertEqual(monitor.main(), 75)
            launch.assert_not_called()


if __name__ == "__main__":
    unittest.main()
