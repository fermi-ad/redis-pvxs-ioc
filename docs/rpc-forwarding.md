# Generic PVA RPC -> gRPC forwarding

The IOC can act as a generic bridge between PVA `pvxcall` and any gRPC service.
A client runs `pvxcall <PVName> <args>`; the IOC forwards it to one method of a
backend gRPC service and maps the reply back into a PVA value.

**The IOC has no compiled-in knowledge of any application service.** It learns a
service's methods and message schema at runtime via gRPC **server reflection**,
so the same IOC image works against any reflection-enabled backend — you only
tell it *which service, at which endpoint*. The only proto compiled into the IOC
is the standard gRPC reflection proto (`proto/reflection.proto`).

## How it works

1. At startup, for each configured `rpc_services` entry, the IOC opens the
   backend's `ServerReflection` service and fetches the `FileDescriptorProto`s
   for the named service (and its dependencies), building a `DescriptorPool`.
2. For each method it creates a PVA `SharedPV` named
   `<server.namespace>:<UPPER_SNAKE(MethodName)><suffix>` with an `onRPC`
   handler. (`Average` -> `AVERAGE`, `OnEventTime` -> `ON_EVENT_TIME`.)
3. On a call, the handler builds the method's request as a protobuf
   `DynamicMessage`, sets fields from the merged `{defaults + pvxcall args}` map,
   makes a generic unary call (`grpc::GenericStub`), parses the reply into a
   `DynamicMessage`, and converts it to a pvxs `Value` mirroring the reply.

The backend must enable reflection (one line:
`grpc::reflection::InitProtoReflectionServerBuilderPlugin()` + link
`grpc++_reflection`). If the backend is down at IOC startup, reflection is
retried for ~30 s before that service's PVs are skipped.

## Config schema

```yaml
server:
  namespace: BI:BPM          # prefixes every derived PV name

rpc_services:
  - endpoint: bpm-query-server:50051   # gRPC host:port
    service: bpm.query.v1.BpmQuery     # fully-qualified service name (reflected)
    suffix: _RPC                       # optional; appended to each PV name
    defaults:                          # optional fixed request-field defaults
      digitizer: MTCA1-1               #   applied before per-call args; a method
      subkey: BPM_H1_POS               #   ignores fields it does not have, so one
      length_ns: 1000000000            #   map can serve every method
      event: 0xFF
```

`pvs` and `rpc_services` are both optional individually, but at least one must be
present. RPC services never touch Redis.

## Argument mapping

A request field is addressed by its proto field name. Because pvxcall args are
flat `name=value` pairs, the handler resolves each name as:
1. an exact top-level field, else
2. a dotted path (`source.digitizer`), else
3. a **unique** leaf field name anywhere in the request message.

So for `AverageRequest{ Source source; Window window; }`, you can pass
`digitizer=MTCA1-1 subkey=BPM_H1_POS length_ns=...` (unique leaves) or the
explicit `source.digitizer=... window.length_ns=...`. Values arrive as strings
(pvxcall) and are converted to the field's type; integers use `strtoll` base 0,
so `0xFF` works. Per-call args override `defaults`.

## Reply mapping

The reply pvxs `Value` mirrors the protobuf reply message generically:
scalar -> scalar field, `repeated` scalar -> array field, singular message ->
sub-struct (recursively). Field names are the proto field names. (Repeated
*message* fields are not supported and raise an error.)

This is generic, so the reply is a plain struct, not a hand-shaped normative type
(`NTScalar`/`NTTable`) — if a normative shape matters, shape the **reply message**
on the server. A non-OK gRPC status or transport failure becomes a PVA error
(`op->error`), which `pvxcall` reports.

## Example

```sh
pvxcall BI:BPM:AVERAGE_RPC digitizer=MTCA1-1 subkey=BPM_H1_POS length_ns=1000000000
pvxcall BI:BPM:ORBIT_RPC   machine=BOOSTER
pvxcall BI:BPM:SLICE_RPC   digitizer=MTCA1-1 subkey=BPM_H1_I_A start_index=0 length=16
```

## Build / run requirements

The IOC links gRPC + protobuf. The `Dockerfile` builder installs
`libgrpc++-dev libprotobuf-dev protobuf-compiler protobuf-compiler-grpc`; the
runtime installs `libgrpc++1.51t64 libprotobuf32t64`. CMake uses module-mode
`find_package(Protobuf)` (Ubuntu ships no `ProtobufConfig.cmake`) +
`find_package(gRPC CONFIG)` and invokes `protoc` directly on
`proto/reflection.proto`. The generic dynamic call uses `grpc::GenericStub` +
`google::protobuf::DynamicMessage` (no per-service generated stubs).
