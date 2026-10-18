# Dictation application draft

Status: target design, after Infer and Mica manager. App ID: `dictation`. Dictation is a standalone device-local application using local or remote inference.

## Ownership and workflow

The Dictation core/server owns recording state, permissions, shortcuts, transcription sessions, cleanup preferences and clipboard delivery. The Infer SDK supplies ASR, optional supported diarization and optional LLM cleanup. Keep the original transcript separate from cleaned text.

Opening configuration does not start recording. Activation enables explicitly approved shortcuts/background behavior. Record captures audio; stop submits it or ends a supported live session; cancel releases capture without submission. Deactivation removes active recording behavior. Auto-paste is opt-in and platform-dependent; clipboard output is available separately.

## Interfaces and connections

Provide CLI, TUI, desktop controls, a configuration web UI, native Android UI and MCP against the same core. File transcription and device microphone control are separate permissions. Use ConnectRPC/JSON and event streaming with WebSocket fallback; add reliable duplex transport only for implemented live sessions. Default new-install listener: `55003`.

Setup always selects local or remote Infer, offering discovery/reuse or approved local installation. Multiple saved connections are allowed. Remote inference processes the audio but does not control the device microphone or clipboard.

## Platform and acceptance targets

macOS/Linux have local services and native device adapters. Android has a local core/server with approved microphone access, appropriate visible foreground work, native UI and document/audio input. Terminal interfaces are available through a terminal environment. Never promise unrestricted desktop global shortcuts or injection on Android.

Acceptance covers permission refusal, record/stop/cancel, disconnect, optional cleanup, local and remote inference, supported diarization, output delivery and microphone release after errors. Closing configuration must not silently change activation state.

Draft services: [Dictation contracts](../../../../proto/mica/dictation/v1/dictation.proto). Shared rules: [application blueprint](../01-generic-app-blueprint.md).
