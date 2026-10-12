# README positioning and presentation

Reviewed 2026-10-12. The README should explain a product, not serve as a build
log or a catalog of whichever models happened to be tested first.

## Reference patterns

| Primary reference | Useful presentation pattern | Application to Mica |
| --- | --- | --- |
| [Ollama README](https://github.com/ollama/ollama#readme) | A short identity statement, installation paths and immediate usage before deeper API documentation | Keep the opening concise; lead into installing/running the TUI, not compiler internals |
| [llama.cpp README](https://github.com/ggml-org/llama.cpp#readme) | Clear project purpose and explicit hardware/implementation scope | Separate native control-plane footprint from optional inference engines and their requirements |
| [vLLM README](https://github.com/vllm-project/vllm#readme) | Groups serving/performance capabilities and routes advanced details into documentation | Group Mica's workload, orchestration, API and configuration strengths rather than listing every field |
| [Open WebUI README](https://github.com/open-webui/open-webui#readme) | Prominent visual demonstrations, accessible feature explanations and install guidance | Include a real Mica TUI capture and links to the chat demo, without copying visual assets |

These are established, highly starred projects in the relevant ecosystem, not
a claim that presentation alone causes stars or that they are a global ranking.
No source prose or screenshots are copied into Mica.

## Proposed narrative

1. Identity: general-purpose local AI hosting with one API and a native TUI.
2. Distinction: a workload is a shareable model collection, not one fixed model
   or engine. Explain multi-modality, simultaneous residency when memory permits,
   and swap-on-demand when it does not.
3. Demonstration: actual TUI capture, followed by a compact capabilities table.
4. First success: install the distribution, open the TUI, choose/install/start a
   workload. Give CLI equivalents without overwhelming the entry point.
5. Configuration: one dependency diagram and links to YAML schemas/design.
6. Interfaces: engine families, operation-specific API routes and browser chat.
7. Maintenance: one-command update channels and confirmed settings restart.
8. Reality check: alpha warning, adapter/certification and memory-estimate limits.
9. Roadmap: native macOS/Linux desktop wrappers for the TUI, generation engines
   and further hardware testing. Explicitly distinguish plans from shipped features.

## Claims to avoid

- Do not claim every model/engine combination already runs. Mica is engine-
  agnostic at the control plane, but new engines need implemented typed adapters.
- Do not promise all workload models stay resident regardless of memory limits.
- Do not call admission estimates a universal kernel-enforced hard RAM/VRAM cap.
- Do not advertise native CUDA/ROCm/XPU vLLM certification as complete.
- Do not present the desktop-app roadmap or unsupported generation endpoints as shipped.
- Do not substitute interface mockups for real capture evidence or include local
  usernames, paths, tokens or private workload information in public assets.

Suggested repository description:

> Configurable local AI hosting: share YAML workloads, serve multimodal models across inference engines, and load or swap them through one API and native TUI.
