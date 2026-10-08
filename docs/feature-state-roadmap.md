# Feature State and Roadmap

`redis-pvxs-ioc` is a released, standalone PVAccess service with a YAML definition
plane and Redis value plane. This document separates current behavior from tracked
future work. The ordered [path to v0.9.0 and v1.0 RC](roadmap-v0.9.md)
is the release checklist and maps each review finding to an outcome.

## Supported now

### Core runtime

- PVA-only serving without an EPICS database layer
- `NTScalar` and `NTScalarArray` values
- Redis-backed read, write, and confirmed readback routes
- one or many standalone Redis backends per config generation
- metadata, display/control limits, scalar alarms, and linear transforms
- Redis alarm-stream publishing
- generation-based YAML hot reload through `SIGHUP` or an admin PV
- rejection of bad replacement configurations while the active generation stays live
- built-in version, revision, config status/error/generation, PV count, and backend
  health PVs
- explicitly enabled native EPICS ACF enforcement for PV, RPC, and admin
  endpoints, with dedicated, whole-config, and optional watched-file reload

### Integrations and packaging

- one-shot ChannelFinder dry-run and publication for Redis-backed definitions
- generic reflection-based PVA RPC to gRPC forwarding
- semver, embedded source revision, OCI image metadata, and immutable image digests
- packaged PVXS command-line tools in the runtime image
- native RecCeiver catalog registration with alias/metadata updates and recovery

## Not supported now

- Redis-backed PV definitions or metadata; definitions still come from YAML
- Channel Access in the main runtime
- normative types beyond `NTScalar` and `NTScalarArray`
- Redis Cluster
- general-purpose IOC hosting or in-process support-module loading
- hot changes to server instance, namespace, interfaces, ports, or beacon settings

## Active roadmap

### Configuration and reliability

- [#2: Version the config schema and define compatibility rules](https://github.com/fermi-ad/redis-pvxs-ioc/issues/2)
- [#3: Add config diff/report and reload observability](https://github.com/fermi-ad/redis-pvxs-ioc/issues/3)
- [#4: Harden backend outage and invalid reload behavior](https://github.com/fermi-ad/redis-pvxs-ioc/issues/4)
- [#15: Add a Redis-backed definition source and full settings path](https://github.com/fermi-ad/redis-pvxs-ioc/issues/15)
- [#66: Reserve the complete always-on admin PV namespace](https://github.com/fermi-ad/redis-pvxs-ioc/issues/66)
- [#67: Always refresh user-supplied tag images in smoke tests](https://github.com/fermi-ad/redis-pvxs-ioc/issues/67)

The Redis definition path must preserve generation staging: validate the full next
definition set before cutover and leave the active generation untouched on error.

### Normative types

- [#17: Normative-types umbrella](https://github.com/fermi-ad/redis-pvxs-ioc/issues/17)
- [#20: Reusable IDs, metadata, validation, and union framework](https://github.com/fermi-ad/redis-pvxs-ioc/issues/20)
- [#18: General normative types](https://github.com/fermi-ad/redis-pvxs-ioc/issues/18)
- [#19: Specific normative types](https://github.com/fermi-ad/redis-pvxs-ioc/issues/19)
- [#21: Appendix A additions](https://github.com/fermi-ad/redis-pvxs-ioc/issues/21)

See [`normative-types-roadmap.md`](normative-types-roadmap.md) for the target type
families and implementation guardrails.

### Product boundary

Automatic discovery is a v0.9.0 release requirement. The standalone PVA service
owns it; conventional IOC hosting and a CA facade are outside this product.
The legacy sidecar is retired, with historical images and source retained in
[the migration notes](legacy-sidecar.md). No replacement repository is planned.

### Release maturity

Automated runtime image publishing and GitHub Releases were completed in
[#8](https://github.com/fermi-ad/redis-pvxs-ioc/issues/8). Remaining release work
focuses on dependency hygiene, public contributor safety, reproducible provenance,
and keeping checked-in image examples synchronized with immutable release digests.

## Expected public-interface evolution

Future work may introduce a versioned definition schema, new normative PVA types,
and Redis-backed definitions. Native ASG assignment already exists. None of these should silently reinterpret an existing YAML
contract or change the meaning of current `NTScalar`/`NTScalarArray` PVs.

## Acceptance principles

- invalid definitions never partially replace the active generation;
- stale callbacks from old generations cannot mutate live state;
- backend loss and reconnect do not crash the process;
- metadata and route changes become observable without a process restart;
- compatibility layers do not weaken the core PVA reload model;
- every release preserves the semver → Git revision → image digest identity chain.
