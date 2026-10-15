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

When installed through Mica, `mica-server app open mica-dictation --accept-code`
(**Apps → o** in its TUI) opens that setup interface in the foreground.
`app start` is the separate background/global-shortcut mode. On a headless
Linux host, install the client on your speaking device and connect it to the
Linux server for inference instead of trying to open a browser for this app.

The old browser bridge remains a diagnostic fallback (`python3 app.py` without
`--desktop`), not the default app. `mcp.py` exposes stdio models/transcribe/cleanup
tools. File uploads require `--allow-directory PATH`. Remote inference needs no
local engines, models or Mica Server. Keys stay in private files.

Only record people with appropriate permission. The server may retain backend
artifacts according to its own policies. The native client displays text in
memory and copies it to the clipboard; generated recording files are temporary.
Speaker diarization and transcript alignment are not included.
