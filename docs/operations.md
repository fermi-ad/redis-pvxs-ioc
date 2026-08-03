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
number remains unchanged and the active generation continues serving.

Namespace and bind settings cannot change through hot reload. Restart the process
to change `server.instance`, `server.namespace`, interfaces, ports, or beacon
configuration.

When access control is enabled, whole-config reload also rereads and activates
the ACF. `SYS:<instance>:access:reload` reloads only that policy, and an optional
settled file watcher can do the same automatically. Access enablement cannot
change through reload. See [Access control](access-control.md).

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
