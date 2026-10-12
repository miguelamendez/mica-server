# Global scheduler validation

Validated on Linux, 2026-10-12, in an isolated checkout/application home.
Installed servers, engines and model caches were not modified.

- Release build passed with one compiler job and an 8 GiB process-tree
  watchdog; observed peak RSS was **1.15 GiB**.
- **93/93 CTest cases passed**, including 11 new scheduler scenarios,
  existing app-package/lifecycle tests, browser settings, authentication and
  configuration tests.
- **95 schema/package documents passed** validation, including regenerated
  assistant/dictation file sizes and SHA-256 manifests.
- Lightweight browser-bridge tests also passed on macOS. Native macOS rebuild
  was deferred because the memory safeguard reported warning pressure.

The new tests launch the real native proxy and small **mock engine processes**.
They verify per-request workload selection, selector stripping/conflicts,
compatible-worker PID reuse, incompatible-context replacement, priority
retention, estimate-driven eviction, oversized-worker rejection, streaming
leases, queued concurrency, live strategy changes, independent default
selection, restartable persisted snapshots and new workload discovery.

CLI lifecycle assertions verify that `config set --strategy` and
`config set --default-workload` apply and persist without changing the server
PID. Browser tests verify that app workload selection does not call the global
default-activation endpoint and that failed selections preserve the binding.

These are scheduling/control-plane tests, **not actual model inference or
quality certification**. No model weights were downloaded. Estimated admission
limits are not universal hard RSS/VRAM caps; real multi-app inference and
additional accelerator backends still require acceptance testing.

App installation and workload preparation remain separate explicit actions.
This change does not implement automatic dependency preparation while Mica is
running, publisher signatures, or a new release.

## Dashboard and scoped-credential follow-up

The subsequent dashboard change passed **94/94 CTest cases** and **95 schema/
package checks** on Linux (2026-10-12). Dashboard cases cover admin/inference
scope separation, cross-origin rejection, key revocation, accepted streams
surviving admin rotation, inference revocation surviving restart, pause/resume,
typed-job rejection, native workload cloning/app-file installation, and live
settings. The guarded build peaked at **1.15 GiB**; static dashboard assets total
**34,939 bytes** without frameworks, fonts or CDN dependencies.

These remain mock-inference/control-plane tests. Actual two-demo-app inference
and interactive browser acceptance are separate checks, not implied by the
regression count. Dashboard workload preparation adds a maintenance pause that
drains/unloads workers without terminating the control plane.
