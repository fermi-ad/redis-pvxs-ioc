# Configuration Reference

The runtime reads one YAML file, `/etc/redis-pvxs-ioc/config.yaml` by default.
Use `--check-config` before deployment:

```sh
redis-pvxs-ioc --check-config /path/to/config.yaml
redis-pvxs-ioc --check-config /path/to/config.yaml --json
```

The command prints the resolved instance, namespace, Redis backends, configured
PVs, and RPC services. It does not connect to Redis or start a PVA server. JSON
mode returns `valid`, `schema_version`, `legacy_input`, configured counts and a
summary, or `valid: false` and an error. Invalid input exits with status 1.

Use `schema_version: 1` for new definitions. An omitted version accepts the
compatible v0.8 form and normalizes to version 1. Unknown versions, unknown keys,
duplicate keys (including properties/defaults), recursive YAML aliases, unsafe
narrowing, nonfinite numeric settings/initial values and inconsistent limits
are rejected. Validation permits up to 64 nesting levels and one million node
visits, including alias expansion. Old prototype `PVList`/`PVBase`/`RedisBase`
inputs remain unsupported.

## Top-level structure

```yaml
schema_version: 1
server: {}
access: {}                # optional; disabled by default
redis: {}                 # or redis_backends, exactly one form
alarms: {}                # optional
channelfinder: {}         # optional
discovery: {}             # optional; automatic RecCeiver registration enabled
limits: {}                # optional; bounded write queues, deadlines and payloads
pvs: []                   # optional with RPC services or selected gateway PVs
rpc_services: []          # optional with PVs or selected gateway PVs
pva_gateway: {}           # optional; explicitly selected upstream PVA channels
```

At least one `pvs`, `rpc_services`, `pva_gateway.pvs` entry or `pva_gateway.file` is required. The old `PVList`,
`PVBase`, and `RedisBase` prototype keys are rejected.

## Operation limits

The optional top-level `limits` mapping controls write admission, alarm delivery
and scalar/array payload validation. Its defaults apply to omitted-version configurations as well
as schema version 1. Changing limits requires a restart and is reported by the
offline difference command.

| Field | Default | Supported values |
| --- | --- | --- |
| `write_workers` | `4` | 1–64 |
| `queued_writes_per_pv` | `16` | 1–4096, shared by canonical name and aliases |
| `queued_write_bytes` | `67108864` | 1024–1073741824 bytes, including active reservations |
| `rpc_workers` | `4` | 1–64 workers, separate from write workers |
| `queued_rpc_per_method` | `16` | 1–4096 waiting calls per served RPC name, in addition to one active call |
| `queued_rpc_bytes` | `67108864` | 1024–1073741824 bytes, including active argument/bookkeeping reservations |
| `max_payload_bytes` | `33554432` | 1–1073741824 bytes per scalar/array payload |
| `operation_timeout_ms` | derived | 1–300000; omitted uses max(5000, confirmation timeout + 2000) |
| `alarm_queue_entries` | `1024` | 1–1000000 transitions; each reserves 1024 bytes |
| `alarm_state_bytes` | `67108864` | 1024–1073741824 bytes for the alarm queue and registered current states |

Oversized writes and full queues return explicit PVA errors. Oversized source
payloads preserve the last good value and set an invalid alarm. Existing control
limits remain advisory; this mapping does not enable value-range enforcement.

The alarm reservation must cover the queue plus 1024 bytes and twice the canonical
PV-name length per registered state. Aliases do not consume another state.
Configuration checks reject a budget that cannot hold the configured generation.
Preparing a replacement on the same publisher also charges any old registrations
still retained during cutover; insufficient headroom rejects the reload without
changing the live generation. Reservations measure bounded bookkeeping and
payload storage, not total process RSS.


## `server`

| Field | Required | Default | Meaning |
| --- | --- | --- | --- |
| `instance` | yes | — | Stable IOC instance name used by built-in PVs |
| `namespace` | no | empty | Prefix added to configured PV and RPC names |
| `interfaces` | no | PVXS environment defaults | Sequence of PVA listen interfaces |
| `tcp_port` | no | PVXS default | PVA TCP server port |
| `udp_port` | no | PVXS default | PVA UDP discovery port |
| `auto_beacon` | no | `true` | Enable PVXS automatic beacons |

Namespace, instance, interfaces, ports, and beacon configuration are immutable
after startup. Change them by restarting the process; ordinary PV/route/metadata
changes use hot reload.

## `access`

Native EPICS ACF access control is opt-in and startup-immutable. When enabled,
the policy, macros, defaults, endpoint assignments, and optional file watcher
are hot-reloadable. See [Access control](access-control.md) for the complete
schema, authorization behavior, and reload guarantees.

## `pva_gateway`

The optional gateway forwards selected PVA channels from explicit numeric IPv4
TCP endpoints. It supports GET_FIELD, GET, PUT, PUT_GET, PROCESS, ARRAY, monitors and RPC, including the
client's operation-specific field selection and `pvRequest`. No Redis-backed
PVs are required for a gateway-only IOC, but the usual Redis backend settings
remain required for the alarm publisher.

```yaml
server:
  instance: camera-gateway
  namespace: VIEW
redis:
  base_key: gateway
  host: redis
  port: 6379
access:
  enabled: true             # built-in DEFAULT READ policy when file is omitted
pva_gateway:
  pvs:
    - name: image
      upstream: "CAMERA:IMAGE"
      server: "192.0.2.10:5075"
      aliases: ["LEGACY:IMAGE"]
  file: /var/lib/xrm-proxy/gateway.yaml
  cache_file: /var/lib/redis-pvxs-ioc/gateway.last-good.yaml
  watch: {enabled: true, interval_ms: 1000, settle_ms: 250}
```

This exports `VIEW:image` and the exact alias `LEGACY:IMAGE` through the same
canonical source. Canonical names receive `server.namespace`; aliases are exact
full names, matching Redis PV aliases. Omit the namespace when a producer emits
fully qualified canonical names. Inline `pvs` and the optional manifest are
combined, with collisions checked across both sets, Redis PVs, aliases,
diagnostics and reflected RPCs.

Each source requires an explicit unicast `server: IPv4:TCP-port`; UDP search,
environment address lists and name servers are not used. Unspecified,
multicast, limited broadcast, `.255` addresses and local-interface broadcast
addresses are rejected. A server/upstream pair may appear once; use aliases for
multiple exported names. Direct addressing does not authenticate the server.
Upstream names cannot match this generation's local endpoints or any endpoint
still served by the preceding generation. Forwarding cycles through independent
gateways remain the operator's responsibility.

The gateway preserves the upstream PVA structure and operation requests.
Each monitor has its own upstream subscription. Upstream disconnects and errors
are reported to downstream clients; commands are never replayed after an
uncertain timeout or disconnect. Operations, channels, monitor queues and
retained payloads share process limits across the entire catalog. These limits
cover admitted forwarding work; PVXS transport buffers and decoding allocations
are additional memory, so `max_payload_bytes` is not an inbound wire-frame cap.
For ARRAY reads with count zero (all remaining elements), the gateway checks the
current upstream length before forwarding the original request. Concurrent
upstream growth can exceed that estimate; the returned payload is checked again
after receipt. Timeout recovery closes the affected downstream channel so the
client can reconnect with fresh operation setups; other subscriptions on that
channel also disconnect.
Device authorization sees the gateway client's
identity; local ACF evaluates the downstream identity independently.

Access control is off by default, permitting forwarded writes and RPCs. Enabling
`access` without a file installs a read-only `DEFAULT` policy, covering gateways,
Redis PVs, RPCs and admin commands. A supplied ACF keeps its existing policy
semantics. Main YAML can assign an inline source with `access` or select a
particular exported name using `access_overrides`:

```yaml
access: {enabled: true, file: /etc/redis-pvxs-ioc/access.acf}
pva_gateway:
  file: /var/lib/xrm-proxy/gateway.yaml
  access_overrides:
    "LEGACY:COMMAND": {asg: OPERATORS, asl: 0}
```

The custom ACF must define every assigned ASG. Override keys match exact exported
names; a namespace-relative canonical name is also accepted. Overrides can name
exports not yet discovered. Each alias has its own local authorization while
sharing the canonical upstream mapping. Manifests cannot select ASGs or change
policy.

The manifest is a bounded YAML or JSON document with exactly this schema:

```yaml
schema_version: 1
revision: inventory-2026-10-29-001
pvs:
  - name: "BASE:DEVICE:STATUS"
    upstream: "DEVICE:STATUS"
    server: "192.0.2.10:5075"
    aliases: ["LEGACY:STATUS"]
```

Only `schema_version`, `revision` and `pvs` are allowed at the root; entries permit
only `name`, `upstream`, `server` and `aliases`. The nonempty revision is at most
256 bytes, files at most 16 MiB, names at most 1024 bytes, and at most 100000 combined canonical and alias names are accepted.
An empty `pvs` list intentionally withdraws manifest exports. Paths resolve
relative to the main YAML. A producer should publish with atomic rename.

The optional watcher polls by path, waits for stable bytes for `settle_ms`, and
stages a complete internal configuration transaction. It requires no admin PVA
WRITE permission and preserves the active ACF bytes; edited policies take effect
through an explicit full reload, policy reload, or an enabled access watcher. Unchanged files do not increment the configuration generation.
Invalid, missing or partially written candidates preserve the active generation;
unchanged invalid content is not retried continuously. Changes to gateway
mappings and aliases use the same all-or-nothing namespace publication as normal
reloads. The `access.enabled` startup setting remains immutable.

With no manifest at first startup, the IOC serves its inline/baseline endpoints
and reports `waiting`. When `cache_file` is supplied, the IOC atomically persists
the last accepted manifest there and revalidates it against the current main
configuration on a later startup. If the producer file is unavailable or
invalid, a valid cache starts in `stale` state. Use a separate writable state
volume for the cache; the source manifest directory may remain read-only.
Without `cache_file`, retention lasts for the current process. A cache write
failure is reported without undoing an accepted generation.

`--check-config` validates the current manifest, using a valid configured cache
when the manifest is missing, unreadable or invalid. `--diff-config OLD NEW` resolves each configuration's
current manifest and reports mapping changes plus `gateway_manifest_changed`
for manifest reference, watcher, cache, revision or content changes. Comparing
two YAML files that reference the same mutable manifest compares its current
contents; keep versioned manifest files to compare historical inventories.

`SYS:<instance>:gateway:status` exposes state, active and attempted manifest
revision/fingerprint, errors, cache state, canonical/alias counts and generation.
Its `sources` table reports canonical names, upstream endpoints, connectivity,
channels, monitors and operation counters. Fingerprints are diagnostic content
identities, not security tokens. Gateway catalog entries are published while
upstreams are unavailable. The Redis `ready` diagnostic continues to describe
Redis sources; gateway health is reported separately.

## Redis backends

Use `redis` for the legacy single-backend form:

```yaml
redis:
  base_key: demo
  host: redis
  port: 6379
  user: optional-user
  password: optional-password
  workers: 1
  readers: 1
  reader_probe_ms: 1000
```

`user_file` and `password_file` are alternatives to inline `user` and `password`.
Each inline/file pair is mutually exclusive. Relative paths resolve beside the
YAML file. Files must be nonempty regular files (symlinks to regular files are
allowed), readable by the runtime UID/GID, and contain at most 16 KiB of text.
One trailing LF or CRLF is removed; other whitespace is preserved. Embedded
newlines and NUL bytes are rejected. Errors and JSON check/diff output do not
include credential contents.

Files are read during configuration parsing, including offline checks/differences.
Replace a secret file atomically, then reload to rotate it. The active configuration
keeps its loaded credentials until reload succeeds. Missing, unreadable or invalid
files reject the candidate. An offline file-to-file diff reads both files' current
secret inputs; it does not recover an older secret from a reused path. See
[Container runtime and secret files](container-runtime.md).

Use `redis_backends` for one or more named backends:

```yaml
redis_backends:
  values:
    base_key: values
    host: values-redis
    port: 6379
  system:
    base_key: system
    host: system-redis
    port: 6379
```

Each backend requires `base_key`, `host`, and `port`. `user` and `password`
default to empty; `workers` and `readers` default to `1`.
Worker/reader counts must be 1–256 and the Redis port must be 1–65535. Server
ports may still be zero to request an ephemeral port.

The IOC deliberately enables standalone continuity inspection with
`reader_probe_ms: 1000` per backend. Zero disables it; enabled values must be
100–60000 ms. The adapter itself defaults to inspection disabled. Probes share
the adapter's scheduler and connections: at most 16 due keys per batch, active
keys skip unnecessary probes, idle keys back off to at most eight times the
interval, and denied/unsupported inspection backs off for 60 seconds. These are
minimum intervals, not hard detection deadlines. `XINFO STREAM FULL COUNT 1`
transfers a retained payload, so large idle images and many keys require an
explicit traffic/capacity check; increase the interval or disable it when
appropriate. A backend policy change recreates that backend and its affected
runtimes on reload. See [source health](source-health.md) for permissions,
readiness semantics and qualification recipes.

Routes may omit `backend` when exactly one backend exists. With multiple
backends, every read/write/confirm route and `alarms.backend` must name a defined
alias. In the legacy `redis` form, route alias `redis` resolves to the single
backend for compatibility.

## `alarms`

```yaml
alarms:
  backend: system
  stream: acorn:alarms
```

`stream` defaults to `acorn:alarms`. `backend` defaults to the only configured
backend, but is required when multiple backends exist. Scalar threshold state
changes are written to this Redis stream.

## `channelfinder`

| Field | Required | Default |
| --- | --- | --- |
| `url` | no | empty; publishing requires it |
| `owner` | no | `redis-pvxs-ioc` |
| `tags` | no | empty sequence |
| `properties` | no | empty mapping |

Tags and property names must not be empty. See
[`channelfinder-sync.md`](channelfinder-sync.md).

## `discovery`

RecCeiver registration is enabled by default. It uses the core service's active
PV catalog and needs no conventional IOC or ChannelFinder credentials.

| Field | Default | Supported values |
| --- | --- | --- |
| `enabled` | `true` | Boolean |
| `bind_address` | `0.0.0.0` | IPv4 listen address |
| `udp_port` | `5049` | 0–65535; zero allocates a local test port |
| `timeout_ms` | `20000` | 1–300000; total connect/greeting/upload deadline |
| `max_holdoff_ms` | `10000` | 0–60000; randomized receiver connection delay |
| `max_records` | `100000` | 1–1000000; includes aliases and admin/RPC endpoints |
| `max_bytes` | `16777216` | 1024–1073741824; encoded catalog byte bound |

These settings require a process restart to change. Successful PV/alias/metadata
reloads automatically replace the catalog; failed staging keeps the previous
session/catalog active. Staging validates protocol field sizes and count/byte
bounds. The worker retains at most one active upload and one latest replacement;
socket buffers and temporary encoding storage add to the encoded byte count.
See [Discovery](reccaster.md) for networking and status semantics.

## `pvs`

Every PV requires:

| Field | Values |
| --- | --- |
| `name` | Non-empty name; `server.namespace` is prepended when set |
| `aliases` | Optional non-empty sequence of exact, fully-qualified PVA names; `server.namespace` is not prepended |
| `type` | `bool`/`boolean`, `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float32`/`float`, `float64`/`double`, or `string` |
| `shape` | `scalar` or `array` |
| `read` | Mapping with required non-empty `key` and optional `backend` |

Boolean and string values are scalar-only. Numeric types support scalar or
array shapes.

For a read-only image, use `kind: ntndarray` instead of `type` and `shape`:

```yaml
- name: GE1350:Image
  aliases: [SPIKE:CAM2:Pva1:Image]
  kind: ntndarray
  read: {backend: default, key: ge1350}
  max_frame_bytes: 33554432
```

NTNDArray configuration rejects scalar-only fields and write routes. See
[Redis-backed NTNDArray](ntndarray.md) for its versioned Redis Stream contract,
acquisition timestamp rules, validation, and alarm behavior.

Aliases are additional names for the same PVXS `SharedPV`; they do not create
another Redis reader, writer, confirmation route, alarm state, or configured
runtime. This permits cross-namespace names while retaining a concise canonical
name:

```yaml
- name: magnet:current
  aliases:
    - FACILITY:AREA_GROUP_MAGNET01:I
```

With `server.namespace: DEMO`, this serves both `DEMO:magnet:current` and the
exact alias `FACILITY:AREA_GROUP_MAGNET01:I`.

Canonical names, aliases, diagnostic PVs and reflected RPC methods share one
reserved namespace. Collisions reject startup or the staged reload before live
endpoints or access policy change, including when discovery is disabled.
`--check-config` checks the statically known names offline; RPC names are also
checked after reflection during startup or reload. Installed diagnostics must
match the same reserved name set.

Adding, renaming, or removing aliases during a successful reload retains the
logical runtime and all Redis routes. Because the PVXS static registry closes a
shared PV when a registered name is removed, clients attached to any name for
that logical PV may briefly reconnect while the same `SharedPV` is reopened and
its desired name set is registered.

Optional route fields:

```yaml
write:
  backend: values
  key: magnet:setpoint
confirm:
  backend: values
  key: magnet:readback
  timeout_ms: 250
```

`confirm` requires `write`; `timeout_ms` defaults to `250`. A confirmed put
completes only after the configured confirmation subscription sees the raw
value written to Redis at a stream position newer than the snapshot taken
before dispatch. Observations from a separate read route do not confirm a put;
confirmation observations do not change displayed readback. This establishes
observed readback, not causal acknowledgement by the command consumer. Repeated
commands each require a new matching observation. The wait is bounded by
`timeout_ms`, and reload deactivation fences puts from an older generation.
The supported confirmation wait is 1–300000 ms; zero and negative waits are
rejected. Control limits remain advisory metadata by default.

Malformed scalar or array payloads retain the last good value and set an INVALID
alarm. A missing source uses the configured `initial` fallback with an INVALID
alarm and zero source timestamp until valid data arrives. Empty numeric arrays
are valid. Legacy payload byte order and source timestamp interpretation are
unchanged; the exact Redis stream cursor is tracked separately for ordering.

### Source health policy

Scalar, array and NTNDArray PVs accept an optional `source_health` mapping:

```yaml
source_health:
  required: true
  stale_after_ms: 0
```

`required` defaults to true and controls inclusion in aggregate Redis source
readiness. It applies to both read and distinct confirmation sources. Setting it
false retains source diagnostics and health alarms. `stale_after_ms` defaults to
zero (cadence unspecified); otherwise it is 1–86400000 ms. Freshness measures
time since the last valid snapshot/update was received on the IOC's monotonic
clock. It does not estimate acquisition age from the legacy source timestamp.
An idle source with valid data and no cadence stays fresh; a source that has
never supplied valid data remains unready. A freshness failure retains the
last-good value and source timestamp with an INVALID alarm. Health diagnostics
and those alarms refresh once per second and after successful configuration
publication. Policy edits retain the runtime, reader, cursor and counters.

Source epoch changes cancel pending confirmations without replaying commands;
new matching observations must belong to the command's source epoch. A valid
decoded sample in the current epoch restores readiness after stream replacement.

### Collision and route validation

Configuration loading rejects:

- any duplicate served name across canonical names and aliases, including an
  alias equal to its own or another PV's canonical name;
- names that expand to any built-in operational PV name;
- more than one subscription to the same `(backend, key)` through a `read` or a
  distinct `confirm` route; and
- route or alarm backend aliases that do not exist.

The same Redis key is allowed on different backends because the backend alias is
part of the subscription identity. `--check-config` performs these checks
offline and prints the resolved PV names, aliases, and routes before deployment.

### Metadata

```yaml
metadata:
  description: Magnet current
  units: A
  precision: 2
  form: engineering
  display: {low: 0, high: 15}
  control: {low: 0, high: 15, min_step: 0.1}
```

Display forms are `default`, `string`, `binary`, `decimal`, `hex`,
`exponential`, and `engineering`. `min_step` may be set directly under
`metadata` or as `metadata.control.min_step`. Numeric display/control limits
require a numeric PV type.

### Threshold alarms

```yaml
alarm:
  low_alarm: 0
  low_warning: 1
  high_warning: 7
  high_alarm: 9
  hysteresis: 0.1
```

Threshold alarms apply only to numeric scalar PVs. `hysteresis` defaults to
`0.0`. Array threshold alarms are unsupported.

### Linear transforms

```yaml
transform:
  kind: linear
  scale: 0.1
  offset: 0.0
```

Transforms apply only to `float32`/`float64` scalars or arrays. `kind` defaults
to `linear`, `scale` defaults to `1.0` and must not be zero, and `offset`
defaults to `0.0`. Reads map Redis values into served units; writes apply the
inverse mapping.

### Initial values

`initial` is optional and must match the declared type and shape. At startup the
runtime first attempts to load the latest Redis value; `initial` is the fallback
when no snapshot is available. Live subscription updates then take over, and
their Redis timestamps are carried into the PVA `timeStamp`. Boolean/string
arrays are unsupported.

## `rpc_services`

```yaml
rpc_services:
  - endpoint: query-server:50051
    service: example.query.v1.Query
    suffix: _RPC
    optional: false
    discovery_timeout_ms: 3000
    timeout_ms: 10000
    retry_interval_ms: 5000
    defaults:
      window_ns: 1000000000
    method_defaults:
      Average:
        window_ns: 2000000000
```

`endpoint` and fully qualified `service` are required and non-empty. The backend
must expose gRPC server reflection. Services are required unless `optional: true`
is explicit: unavailable required services reject startup or a changed service
on reload. Unchanged discovered services keep their endpoints during an outage.

`discovery_timeout_ms` and `timeout_ms` accept 1–300000 ms; the latter is the total
call budget, including queue time. An unavailable optional service retries
reflection in the background after `retry_interval_ms` (100–300000 ms). Invalid
schemas, defaults or name collisions require correction and a configuration
reload. Optional does not suppress validation errors.

Shared `defaults` must match at least one method and apply to methods containing
that field. `method_defaults` uses exact protobuf method names and overrides the
shared defaults. Per-call arguments then override defaults by canonical field
path. Unknown or ambiguous fields and invalid values are errors. Offline checks
validate the configuration shape; reflected-schema validation needs the backend.
See [`rpc-forwarding.md`](rpc-forwarding.md) for supported shapes and limits.

## Reserved names

Do not configure any PV with the built-in names listed in
[`operations.md`](operations.md). Configuration validation reserves the full
operational namespace.

## Complete examples

- [`../demo/config.yaml`](../demo/config.yaml): single Redis backend, metadata,
  write confirmation, alarms, transforms, arrays, and RPC forwarding.
- [`../demo/config.multi.yaml`](../demo/config.multi.yaml): multiple named Redis
  backends and explicit route selection.
- [`../demo/config.rpc.yaml`](../demo/config.rpc.yaml): reflection-based RPC-only
  process with no Redis-backed PV definitions.
- [`../demo/config.access.yaml`](../demo/config.access.yaml): explicitly enabled
  ACF policy, endpoint assignments, and file monitoring.

## Offline configuration differences

```sh
redis-pvxs-ioc --diff-config old.yaml new.yaml
redis-pvxs-ioc --diff-config old.yaml new.yaml --json
```

The command compares validated, normalized definitions without Redis, RPC
reflection or PVA startup. It reports additions, removals, replacements,
metadata/access/alias changes, changed backends/services, alarm/catalog changes, and
settings requiring a restart. Credential values are never included. An omitted
schema version and explicit version 1 compare equally. Alias ordering alone is
not a change.

A replacement includes type/route/confirmation changes and affected PVs when a
backend definition changes. Alias-set changes are reported as `alias_changes`;
they retain the canonical runtime and disconnect only removed alias channels.
Metadata changes include metadata, alarm thresholds, transforms, source-health policy and initial
fallback definitions. They retain the runtime's subscription topology; changing
a transform can cancel pending commands. Access and alias changes are listed separately
and may overlap retained metadata changes.
External ACF file contents and environment variables are not inputs to this
file-to-file diff. RPC service differences are shown offline; reflected endpoint
names and availability still require staging-time discovery.

Numeric display/control limits must have `low <= high`; alarm thresholds must
be ordered `low_alarm <= low_warning <= high_warning <= high_alarm` among the
thresholds that are present. Hysteresis and minimum step must not be
negative. Transform coefficients must be finite, and scale must be nonzero with
a finite inverse. Legacy `metadata.control.min_step` remains accepted and retains
its precedence over `metadata.min_step` when both are supplied.
