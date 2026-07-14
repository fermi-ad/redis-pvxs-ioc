# Configuration Reference

The runtime reads one YAML file, `/etc/redis-pvxs-ioc/config.yaml` by default.
Use `--check-config` before deployment:

```sh
redis-pvxs-ioc --check-config /path/to/config.yaml
```

The command prints the resolved instance, namespace, Redis backends, configured
PVs, and RPC services. It does not connect to Redis or start a PVA server.

## Top-level structure

```yaml
server: {}
redis: {}                 # or redis_backends, exactly one form
alarms: {}                # optional
channelfinder: {}         # optional
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

## `pvs`

Every PV requires:

| Field | Values |
| --- | --- |
| `name` | Non-empty name; `server.namespace` is prepended when set |
| `type` | `bool`/`boolean`, `int8`, `uint8`, `int16`, `uint16`, `int32`, `uint32`, `int64`, `uint64`, `float32`/`float`, `float64`/`double`, or `string` |
| `shape` | `scalar` or `array` |
| `read` | Mapping with required non-empty `key` and optional `backend` |

Boolean and string values are scalar-only. Numeric types support scalar or
array shapes.

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

`confirm` requires `write`; `timeout_ms` defaults to `250`. Subscribed read and
confirm keys must be unique within each backend generation.

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

`initial` is optional and must match the declared type and shape. It primes the
served value until Redis supplies data. Boolean/string arrays are unsupported.

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
[`operations.md`](operations.md). Config validation currently enforces the
version/revision aliases; [issue #66](https://github.com/fermi-ad/redis-pvxs-ioc/issues/66)
tracks reserving the complete operational namespace.

## Complete examples

- [`../demo/config.yaml`](../demo/config.yaml): single Redis backend, metadata,
  write confirmation, alarms, transforms, arrays, and RPC forwarding.
- [`../demo/config.multi.yaml`](../demo/config.multi.yaml): multiple named Redis
  backends and explicit route selection.
- [`../demo/config.rpc.yaml`](../demo/config.rpc.yaml): reflection-based RPC-only
  process with no Redis-backed PV definitions.
