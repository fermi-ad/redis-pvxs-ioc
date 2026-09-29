# PVA RPC to gRPC forwarding

The IOC bridges PVA RPC to reflected unary gRPC methods. It has no compiled-in
application schema: the backend must enable gRPC server reflection. Reflection
provides the protobuf descriptors used to validate requests and construct PVA
replies. Streaming methods and recursive or repeated-message reply shapes are
unsupported.

## Configuration and availability

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
      digitizer: MTCA1-1
      length_ns: 1000000000
    method_defaults:
      Average:
        length_ns: 2000000000
```

Each method becomes `<server.namespace>:<UPPER_SNAKE(MethodName)><suffix>`:
`Average` becomes `AVERAGE`, and `OnEventTime` becomes `ON_EVENT_TIME`. All names
share the canonical/alias/diagnostic namespace and must be unique.

Services are required by default. One reflection attempt has a finite
`discovery_timeout_ms` budget (3000 ms by default). An unavailable required service
rejects startup or a changed service on reload, preserving the active generation.
Unchanged, already discovered services retain their endpoints during outages;
restarting the IOC refreshes all schemas. `optional: true` explicitly permits a
service to be absent at startup. Both deadlines accept 1–300000 ms; the optional
retry interval accepts 100–300000 ms.

Unavailable optional services appear in `SYS:<instance>:rpc:status` and retry
reflection in the background. Only one retry runs at a time, in round-robin order.
Each attempt has the same discovery deadline. The complete service must pass
schema, defaults and endpoint-collision checks before its endpoints and discovery
catalog publish in one new generation. Existing Redis runtimes, subscriptions,
pending writes, RPC endpoints and the active ACF are retained. Background recovery
does not reread the ACF file. Results from retired generations are discarded;
shutdown cancels outstanding reflection.

Optional permits availability failures only. Invalid schemas, defaults or name
collisions still reject startup/reload. If first discovered by a background
retry, the service becomes `invalid`, publishes no methods, and requires a
correction followed by configuration reload. Offline checks validate configuration
shape; validation against reflected methods requires the backend.

RPC services do not use Redis, although the schema still requires `redis` or
`redis_backends`. See [the RPC-only example](../demo/config.rpc.yaml).

## Arguments and defaults

Both flat PVA structures and NTURI `query` structures are accepted, including
nested structures. Fields resolve by exact top-level name, explicit dotted path,
or a unique leaf name. YAML defaults and structured PVA requests support dotted
paths. This version of the `pvxcall` CLI accepts simple argument names, so use
unique leaves there:

```sh
pvxcall BI:BPM:AVERAGE_RPC digitizer=MTCA1-1 length_ns=1000000000
```

Each shared `defaults` key must match at least one method and applies only to
methods containing that field. `method_defaults` uses exact protobuf method names
and overrides the shared values. Per-call arguments override both by canonical
field path. Two caller names resolving to the same field are rejected, as are
unknown or ambiguous fields, unknown methods, conflicting oneof fields, arrays,
unions and message-valued scalar arguments. These errors never reach gRPC.

Integer parsing consumes the full value and checks the protobuf type's range;
hexadecimal `0xFF` is accepted. Negative unsigned values and non-finite or
out-of-range floating-point values are rejected. Booleans accept `true`, `false`,
`yes`, `no`, `1` and `0` (case-insensitive words). Enums accept declared names or
declared numeric values.

## Calls, cancellation and limits

Calls run on a dedicated bounded worker pool, separate from Redis writes. Each
served method has one active call and at most 16 waiting calls by default. Defaults
are four RPC workers, 64 MiB aggregate argument/bookkeeping reservations (including
active calls), and 32 MiB request/reply payloads. Configure `limits.rpc_workers`,
`queued_rpc_per_method`, `queued_rpc_bytes` and `max_payload_bytes`; these limits
require restart. Each queued task reserves 1024 bytes plus 128 bytes per argument
and its name/value bytes. Shared defaults/descriptors and active protobuf/reply
buffers are additional bounded allocations; reservations are not process RSS.

`timeout_ms` (10000 ms by default) bounds the whole call, including queue time.
Full queues, byte-budget exhaustion and oversize payloads return explicit errors.
Authorization and endpoint ownership are rechecked immediately before dispatch.
Cancellation, endpoint retirement, access revocation, deadline expiry and shutdown
cancel active gRPC contexts and remove waiting work. An active call retains its
serial lane until its worker has finished cancellation, preventing overtaking.
Cancellation or a deadline cannot undo a command already accepted by a backend.
The IOC disables gRPC retries and never replays a command after an ambiguous error.

Reflection is limited to 4 MiB, 256 descriptor files and 256 methods per service.
Schema traversal permits at most 32 nesting levels and 4096 reply fields. Request
structures permit at most 4096 visited fields and 1024 resolved scalar arguments.
Reply limits check both protobuf wire bytes and decoded PVA payload storage,
including numeric array expansion. Input/output transport limits and decoded
payload limits are separate checks.

## Replies and diagnostics

Replies are plain PVA structures mirroring the protobuf message: scalar fields,
arrays of repeated scalars, and nested structures for singular messages. Repeated
message fields and recursive/deeply nested reply schemas are rejected at discovery.
Backend errors become PVA operation errors. This bridge does not assign a
normative type such as NTScalar or NTTable to an arbitrary protobuf reply.

`SYS:<instance>:rpc:status` reports:

- `state`: `disabled`, `ready` or `degraded`, describing discovery availability.
- `services`: aligned name, endpoint, optional, state, lastError, methods, attempts
  and retryInMs arrays. Service states are `ready`, `unavailable`, `discovering`
  and `invalid`. Error text is limited to 1024 bytes.
- Per-service `dispatched`, `succeeded`, `failed`, `invalid` and `unauthorized`
  counters. `failed` includes backend/transport errors and cancelled dispatched
  calls. `invalid` counts argument/marshalling rejection before dispatch.
  Rights denied at initial admission remain in the access-control counters.
- `queue`: accepted/finished executor tasks, overloads, deadline expirations,
  cancellations, queued/running counts, byte reservations/peak and configured
  limits. A finished executor task is not necessarily a successful backend call.

`ready` means schemas and endpoints are installed; it does not assert continuing
backend connectivity. A backend outage leaves endpoints installed and appears in
call failures. Counters follow retained service instances across unrelated reloads;
replacing a service resets that service's counters.

## Build and test

Enable `REDIS_PVXS_IOC_ENABLE_GRPC` and install gRPC C++, protobuf, `protoc`, and
the gRPC C++ plugin as described in [Building from source](building-from-source.md).
Only the standard reflection proto is compiled into the IOC. The test-only
`rpc_fixture` enables reflection and provides strict-argument, cancellation,
backend-outcome and unsupported-schema cases. Full-feature tests use isolated
Redis/PVA fixtures and require no external application backend.
