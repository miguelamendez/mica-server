# Dictation demo

A small open-source WhisperFlow-style client, not the proprietary WhisperFlow
product. Native microphone → Mica ASR → optional LLM cleanup → clipboard, with
opt-in paste into the original focused application on macOS.

`mica-dictation --tui` sets local/remote connections. Ctrl+Alt/Option+R toggles
recording (two-minute cap). macOS requires microphone permission and Input
Monitoring for global shortcuts; automatic paste needs Accessibility. Linux
uses PipeWire/ALSA recording, X11 shortcuts when available and clipboard tools;
Wayland global shortcuts and Linux auto-type are not implemented. The TUI's
Record button does not require a global shortcut.

The old browser bridge remains a diagnostic fallback (`python3 app.py` without
`--desktop`), not the default app. `mcp.py` exposes stdio models/transcribe/cleanup
tools. File uploads require `--allow-directory PATH`. Remote inference needs no
local engines, models or Mica Server. Keys stay in private files.

Only record people with appropriate permission. The server may retain backend
artifacts according to its own policies; this demo retains text in the current
browser page only. Speaker diarization and transcript alignment are not included.
