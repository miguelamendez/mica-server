# Request-scoped workloads and global scheduling

A workload groups model configurations. It does not own an exclusive server or
memory pool. Several apps can stay open and send requests to different workloads
through one Mica server; residency follows requests, not which app was opened last.

```text
App A → request + workload A ─┐
                            ├→ select model/config → reuse compatible worker
App B → request + workload B ─┘                       or estimate → evict idle → load
                                                     ↓
                                              hold lease until response ends
```

## Choose one server strategy

| Strategy | Behavior |
| --- | --- |
| `single` | One resident model worker across all workloads. A request for another worker waits for busy work to finish, then replaces it. |
| `high` | Keep as many workers as the budget permits. Evict idle workers when a new worker needs space. |
| `balanced` | Default. Keep shared compatible workers and high-priority workers (priority ≥100); drop idle lower-priority workers on workload transitions. Evict higher-priority workers too if necessary to admit a request. |

```sh
mica-server start --strategy balanced
mica-server config set --strategy single
```

The setting is persisted as `scheduling_strategy` in `~/.mica/config/server.json`.
Changing it with `config set` applies live; no restart is necessary. A temporary
API override uses `POST /admin/scheduler` with `{"strategy":"high"}`; this does
not persist configuration. Already-running responses are not interrupted.

Workload planning strategies (`all`, `sequential`, `balanced`) remain useful for
memory reports and installation validation. They do not replace the global
serving strategy or impose the previous workload's worker-count limit on another
app's requests. A workload memory ceiling still constrains admission.

## Bind each request

Use `X-Mica-Workload: WORKLOAD_ID` on inference requests, or a `workload` field
in JSON/multipart requests. Conflicting selectors are rejected. Mica removes
the selector before forwarding to an engine. `GET /v1/models?workload=ID` lists
that workload's selected model variants and capabilities.

Without a selector, a request captures the current server default.
`workload activate ID` changes that default; it does not interrupt bound apps
or immediately unload all models. Residency is reconciled on subsequent requests.
New installed workload YAMLs referencing registered models can be discovered
without restarting. New model/engine registry definitions still need the
existing preparation/reload workflow.

Demo apps accept `app start ID --workload WORKLOAD_ID --accept-code`; otherwise
Mica chooses a compatible bundled workload. The assistant's picker changes
only that app bridge's binding. Running-app records provide priority hints;
these are preferences, not memory reservations or guarantees of residency.

## Admission, reuse and eviction

1. Validate the workload and selected model variant against global allocations.
2. Estimate full memory: weights, projector/drafter/components, configured KV
   context, batching and runtime overhead; honor placement and dedicated-device limits.
3. Reuse only compatible workers. Engine, artifact, context, batching, KV,
   placement, reservation overrides and native options must match.
4. Reconcile according to the global strategy; evict idle lower-priority workers
   first, then use expiration, residency preference and last-use time.
5. If busy workers prevent admission, wait for their leases to finish, bounded
   by `policy.lua`'s `queue_timeout_seconds`. This is a bounded wait, not a FIFO
   fairness guarantee. An individually oversized worker is rejected before loading.

Priority is retained at the highest priority seen by a compatible resident
worker; compatible running-app bindings can raise it. Idle timers still apply.
An in-flight stream holds its worker until streaming completes or is cancelled.
`pinned` is an idle-retention preference, not an absolute exemption from eviction
under the global scheduler. Conflicting configurations of the same model variant
drain and replace its worker instead of running two copies.

`GET /admin/models` exposes the global strategy, queued request count, resident
workers and RAM/VRAM reservations. These are **estimates**, not an OS-enforced
hard memory cap. Admission tests use mock engines; actual inference quality and
peak memory require separate backend/model tests.

## Lifecycle boundaries

Installing app files, launching/stopping an app, choosing its workload, changing
the default workload or changing the scheduler strategy does not restart Mica.
Listener, credentials and allocation edits still use explicit restart handling.
The existing app `install`/`setup` steps remain separate: automatic guided
engine/model preparation while the server is running is not implemented here.
