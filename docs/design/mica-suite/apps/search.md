# Search application draft

Status: target design, after Infer and Mica manager. App ID: `search`. Search owns a local semantic index; Infer provides embeddings through a selected connection.

## Ownership and storage

The Search core/server owns authorized folders/document grants, ingestion jobs, chunking, index metadata, embedding provenance and retrieval. Begin with embedded SQLite metadata/vectors and exact similarity search; add an optional vector extension only when measurements justify it. No separate database service is required by this draft.

Record embedding model/artifact revision, dimensions, metric, normalization, query/document formatting and chunking version. Changing vector space requires a new index or explicit rebuild. Verify actual embedding modalities and prefixes before selecting a model; an app accepting video does not imply every embedding model can embed video directly.

## Input and results

Users choose a source root, recursive or single-folder ingestion, file types and exclusions. PDF results identify pages; text uses stable chunks/offsets; images keep file/region identity; video/audio use timestamped segments. Use direct multimodal embeddings only when supported, otherwise an explicit transcription/caption/rendering pipeline.

Queries return top-N results with score, source ID, file/section/page/time locator and a served preview where appropriate. Canonicalize source paths, enforce document grants and constrain symlinks. Support incremental updates, deletion and cancellation without escaping authorized roots.

## Interfaces and platforms

Provide CLI, TUI, desktop, browser, native Android and MCP clients over Search's own SDK/core/server. Use ConnectRPC/JSON and event streams with WebSocket fallback for ingestion progress. Default new-install listener: `55004`.

Each macOS/Linux/Android installation owns its local data, even when Infer is remote. Android uses user-granted document URIs; its UI/core/server are local. Setup offers local Infer discovery/reuse, approved installation or a remote connection. Mica need not stay open.

Acceptance covers authorized ingestion, correct locators, a small known-answer retrieval set, model-format correctness, no mixing of embedding spaces, incremental deletion and local/remote inference. Test the supported media types on each release target.

Draft services: [Search contracts](../../../../proto/mica/search/v1/search.proto). Shared rules: [application blueprint](../01-generic-app-blueprint.md).
