# App package validation

Validated on Apple Silicon/macOS, 2026-10-12.

- Native CLI/TUI builds passed with one compiler job, a 6 GiB process-tree
  watchdog and warning-pressure cancellation. Observed peak RSS was 1.25 GiB.
- All **92 CTest cases** passed, including app lifecycle, actual app process
  launch/stop, checksum rejection, unsafe manifest rejection, headless manifests,
  TUI navigation, API authentication, native updater and existing regressions.
- **95 configuration/package documents** passed schema validation, including
  exact file sizes, package hashes and manifest/index hashes for both demos.
- A staged installation discovered its bundled catalogue without the source
  checkout or network; neither demo was automatically installed or launched.
- Public GitHub discovery resolved the repository HEAD to an immutable commit
  and fetched the app index without executing code.
- The packaged binary downloaded Dictation from that pinned public revision,
  verified its manifest and every payload hash, and registered both workloads
  in an isolated application home. It did not launch code or download weights.
- The README screenshot was captured from the real native TUI in an isolated
  application home, not generated as an interface mockup.

The dictation process/bridge tests use a local **mock model server**. They verify
HTTP routing, key forwarding, origin rejection, operation compatibility and
lifecycle—not transcription accuracy, microphone hardware or cleanup quality.
No model weights were downloaded for these package tests. The existing assistant
client is packaged unchanged; prior model validations remain separate evidence.

Remaining scope: real microphone/ASR/cleanup user acceptance, desktop permissions
and global hotkeys, native publisher signatures, upgrades, private repositories,
and testing source adapters on additional hosting/hardware combinations. No
release was created as part of this change.
