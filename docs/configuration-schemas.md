# Configuration schema index

Mica has four profiling layers plus a separate server configuration. The machine
layer has **two contracts**: rediscovery can replace facts, but never user limits.
All schemas use JSON Schema 2020-12; they describe YAML or JSON documents, not
the file extension. Version numbers are per contract, not a global release number.
Their `$id` values point to the published raw GitHub files, so relative references
can resolve outside the local repository too.

| Contract | File | Active schema | Owns |
| --- | --- | --- | --- |
| Hardware facts | `~/.mica/state/hardware.yaml` | [hardware v1](../schemas/hardware-v1.schema.json) | OS, CPU, RAM/unified memory, accelerators, runtime APIs, toolchains |
| Machine policy | `~/.mica/config/machine.yaml` | [machine policy v1](../schemas/machine-policy-v1.schema.json) | Allowed devices, global inference allocations, CPU/build limits |
| Engine | `config/engines/ID.yaml` | [engine v2](../schemas/engine-v2.schema.json) | Hardware support, installation/build recipes, launchers, endpoint contracts |
| Model | `config/model-manifests/ID.yaml` | [model v2](../schemas/model-v2.schema.json) | Modalities, abilities, context guidance, engine-compatible artifacts, components, provenance and memory metadata |
| Workload | `config/workloads/ID.yaml` or a user file | [workload v5](../schemas/workload-v5.schema.json) | Description, model collection, optional pins/defaults, context, batching, KV cache, placement, priority, residency and optional workload ceiling |
| Server/client settings | `~/.mica/config/server.json` | [server config v1](../schemas/server-config-v1.schema.json) | Bind address, port, default workload, API-key file; optional browser-client settings |

The workload catalog is an index of workload documents. Modality/ability/operation
values share the [vocabulary schema](../schemas/vocabulary-v1.schema.json).
Compatibility, cache presence and installation status shown in the TUI are
derived inventory—not fields to copy into all the manifests.

## Validation and versions

Native loaders validate known fields, types and relationships. Model/workload
resolution additionally checks context ceilings, engine/artifact compatibility,
memory allocations and placement. JSON Schema alone cannot establish that a
GPU exists, a driver works, or measured peak memory fits.

Models, engines and workloads were updated for their current contracts; machine
policy v1 already covers the current allocations and build limits. Hardware v1
and server-config v1 now have explicit published schemas too. New server-config
writes include `schema: 1`; existing unversioned server JSON remains readable.
This exception is for server configuration, **not** JSON workload profiles.

Canonical hardware YAML contains observations only. Older JSON hardware fixtures
may include optional derived `backend_targets`; user budgets belong exclusively
in machine policy. The hardware loader rejects budgets mixed into facts,
negative memory, duplicate accelerator IDs and invalid core counts.

Server configuration rejects unknown keys, invalid ports, versions and types.
Prefer `api_key_file` over an inline key. Both server and optional UI inline keys
are redacted from `config show` and TUI snapshots. TUI/API responses add diagnostic
fields such as `api_key_configured`; they are not settings documents to save back.

Run development-time validation with:

```sh
uv run --with pyyaml --with jsonschema python scripts/validate_manifest_schemas.py
ctest --test-dir build --output-on-failure -j1
```

The schema script checks packaged manifests, workload catalog, server example,
and hardware/machine fixtures. Native regression tests cover rejected hardware
and server documents plus CLI/TUI secret redaction. These Python dependencies
are developer tools, not required by a native serving workload.

Historical schema files remain for explicit legacy fixtures. New configurations
should use the active contracts above; changing UI layout does not require
changing a persisted schema's version.
