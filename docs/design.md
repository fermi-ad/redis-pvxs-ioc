# Architecture

`redis-pvxs-ioc` is a standalone PVXS server. It exposes Redis-backed process
variables over PVAccess without loading an EPICS database or calling `iocInit()`.

## Runtime boundaries

- PVAccess is the core client protocol. The main process does not serve Channel
  Access or load record/support-module databases.
- YAML is the current definition plane for server settings, Redis backends, PV
  definitions, alarm publication, ChannelFinder metadata, and optional gRPC
  forwarding.
- Redis is the value plane for configured reads, writes, confirmations, and alarm
  stream events. One generation may use multiple independent Redis servers.
- The legacy conventional IOC is a separate, optional sidecar with its own image
  and release history.

## Startup and configuration

The process loads and validates the entire YAML file before starting the server.
It constructs:

1. the PVXS server and its network configuration;
2. the always-on operational PV namespace;
3. one Redis adapter per configured backend;
4. Redis-backed PV runtimes and alarm publication;
5. optional reflected gRPC RPC PVs.

`--check-config` performs the same schema and naming validation without serving
PVs. Configuration errors include a YAML path so a bad definition can be fixed
before deployment.

## PVA data model

Configured values are served as `NTScalar` or `NTScalarArray`. Scalar and array
element types are explicit, and the server constructs the normative fields
expected by clients such as Phoebus:

- `value`, `alarm`, `timeStamp`, and `display`;
- `control` and `valueAlarm` for numeric values;
- units, description, precision, display form, limits, and minimum step metadata.

Routes use Redis stream keys through the pinned `redis-adapter`. A PV may have a
read route, optional write route, and optional confirmation/readback route.
Floating-point values and arrays may use a linear scale/offset transform; clients
always see served-unit values.

## Generation-based hot reload

`SIGHUP` and `SYS:<instance>:config:reload` request a reload. The process parses
and validates the replacement file, constructs the next generation, and only
then applies it. Unaffected PVs remain live. Removed or structurally changed PVs
are deactivated, and stale callbacks from prior generations are fenced.

If parsing, validation, backend construction, or RPC reflection fails, the new
generation is rejected and the active generation continues serving. The
generation, last status, and last error are observable through built-in PVs.
Server namespace and bind settings are intentionally immutable after startup.

## Operational namespace

The runtime always exposes version/revision aliases and the `SYS:<instance>`
diagnostic namespace. These PVs report configuration generation, reload state,
configured PV count, and Redis backend connectivity. The complete table and
verification commands are in [`operations.md`](operations.md).

## Optional integrations

- [`channelfinder-sync.md`](channelfinder-sync.md) documents one-shot catalog
  generation and publication for configured Redis PVs.
- [`rpc-forwarding.md`](rpc-forwarding.md) documents reflection-based PVA RPC to
  gRPC forwarding.
- [`legacy-sidecar.md`](legacy-sidecar.md) documents the experimental conventional
  IOC compatibility path. It is not part of the main runtime process.

## Dependencies and releases

Build dependencies are pinned as git submodules to make source revisions
reproducible. The release image embeds semver and Git revision metadata; deployment
references retain the human-readable tag and immutable registry digest. See
[`submodule-remotes.md`](submodule-remotes.md) and [`releasing.md`](releasing.md).
