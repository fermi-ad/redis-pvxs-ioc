# Operations and Diagnostics

## Built-in PVs

The runtime always installs these PVs using `server.instance`. They are not
prefixed by `server.namespace`.

| PV | Type/access | Meaning |
| --- | --- | --- |
| `<instance>:version` | string/read | `redis-pvxs-ioc v<version>` |
| `<instance>:revision` | string/read | `redis-pvxs-ioc <git-revision>` |
| `SYS:<instance>:version` | string/read | Version alias |
| `SYS:<instance>:revision` | string/read | Revision alias |
| `SYS:<instance>:config:reload` | int64/write | Write any value to request reload |
| `SYS:<instance>:config:generation` | int64/read | Active generation, starting at `1` |
| `SYS:<instance>:config:lastStatus` | string/read | Active/rejected/failed status |
| `SYS:<instance>:config:lastError` | string/read | Last reload error; empty after success |
| `SYS:<instance>:config:lastDiff` | string/read | JSON differences for the current attempt; `{}` if parsing failed |
| `SYS:<instance>:config:reloadStatus` | structure/read | Attempt, phase, duration, change counts and per-backend outcomes |
| `SYS:<instance>:stats:pvCount` | int64/read | Configured Redis-backed and RPC PV count |
| `SYS:<instance>:stats:operations` | structure/read | Write queue counts, reservations, peaks and configured limits |
| `SYS:<instance>:rpc:status` | structure/read | RPC discovery/retry state, per-service call outcomes and RPC queue reservations |
| `SYS:<instance>:alarms:status` | structure/read | Alarm delivery state, outcomes, reconciliation and reservations |
| `SYS:<instance>:backend:health` | string/read | `<connected>/<total> connected`, with disconnected aliases when applicable |
| `SYS:<instance>:access:reload` | int64/write | Request an ACF-only reload |
| `SYS:<instance>:access:enabled` | bool/read | Startup access-control state |
| `SYS:<instance>:access:generation` | int64/read | Active ACF generation; `0` disabled, `1` after enabled startup |
| `SYS:<instance>:access:lastStatus` | string/read | Last policy/watcher status |
| `SYS:<instance>:access:lastError` | string/read | Last policy/watcher error |
| `SYS:<instance>:access:policyFingerprint` | string/read | Fingerprint of expanded active policy bytes |
| `SYS:<instance>:access:watchStatus` | string/read | Watcher state and path |
| `SYS:<instance>:access:activeClients` | int64/read | Connected access-controlled channels |
| `SYS:<instance>:access:deniedReads` | int64/read | Process-lifetime denied-read count |
| `SYS:<instance>:access:deniedWrites` | int64/read | Process-lifetime denied-write count |
| `SYS:<instance>:access:rightsChanges` | int64/read | Process-lifetime changed-rights count |
| `SYS:<instance>:access:operations` | structure/read | Authorized PUT/RPC counts, completion outcomes and live denial-rate state |

## Reload

Request a reload with either mechanism:

```sh
kill -HUP "$(pgrep -f redis-pvxs-ioc)"
pvxput SYS:<instance>:config:reload 1
```

The runtime parses and validates the entire replacement file, stages the next
generation, and only then applies it. A successful reload increments
`config:generation`, reports `generation <n> active`, and clears
`config:lastError`.

`config:reloadStatus` describes the latest startup, whole-file reload or optional
RPC recovery publication (`kind=rpc-recovery`) attempt.
Its monotonically increasing `attempt` is separate from `activeGeneration` and
`candidateGeneration`: rejected attempts do not advance the active generation.
It reports the schema version, elapsed `durationMs` at each published phase, and
`running`, `committed`, `committed-with-error`, `rejected` or `failed` state. The
terminal duration includes parsing, preparation, cutover and synchronous refresh.
Asynchronous alarm/discovery convergence is reported by their own diagnostics.
Startup failures before PVA is available remain visible in the configuration log.

Counts describe the parsed candidate's canonical Redis PVs: `added`, `removed`,
`recreated` and `retained`; `metadataChanged` counts updates within retained
runtimes. `aliasesChanged` and `accessChanged` are independent difference counts
(access includes a policy/defaults/watcher entry). RPC changes remain in
`config:lastDiff` and the `rpc` preparation phase. Counts on a rejected attempt
describe proposed changes, not applied changes. `diffKnown=false` means parsing
failed: counts and backend rows are cleared, as is `config:lastDiff`, so an older
attempt cannot be mistaken for the failed candidate.

The aligned `backends.name/action/preconnect/cutover` arrays record Redis backend
preparation. Actions are `retained`, `created` or `removed`; preconnect is
`connected`, `disconnected`, `failed`, `not-tested` or `not-applicable` at the
time of preparation. A disconnected backend can still commit with fallback values;
this snapshot is not a continuing health or readiness claim. Cutover is `active`
or `removed` after commit, and `previous-preserved` or `not-activated` after
rejection. Only attempted backends are included if construction fails early.
No endpoints, credentials or values are embedded in these rows. A structured
configuration log records the same terminal result, duration and counts.

Parsing or schema errors report `reload failed`. A replacement that parses but
cannot be safely applied reports `reload rejected`. In both cases the generation
number remains unchanged. Preparation and final-gate rejection preserve the
active values, channels, pending confirmations, access policy and discovery
catalog. The complete endpoint set is prepared before policy activation, then
published as one snapshot. No endpoint-by-endpoint installation occurs after
policy activation. Metadata refresh uses the latest source sample, including
updates received during preparation.

Removing an alias disconnects that alias's channels while retaining canonical
and unchanged-alias monitors. Once the complete generation is published, a later
notification/refresh failure is reported as `generation active; refresh failed`,
with the committed generation retained; it is not reported as a rollback.

Unchanged Redis backend definitions retain their existing adapters. Only PVs
whose reader topology or referenced backend changes are replaced. An unrelated
backend edit or alarm-stream edit therefore preserves cached readback, owned
subscriptions and pending confirmations on unchanged runtimes. Staging a new
runtime does not globally defer the existing readers.

Unchanged RPC service definitions retain their reflected methods, bridge and PVs,
including when that backend is temporarily unavailable. Access assignments still
follow the new configuration. Changing endpoint, service, suffix or defaults
rebuilds that service; restarting the IOC refreshes all reflected schemas. Optional
service recovery publishes a new configuration/catalog generation using the
current policy, without rereading the ACF. RPC retry and call diagnostics are
documented in [RPC forwarding](rpc-forwarding.md).

Namespace and bind settings cannot change through hot reload. Restart the process
to change `server.instance`, `server.namespace`, interfaces, ports, or beacon
configuration.

When access control is enabled, whole-config reload also rereads and activates
the ACF. `SYS:<instance>:access:reload` reloads only that policy, and an optional
settled file watcher can do the same automatically. Access enablement cannot
change through reload. See [Access control](access-control.md).

## Write operations

Write operations run on a bounded worker pool. Canonical PVs and their aliases
share one serial queue. The default is one active write plus at most 16 queued
writes per canonical PV, a 64 MiB aggregate payload reservation and a 32 MiB
payload limit. Reservations include active operations and charge at least 256
bytes per operation. They measure payload/bookkeeping reservations, not total
process RSS. `stats:operations` reports accepted and finished executor tasks,
overloads, deadline expirations, cancellations, current queue/running counts and
reservation peaks; a finished task is not necessarily a successful backend write.

Cancellation before dispatch prevents the Redis write. Rights are checked again
at dispatch. A cancellation or deadline after dispatch cannot undo a command
Redis may have accepted. The IOC does not replay it, and the next write for that
PV waits for the underlying worker to finish. Readback confirmation still uses
the configured wait (250 ms by default); value matching establishes an observed
readback rather than command causality.

The total operation deadline includes queue time. If omitted, it is the larger
of five seconds and the configured confirmation wait plus two seconds. See
[`limits`](configuration.md#operation-limits) for overrides. PVA callbacks do not
wait for Redis or confirmation.

## Alarm delivery

A dedicated worker delivers alarms. Runtime callbacks update fixed-size current
state and a bounded transition queue without waiting for Redis. Healthy delivery
preserves queued transitions in order. After overload, rejection or a transport
failure, pending history is coalesced and the worker reconciles each active PV's
current alarm state, including clears, without requiring another source update.
A one-second heartbeat detects a disconnected idle connection; retries wait one
second, with 500 ms socket connection and I/O timeouts.

This is current-state recovery, not durable delivery of every transition. An
interrupted reply may mean Redis accepted an earlier record; reconciliation can
therefore repeat a state. The stream keeps its existing fields and alarm/clear
representation. Healthy events retain their observation time; recovery snapshots
use the reconciliation time (Unix seconds). Messages are capped at 512 bytes,
with UTF-8 boundary truncation and a trailing `...` when necessary.

Preparation reserves state without publishing. Registrations become active only
at commit, and retired runtimes cannot enqueue new alarms. When the alarm backend
or stream changes, the old worker drains its in-flight call and stops before the
new publisher is enabled. Unchanged runtimes retain their readback and subscriptions.

`alarms:status` is updated once per second and on successful reload, under the
admin-read access assignment. It reports `staged`, `idle`, `disconnected`,
`recovering`, `ready` or `stopped`; accepted deliveries (`sent`), server rejections
and transport failures are counted separately. `reconciled` counts accepted
current-state snapshots. `coalescedUpdates`, `discardedTransitions` (queued
history discarded) and `messagesTruncated` expose lossy recovery or bounds.
`registrations` includes prepared/retained owners; `active` counts canonical
current owners and `pending` counts states needing delivery. Queue depth, reserved
bytes, peaks and configured limits are reported separately. Counters belong to
the publisher and reset when its backend or stream is replaced.

## Container verification

The image includes PVXS clients:

```sh
IOC=redis-pvxs-ioc-demo
PV_ENV='EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1'
PVX=/opt/redis-pvxs-ioc/bin/pvxs

for pv in \
  demo:version \
  demo:revision \
  SYS:demo:config:generation \
  SYS:demo:config:lastStatus \
  SYS:demo:config:lastError \
  SYS:demo:stats:pvCount \
  SYS:demo:backend:health
do
  docker exec "$IOC" sh -lc "$PV_ENV $PVX/pvxget $pv"
done
```

Run `../scripts/smoke-test.sh` from the repository root for end-to-end value,
write, alarm, successful reload, rejected reload, and diagnostic PV validation.

## Network troubleshooting

The default Compose stack uses a private bridge, so validate it inside the
container. A real controls deployment needs a routable `ipvlan`, `macvlan`, or
host-network identity. See [`pva-networking.md`](pva-networking.md).


The `SYS:<instance>:config:lastDiff` read-only PV contains the JSON difference
between the active configuration and the last successfully parsed reload
request, including requests later rejected during staging. It uses the same
categories as `--diff-config` and does not include credential values. Existing
config status/error/generation PVs remain available.
