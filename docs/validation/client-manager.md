# Client manager and port pools: validation

Date: 2026-10-15. Platform actually built/tested: Linux x86-64.

The isolated source build passed **97/97 CTest groups**, including the existing
server regressions. The client manager tests contain four acceptance cases:

- Help is read-only; local detection reports the configured/default listener;
  installing an app with inference deferred creates no server, engine or models.
- Named remote connections authenticate the workload, redact keys, save private
  credential files, and reject binding an app to missing operation capabilities.
- The native C++/Lua TUI opens in a real PTY and exits without starting inference.
- The HTML manager rejects missing credentials/cross-origin access, opens the
  actual Assistant HTTP process through an authenticated `/apps/` route, leases
  its upstream in 55200–55399, and serves a verified static app without a process
  or port. Package receipts cannot be fetched as static app assets.

Port tests cover both bounded ranges, distinct leases, cooperating-process
exclusion, skipping an unrelated listener, exhaustion, and release/reuse. These
tests reserve slots temporarily; they do not launch hundreds of services.
Server TUI navigation verifies the installed-app/client-discovery separation.
Updater tests verify recoverable replacement of the client manager binary along
with the server, Dictation and shared assets.

Offline schema validation passed for all 95 persisted example/fixture documents;
all schema definitions, including the new client connections schema, were also
checked. Node tests passed for dashboard popup fallback, readiness, native-app
guidance, deep links and route validation. Client JavaScript passed syntax checks.

A staged installation was checked from outside the source checkout: binary help,
bundled app discovery, relative shared-asset resolution and local detection.
The unstripped Linux client binary was approximately 4.4 MiB. Native builds used
one compiler job and an 8 GiB process-tree cap; the initial full build peaked at
approximately 1.16 GiB RSS.

## Limits of this evidence

Inference in these tests is a mock service; no new model quality/speed claims.
Fresh-machine source bootstrap, large model downloads, macOS compilation, and
microphone/global-shortcut/paste interaction were not exercised in this pass.
The HTML manager was tested through HTTP and frontend logic checks, not an
automated real-browser click-through suite. Local preparation and missing-server
installation code exist, but should not be described as fresh-host certified.

Live installations and their explicit legacy listener ports were not replaced
or migrated. The Mac's conservative build-headroom check still declined a build;
it was not bypassed. See [client setup](../client.md) and [port migration](../ports.md).
