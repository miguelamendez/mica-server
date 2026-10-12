# Local Assistant

The existing Mica browser chat, packaged as a reference app. It supports text,
media, streaming, voice where the active workload supports it, and conversation
import/export. Python 3 is needed only for this optional standard-library HTTP
client, not for Mica's native app catalogue or GGUF engine.

Install through Apps in the TUI, prepare one bundled workload, start Mica with
that workload, then launch the app. Default URL: http://127.0.0.1:8090/chat.
Browser microphone permission is requested only when recording. Models are
owned by Mica, not by this app. Hardware compatibility is not certification.

The dashboard opens this UI through `/apps/mica-assistant/chat` on its own port.
A standalone download can run the app on your device and use a remote server;
local Mica installation remains optional. Add named server connections once,
then select them without reinstalling.

`python3 mcp.py --connection NAME` exposes stdio `models` and `assistant_chat`
tools. Explicitly allow upload roots with `--allow-directory PATH`; no file
uploads are allowed by default. The MCP adapter has no SDK/pip dependency and
does not start the browser or install inference engines.
