# Apps: clients packaged with default workloads

Apps are optional programs that call Mica's API. Workloads still own the model
collection, context, placement and residency policy; engines own inference.
Installing an app never silently changes the active workload or starts code.

## Use the TUI

Open `mica-server tui`, press **7** for **Apps**, and select a demo:

1. **Enter** inspects its manifest, permissions and requirements.
2. **i** installs verified files and registers its bundled workloads.
3. **w** prepares a bundled workload; the editable field lets you choose its ID.
4. Select/start that workload in **Server** (or hot-swap a prepared workload).
5. Return to **Apps**, press **s**, and confirm execution of trusted code.
6. Open the URL shown by `app show`/the manifest: assistant uses
   `http://127.0.0.1:8090/chat`; dictation uses `http://127.0.0.1:8091/`.

**x** stops the app without stopping Mica. **d** uninstalls it. The official
`mica-examples` source is included offline with every matching installation;
listing it does not download models or require internet. Nothing is prelaunched.

The assistant packages the existing browser chat. Dictation records/uploads
audio, transcribes it, optionally cleans up with an LLM, and lets you copy text.
Its shortcut works only in the focused browser page. Global hotkeys, automatic
typing, transcript/speaker alignment and desktop wrappers are future additions.
Both optional demos need Python 3's standard library, **no pip environment**.
The native C++/Lua app manager itself does not require Python.

## CLI

```sh
mica-server app list
mica-server app show mica-dictation
mica-server app install mica-dictation --accept-code
mica-server app setup mica-dictation mica-dictation--mac
mica-server start --workload mica-dictation--mac
mica-server app start mica-dictation --accept-code
mica-server app stop mica-dictation
```

Choose the CUDA workload instead on compatible NVIDIA hardware. Mica's global
machine allocation must satisfy the selected workload; change the allocation
through Settings if appropriate. Bundled manifests are examples, not a promise
of certification on every machine. An app can also use another compatible
workload rather than its bundled default.

## Add another source

In **Apps → Add source**, enter a canonical GitHub/Hugging Face repository URL,
or a direct HTTPS `apps.yaml`/`app.yaml` URL. CLI equivalents:

```sh
mica-server app source-add my-apps https://github.com/OWNER/REPO
mica-server app source-add research https://huggingface.co/OWNER/REPO
mica-server app source-add custom https://example.com/apps.yaml
mica-server app list
mica-server app show my-apps:APP_ID
mica-server app install my-apps:APP_ID --accept-code
```

Repository discovery resolves an immutable HEAD/main commit, then searches
`apps/apps.yaml`, `apps.yaml`, and `app.yaml`. GitHub discovery needs `git`;
HTTPS discovery needs `curl`. Hugging Face model/dataset/Space repositories are
supported. Other hosts need a direct HTTPS manifest/index URL, not an arbitrary
web page. Re-run `source-add` with the same source ID to explicitly refresh it.
No authentication tokens are accepted in URLs; private-source authentication
and signed publishers are not implemented.

The cached catalogue works offline. Source entries are stored in
`~/.mica/config/app-sources.json`; they are discovery metadata, not YAML workload
policy. Remote catalogue IDs are namespaced as `SOURCE:APP_ID`. App IDs are unique
per installation; selecting the same ID from another publisher cannot replace
an installed app silently. Removing a source keeps installed apps discoverable.

## Package contracts

Single-app repositories place `app.yaml` at their root. Multi-app repositories
use an index, with manifest paths relative to that index:

```yaml
schema: 1
apps:
  - id: dictation
    name: Dictation
    description: Local speech transcription and cleanup.
    manifest: dictation/app.yaml
    sha256: "<SHA-256 of the exact app.yaml bytes>"
```

An app manifest identifies its publisher and declares verified package files:

```yaml
schema: 1
id: dictation
name: Dictation
description: Local speech transcription and cleanup.
version: 0.1.0
source:
  repository: https://github.com/OWNER/REPO
  path: .
  # revision: <optional immutable 40/64-character commit digest>
distribution:
  format: files
  files:
    - path: app.py
      size_bytes: 1234
      sha256: "<64 lowercase hexadecimal characters>"
      # url: https://example.com/releases/0.1.0/app.py
    # Include README, assets and every referenced workload with their hashes.
requirements:
  operations: [audio.transcribe]
permissions: [microphone, clipboard]
launch:
  executable: python3
  arguments: ["{app_dir}/app.py", "--config", "{mica_config}", "--port", "{port}"]
  port: 8091
  path: /
workloads: []
```

This is a **file-based package installer**, not a general-purpose package manager:
1–512 files, at most 64 MiB each and 256 MiB total. Archives, arbitrary dependency
recipes, automatic app upgrades, signatures and executable sandboxes are not
implemented. Files default to URLs relative to the app manifest; explicit HTTPS
URLs may point elsewhere. Paths must be safe ASCII relative paths. No symlinks,
implicit shell interpretation or path traversal are accepted. `source.repository` is
attribution; `distribution` determines the exact bytes downloaded.

`launch.port` and `launch.path` are optional together: omit both for a headless
or native desktop app. `launch.executable` can name a program on PATH, or a
verified package file such as `{app_dir}/bin/app`. The latter must declare
`executable: true` in its distribution file record. Arguments are passed as an
argv array, not interpreted as a shell command.

Bundled workloads are schema-5 YAMLs registered as `APP_ID--WORKLOAD_ID`, with
their models checked against Mica's model/engine registry before registration.
Unknown models/engines require registration separately; this version does not
silently import additional model/engine manifests from app publishers.

See [app schema](../schemas/app-v1.schema.json),
[index schema](../schemas/app-index-v1.schema.json) and the
[demo manifests](../apps/apps.yaml) for complete examples.

## Integrity, permissions and lifecycle

Mica stages files, verifies byte sizes and **SHA-256 before registration**, then
publishes the installed directory under `~/.mica/apps/APP_ID`. A manifest/file
mismatch aborts installation; no code runs. Every launch verifies installed
package files again. Checksums from the same publisher detect inconsistency,
**not publisher impersonation**. `--accept-code` means you trust the publisher;
their code runs as your user, not inside a sandbox. Declared permissions are
informational; actual microphone/clipboard/accessibility approval belongs to
the OS/browser. The Mica configuration may contain credentials—only launch apps
you trust with that access. Never install untrusted apps because a hash matches.

Launching requires a running Mica workload whose model interactions and engine
contracts provide the required operations. It does not change the workload.
The dictation client refreshes available operations before each transcription;
if a switch removes ASR/cleanup, the relevant feature becomes unavailable.
Backend dependencies and models are installed by ordinary workload setup and
reused across apps. App process state/logs live in `~/.mica/state/apps/`.
Launch reports a process and URL, not an inference/readiness certification.

Uninstall refuses while the app runs or its workload is active/default. It
removes unchanged registered workloads, preserves edited ones, and moves app
files to a recoverable `.removed-…` directory. Shared model/engine caches and
user data are not deleted. Do not store personal data inside package files:
put it in an application-specific data directory outside the immutable package.

For maintainers, `scripts/package_demo_apps.py` refreshes generated assistant
assets and package hashes after edits. Schema validation also verifies hashes.
