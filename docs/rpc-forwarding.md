# PVA RPC -> gRPC forwarding

A PVA client runs `pvxcall <PVName> <args>`. The IOC receives the RPC on a
`pvxs::server::SharedPV` (via `onRPC`), forwards it to the BpmQuery gRPC server,
maps the gRPC reply back to a PVA normative type, and returns it.

The gRPC contract is vendored at [`proto/bpm_query.proto`](../proto/bpm_query.proto)
(copied from `orbit/proto/bpm_query.proto`). Service `bpm.query.v1.BpmQuery`
with RPCs `Average`, `Orbit`, `OnEvent`, `OnEventTime`.

## pvxs RPC mechanism used

`pvxs::server::SharedPV::onRPC(std::function<void(SharedPV&,
std::unique_ptr<ExecOp>&&, Value&&)>)`. The handler receives the client-supplied
argument `Value` (for `pvxcall` this is an `NTURI`, whose `name=value` arguments
land under a `query` sub-structure). It replies with `op->reply(value)` on
success or `op->error(message)` on failure. This is the only RPC entry point the
vendored pvxs exposes for a server-side `SharedPV`, and it fits the existing
IOC, which already registers every PV as a `SharedPV` via `Server::addPV`.

## Config schema

An RPC PV is any `pvs[]` entry that has an `rpc:` block. It does **not** take
`type`/`shape`/`read`/`write`/`confirm`/`transform`/`initial` (those are
rejected) because it never touches Redis.

```yaml
server:
  instance: demo
  # Optional IOC-wide default gRPC endpoint for RPC PVs without rpc.endpoint.
  rpc_default_endpoint: bpm-query-server:50051

pvs:
  - name: BI:ORBIT
    rpc:
      method: OnEvent          # Average | Orbit | OnEvent | OnEventTime
      endpoint: host:port      # optional; falls back to rpc_default_endpoint
      # Optional fixed defaults for the method's fields. Any of these may be
      # overridden at call time by a matching pvxcall argument.
      digitizer: MTCA1-1       # Source.digitizer (Average/OnEvent/OnEventTime)
      subkey: BPM_H1_POS       # Source.subkey
      event: 0xFF              # OnEvent.event (uint32)
      delta_ns: 1000000000     # OnEvent.delta_ns (int64 ns)
      event_time_ns: 0         # OnEventTime.event_time_ns
      start_ns: 0              # window/offset
      end_ns: 0
      length_ns: 0
      per_entry_mean: false    # Average.per_entry_mean
      machine: BOOSTER         # Orbit.machine
      section: ""              # Orbit.section
      start_index: 0           # Orbit.start_index (int32)
      end_index: 0             # Orbit.end_index
```

Either `rpc.endpoint` or `server.rpc_default_endpoint` must be set.

## Argument resolution

For each gRPC field the handler uses, in order:
1. the matching `pvxcall` argument (looked up as `query.<name>`, or top-level
   `<name>`), if the caller supplied it;
2. the fixed default from the `rpc:` config block;
3. a zero / empty fallback.

Argument names match the proto field names: `digitizer`, `subkey`, `event`,
`delta_ns`, `event_time_ns`, `start_ns`, `end_ns`, `length_ns`,
`per_entry_mean`, `machine`, `section`, `start_index`, `end_index`. Numeric
arguments are accepted either natively or as strings (`pvxcall` sends strings),
parsed with `strtoll` (so `0xFF` hex works).

## Example invocations and reply shapes

```sh
# OnEvent: window [t-delta, t] of a source, t resolved from TCLK event 0xFF
pvxcall DEMO:BI:ORBIT event=255 delta_ns=1000000000

# Average over an explicit length window
pvxcall DEMO:BI:AVG length_ns=1000000000

# Orbit table for a machine
pvxcall DEMO:BI:ORBIT:TABLE machine=BOOSTER length_ns=1000000000
```

Reply mapping:

- `QueryReply` (Average / OnEvent / OnEventTime) ->
  - 1 value: **NTScalar** (`epics:nt/NTScalar:1.0`), `value` = the double.
  - otherwise: **NTScalarArray** of doubles, `value` = the array.
  - Both carry extra members `times_ns` (int64 array) and `event_time_ns`
    (int64), plus `display.units` from the reply's `units`.
- `OrbitReply` (Orbit) -> **NTTable** (`epics:nt/NTTable:1.0`) with columns
  `h_name`, `h_orbit`, `h_intensity`, `v_name`, `v_orbit`, `v_intensity`.

If the gRPC reply's `status.ok` is false, or the transport fails, the handler
returns a PVA error (`op->error(...)`) and `pvxcall` reports it.

## Build / run requirements

The IOC now links gRPC and protobuf. The `Dockerfile` builder stage installs
`libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc`; the
runtime stage installs `libgrpc++1.51t64 libprotobuf32t64`. CMake uses
`find_package(Protobuf CONFIG)` + `find_package(gRPC CONFIG)` and
`protobuf_generate` to build `proto/bpm_query.proto` into a `bpm-query-proto`
static lib that the core links against (same pattern as `orbit/CMakeLists.txt`).

Locally (outside the container) you need the same dev packages plus the existing
EPICS-base/pvxs bootstrap (see [../README.md](../README.md)) before
`cmake -S . -B build && cmake --build build`.
