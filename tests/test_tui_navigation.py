"""Read-only terminal navigation smoke; isolated home, no inference or installs."""
import fcntl
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import sys
import tempfile
import termios
import time
import unittest

BINARY, CONFIG = map(Path, sys.argv[1:3])
sys.argv = sys.argv[:1]


class NavigationTests(unittest.TestCase):
    def test_relationship_navigation_and_server_subsections(self):
        with tempfile.TemporaryDirectory(prefix="mica-tui-navigation-") as temporary:
            master, slave = pty.openpty()
            fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack("HHHH", 50, 180, 0, 0))
            process = subprocess.Popen([str(BINARY), "tui", "--root", temporary,
                                        "--config-dir", str(CONFIG)],
                                       stdin=slave, stdout=slave, stderr=slave,
                                       env={**os.environ, "TERM": "xterm-256color"})
            os.close(slave)

            def read_until(expected, timeout=8):
                output = b""
                deadline = time.monotonic() + timeout
                while time.monotonic() < deadline:
                    if select.select([master], [], [], 0.1)[0]:
                        try:
                            output += os.read(master, 65536)
                        except OSError:
                            break
                    if expected.encode() in output:
                        return output
                self.fail(f"Missing {expected!r} in terminal output: {output.decode(errors='replace')[-1500:]}")

            def key(value, expected):
                os.write(master, value)
                return read_until(expected)

            try:
                read_until("Stored data")
                key(b"2", "Workload · Overview")
                key(b"/", "Filter")
                os.write(master, b"local-researcher-gguf")
                time.sleep(0.15)
                key(b"\r", "local-researcher-gguf")
                key(b"\r", "Models in local-researcher-gguf")
                key(b"\r", "Supported engines for")
                # Escape restores the selection/scope; message may be unchanged.
                key(b"\x1b", "Model · Overview")
                key(b"\x1b", "Workload · Overview")
                key(b"1", "Stored data")
                key(b"\x1b[B", "Choose workload")
                key(b"\r", "Choose a workload")
                key(b"\x1b", "Stored data")
                key(b"\x1b[B", "Detected machine")
                key(b"5", "example")
                os.write(master, b"q")
                # Drain terminal restoration bytes while waiting (PTY buffers
                # can otherwise block the UI's final write).
                deadline = time.monotonic() + 8
                while process.poll() is None and time.monotonic() < deadline:
                    if select.select([master], [], [], 0.1)[0]:
                        try:
                            os.read(master, 65536)
                        except OSError:
                            break
                self.assertEqual(process.wait(timeout=1), 0)
                self.assertFalse((Path(temporary) / "state/runtime.json").exists())
            finally:
                if process.poll() is None:
                    process.kill()
                    process.wait(timeout=8)
                os.close(master)


if __name__ == "__main__":
    unittest.main()
