# Deterministic MiniCPM-V validation assets

These generated assets avoid network-dependent test inputs and make the
expected answers unambiguous.

- `minicpm-scene.png` (SHA-256
  `7544042cb582188ed19683bc4a3effd310ea2a39dd2a5c80679a800da4e1182d`):
  white 640×480 image containing the text `MICA 31415`, a blue upper-left
  square, and a red lower-right square.
- `minicpm-sequence.mp4` (SHA-256
  `1898775b941dbd0849be05182e3a3b7530c409c57f7155e42dfa495e85657e97`):
  six-second 512×384 H.264 video at 4 fps, with two seconds each of red, green,
  and blue plus corresponding ordinal text.

They were generated locally with FFmpeg and contain no third-party media.
