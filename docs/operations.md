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
| `SYS:<instance>:stats:pvCount` | int64/read | Configured Redis-backed and RPC PV count |
| `SYS:<instance>:stats:operations` | structure/read | Write queue counts, reservations, peaks and configured limits |
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

Parsing or schema errors report `reload failed`. A replacement that parses but
cannot be safely applied reports `reload rejected`. In both cases the generation
number remains unchanged. Rejection during staging preserves the active
generation; complete rollback for failures after endpoint cutover begins remains
tracked by [#4](https://github.com/fermi-ad/redis-pvxs-ioc/issues/4).

Unchanged Redis backend definitions retain their existing adapters. Only PVs
whose reader topology or referenced backend changes are replaced. An unrelated
backend edit or alarm-stream edit therefore preserves cached readback, owned
subscriptions and pending confirmations on unchanged runtimes. Staging a new
runtime does not globally defer the existing readers.

Unchanged RPC service definitions retain their reflected methods, bridge and PVs,
including when that backend is temporarily unavailable. Access assignments still
follow the new configuration. Changing endpoint, service, suffix or defaults
rebuilds that service; restarting the IOC refreshes all reflected schemas.

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
wait for Redis or confirmation. Alarm socket connection and I/O waits are bounded
to 500 ms; asynchronous alarm reconciliation remains a separate #4 requirement.

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
