# Assistant application draft

Status: target design, after Infer and Mica manager. App ID: `assistant`. Assistant is its own application, whether inference is local or remote.

## Ownership and workflow

The Assistant core/server owns conversation history, attachments, prompts, agent/tool permissions, media preparation, import/export and speech orchestration. Its clients use the Assistant SDK; the core uses the Infer SDK. Infer generates outputs and proposed tool calls but never owns the conversation or executes Assistant's local tools.

Text is the instruction. A voice note alone becomes an instruction after transcription; text plus voice uses the transcript as extra context. Upload media into authorized Assistant storage, then use model-supported processing through the selected workload. Tools execute only where Assistant has explicit permissions.

Stream text, current-step updates and optional audio through session events. Assemble one final voice reply for history and export. Support cancellation and keep replay/reconnect from repeating tools. Conversations survive a restart of the inference node.

## Interfaces and connections

Provide CLI, TUI, desktop, vanilla browser UI, native Android UI and MCP over the same core. Use ConnectRPC/JSON and event streaming with WebSocket fallback; add a live duplex transport only for a validated live-audio feature. Default new-install listener: `55002`.

Setup discovers local Infer and always offers reuse, approved local installation or a remote connection. The app remains usable without a running Mica manager. A remote browser accesses served media, not raw paths on another computer.

## Platform and acceptance targets

macOS/Linux have local servers and native clients. Android has its own local core/server, storage and permission-aware UI, even with remote inference; terminal clients can run through a terminal environment. Native capture and file grants stay on the user's device.

Acceptance covers local/remote text and media conversations, speech routing, tool permissions, archive round trips, cancellation and actual recording/device lifecycle. Unavailable workload capabilities disable relevant controls with an explanation.

Draft services: [Assistant contracts](../../../../proto/mica/assistant/v1/assistant.proto). Shared rules: [application blueprint](../01-generic-app-blueprint.md).
