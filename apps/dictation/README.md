# Dictation demo

A small open-source WhisperFlow-style reference app, not the proprietary
WhisperFlow product. Browser microphone or uploaded audio → Mica ASR → optional
LLM cleanup → editable text → explicit copy. Ctrl/Cmd+Shift+R works only while
the browser page is focused. No system-wide hotkeys or automatic typing.

Python 3's standard library runs the optional loopback bridge; no pip packages.
The API key stays in the private Mica configuration, not browser storage. Default
URL: http://127.0.0.1:8091/. Stop/start the app separately from the model server.
Prepare a bundled workload and explicitly select it in Mica before launching.
Missing operations after a workload switch disable the relevant feature.

Only record people with appropriate permission. The server may retain backend
artifacts according to its own policies; this demo retains text in the current
browser page only. Speaker diarization and transcript alignment are not included.
