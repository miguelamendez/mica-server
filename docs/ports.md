# Network ports and reusable service pools

Mica-owned default listeners use **55xxx** ports. These are application
conventions, not IANA registrations or operating-system reservations. Existing
explicit server-port settings are not silently overwritten. Never kill a process
or steal a busy port to make a Mica listener fit.

| Port / range | Purpose | Exposure |
| --- | --- | --- |
| `55000` | Inference API, optional `/dashboard`, server-hosted `/apps/APP_ID/` | Loopback by default; explicit trusted-LAN binding |
| `55001` | Mica Client HTML manager and device-hosted `/apps/APP_ID/` | Loopback only |
| `55200–55399` | App HTTP service pool: **200 slots** | Private loopback upstreams |
| `55900–55999` | Inference worker pool: **100 slots** | Private loopback upstreams |

`/v1/chat/completions`, `/v1/models`, speech, embeddings and other API endpoints
share the server listener. Dashboard authentication does not require another
port. Client control credentials are separate from server inference/admin keys.

## Allocation and reuse

Pools are **not prestarted processes**. Acquire one available slot only when an
HTTP app service or inference worker starts. Skip slots held by another Mica
instance or occupied by an unrelated listener. Reuse a slot only once its
process has exited; an idle/unloaded model's worker must actually terminate.
Pool exhaustion returns a clear error; do not spill into random ephemeral ports.
Memory budgets, priorities, in-flight requests and idle eviction still limit
actual model concurrency, normally far below 100 workers.

Locks coordinate cooperating Mica processes on the same machine, including
client and server state roots. Checking a free TCP port and starting a third-party
process cannot be perfectly atomic without socket activation; an unrelated
process may win that race. Startup must fail safely rather than terminate it.

The app pool caps **simultaneously running HTTP services**, not installed apps.
Native terminal/desktop apps normally need no HTTP port. Static web assets are
served directly under `/apps/APP_ID/`; no process or service slot is needed.
Apps with their own HTTP server are reverse-proxied under the same prefix;
root-relative links, API calls and streaming must retain app isolation.

The browser-facing address remains:

```text
http://SERVER:55000/apps/APP_ID/    server-hosted app
http://127.0.0.1:55001/apps/APP_ID/ device-hosted app
```

Private upstream port numbers are implementation details, not shareable URLs.
Legacy package `launch.port` values are hints, not an entitlement to a fixed
port; allocated state records the actual upstream. Native app packages without
an HTTP service need no port declaration. Static apps declare an entry file.

## Existing installations

New installations default to `55000`. For an existing server, explicitly migrate:

```sh
mica-server config set --port 55000 --restart
mica-server dashboard
```

Update clients pointing at the old URL. This intentionally restarts that
listener; ordinary app installation or launch does not. Old benchmark logs in
`docs/validation/` retain their actual historical ports as evidence, not defaults.

Multiple server/client instances must choose distinct management ports in the
55xxx range; all still share the machine's private service pools.
