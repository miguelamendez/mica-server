# Local Assistant

The existing Mica browser chat, packaged as a reference app. It supports text,
media, streaming, voice where the active workload supports it, and conversation
import/export. Python 3 is needed only for this optional standard-library HTTP
client, not for Mica's native app catalogue or GGUF engine.

Install through Apps in the TUI, prepare one bundled workload, start Mica with
that workload, then launch the app. Default URL: http://127.0.0.1:8090/chat.
Browser microphone permission is requested only when recording. Models are
owned by Mica, not by this app. Hardware compatibility is not certification.
