#!/usr/bin/env python3
"""Read-only native TUI screen capture to SVG (optional developer dependency).

uv run --with pyte python scripts/capture_tui.py --binary build/mica-server \
  --config-dir config --output docs/assets/tui-workload.svg

Uses an isolated app home, no model download/inference, no private API keys.
The SVG is rendered from actual terminal cells, not a hand-drawn UI mockup.
"""
import argparse
import codecs
import fcntl
import json
import os
from pathlib import Path
import pty
import select
import struct
import subprocess
import tempfile
import termios
import time
from xml.sax.saxutils import escape

import pyte


def render(screen):
    cell_width, line_height, margin = 8.4, 18, 24
    width, height = screen.columns * cell_width + margin * 2, screen.lines * line_height + margin * 2
    colors = {'default': '#cdd6f4', 'black': '#45475a', 'red': '#f38ba8', 'green': '#a6e3a1',
              'brown': '#f9e2af', 'blue': '#89b4fa', 'magenta': '#cba6f7', 'cyan': '#94e2d5', 'white': '#cdd6f4'}
    def color(value, default):
        if value == 'default':
            return default
        return colors.get(value, '#' + value if len(value) == 6 else default)
    output = [f'<svg xmlns="http://www.w3.org/2000/svg" width="{width}" height="{height}" viewBox="0 0 {width} {height}">',
              '<title>Mica native terminal interface — workload details</title>',
              '<desc>Actual read-only terminal capture. Isolated app home; no model inference or private settings.</desc>',
              '<rect width="100%" height="100%" rx="18" fill="#1e1e2e"/>',
              '<g font-family="SFMono-Regular,Consolas,Liberation Mono,monospace" font-size="13.5">']
    for row in range(screen.lines):
        for column in range(screen.columns):
            cell = screen.buffer[row][column]
            fg, bg = color(cell.fg, '#cdd6f4'), color(cell.bg, '#1e1e2e')
            if cell.reverse:
                fg, bg = bg, fg
            x, y = margin + column * cell_width, margin + row * line_height
            if bg != '#1e1e2e':
                output.append(f'<rect x="{x}" y="{y}" width="{cell_width}" height="{line_height}" fill="{bg}"/>')
            if cell.data.strip():
                weight = ' font-weight="700"' if cell.bold else ''
                output.append(f'<text x="{x}" y="{y + 14}" fill="{fg}"{weight}>{escape(cell.data)}</text>')
    output.append('</g></svg>')
    return '\n'.join(output) + '\n'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=Path, required=True)
    parser.add_argument('--config-dir', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    parser.add_argument('--workload', default='mac_assistant')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(prefix='mica-readme-capture-') as temporary:
        root = Path(temporary)
        (root / 'config').mkdir()
        (root / 'config/server.json').write_text(json.dumps({'schema': 1, 'default_workload': args.workload}))
        # A demo allocation policy, not fake installation or inference data.
        (root / 'config/machine.yaml').write_text('schema: 1\nlimits:\n  inference: {ram_gib: 16}\n')
        master, slave = pty.openpty()
        fcntl.ioctl(slave, termios.TIOCSWINSZ, struct.pack('HHHH', 48, 160, 0, 0))
        child = subprocess.Popen([str(args.binary.resolve()), 'tui', '--root', str(root),
                                  '--config-dir', str(args.config_dir.resolve())],
                                 stdin=slave, stdout=slave, stderr=slave,
                                 env={**os.environ, 'TERM': 'xterm-256color'})
        os.close(slave)
        screen = pyte.Screen(160, 48)
        stream, decoder = pyte.Stream(screen), codecs.getincrementaldecoder('utf-8')('replace')
        def read(seconds):
            deadline = time.monotonic() + seconds
            while time.monotonic() < deadline:
                if select.select([master], [], [], 0.05)[0]:
                    try:
                        chunk = os.read(master, 65536)
                    except OSError:
                        break
                    if not chunk:
                        break
                    stream.feed(decoder.decode(chunk))
        try:
            read(2)
            os.write(master, b'2')
            read(0.3)
            os.write(master, b'/')
            read(0.2)
            os.write(master, args.workload.encode())
            read(0.3)
            os.write(master, b'\r')
            read(0.5)
            if not any(args.workload in line for line in screen.display):
                raise RuntimeError('Native TUI did not reach the requested workload view')
            svg = render(screen)
            for private in (str(root), str(args.config_dir.resolve()), str(Path.home())):
                if private in svg:
                    raise RuntimeError('Capture contains a local path; choose a different view')
            args.output.parent.mkdir(parents=True, exist_ok=True)
            args.output.write_text(svg)
            os.write(master, b'q')
            read(0.5)
            child.wait(timeout=5)
            print(f'Saved actual native TUI capture: {args.output}')
        finally:
            if child.poll() is None:
                child.kill()
                child.wait(timeout=5)
            os.close(master)


if __name__ == '__main__':
    main()
