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
pvs: []                   # optional when rpc_services is non-empty
rpc_services: []          # optional when pvs is non-empty
```

At least one `pvs` or `rpc_services` entry is required. The old `PVList`,
`PVBase`, and `RedisBase` prototype keys are rejected.

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
```

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
    defaults:
      window_ns: 1000000000
```

`endpoint` and fully qualified `service` are required and non-empty. `suffix`
and string-valued `defaults` are optional. The backend must expose gRPC server
reflection. See [`rpc-forwarding.md`](rpc-forwarding.md).

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
metadata/access changes, changed backends/services, alarm/catalog changes, and
settings requiring a restart. Credential values are never included. An omitted
schema version and explicit version 1 compare equally. Alias ordering alone is
not a change.

A replacement includes type/route/confirmation changes, alias-set changes (which
can reconnect clients), and affected PVs when a backend definition changes.
Metadata changes include metadata, alarm thresholds, transforms and initial
fallback definitions. They retain the runtime's subscription topology; changing
a transform can cancel pending commands. Access changes are listed separately.
External ACF file contents and environment variables are not inputs to this
file-to-file diff. RPC service differences are shown offline; reflected endpoint
names and availability still require staging-time discovery.

Numeric display/control limits must have `low <= high`; alarm thresholds must
be ordered `low_alarm <= low_warning <= high_warning <= high_alarm` among the
thresholds that are present. Hysteresis and minimum step must not be
negative. Transform coefficients must be finite, and scale must be nonzero with
a finite inverse. Legacy `metadata.control.min_step` remains accepted and retains
its precedence over `metadata.min_step` when both are supplied.
