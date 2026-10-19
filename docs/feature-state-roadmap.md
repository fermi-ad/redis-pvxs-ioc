# Feature State and Roadmap

`redis-pvxs-ioc` is a released, standalone PVAccess service with a YAML definition
plane and Redis value plane. This document separates current behavior from tracked
future work. The [path to v1.0.0](roadmap-v1.0.md) is the evolving candidate and
decision map.
The [v0.9 roadmap](roadmap-v0.9.md) is archived planning history; the
[v0.9.0 release](https://github.com/fermi-ad/redis-pvxs-ioc/releases/tag/v0.9.0)
contains the release evidence.

## Supported now

### Core runtime

- PVA-only serving without an EPICS database layer
- `NTScalar` and `NTScalarArray` values
- read-only Redis-backed `NTNDArray` image values with envelope/pixel validation
  and last-good-frame preservation
- Redis-backed read, write, and confirmed readback routes
- one or many standalone Redis backends per config generation
- metadata, display/control limits, scalar alarms, and linear transforms
- Redis alarm-stream publishing
- schema-version validation, machine-readable configuration checks/differences,
  and generation-based YAML hot reload through `SIGHUP` or an admin PV
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
- normative types beyond `NTScalar`, `NTScalarArray`, and the supported read-only
  `NTNDArray` adapter
- Redis Cluster
- general-purpose IOC hosting or in-process support-module loading
- hot changes to server instance, namespace, interfaces, ports, or beacon settings

## Path to 1.0 and candidate work

[The 1.0 decision map](roadmap-v1.0.md) records consumers, benefits, evidence,
dependencies, interfaces, uncertainties, next validation, disposition and ownership.
Its candidate list is open for evaluation, not a locked release checklist.

The current gates are Linux amd64-only IOC qualification, exact official EPICS
Base/PVXS dependencies (#138 with #122 startup evidence), the remaining #2
compatibility-policy decision, maintenance/handoff (#155), independent review and
the focused 1.0 RC qualification policy/evidence work (#156). Linux arm64 CI remains informational; Darwin
build code remains without a support promise. ARM Redis producers are unaffected.

Candidates include Redis definitions (#15), SecurePVA (#47), Redis TLS,
authenticated gRPC, opt-in acquisition timestamps and producer command correlation,
write tolerance/range policy, Ubuntu dependency retention (#141), and an XRM PVA
gateway with sample forwarding to Redis (#148–#150). The XRM consumer
profile keeps acquisition setup, triggers and scan sequencing in the external
application. The independent BCM I2C-to-Redis producer has its own compatibility
profile; it is not a D-TACQ adapter. Follow the decision map for focused issues and
current dispositions.

### Normative types

The read-only Redis-backed `NTNDArray` subset is delivered. Broader coverage
remains consumer-driven under [#17](https://github.com/fermi-ad/redis-pvxs-ioc/issues/17),
with [#20](https://github.com/fermi-ad/redis-pvxs-ioc/issues/20) for reusable
framework work, [#18](https://github.com/fermi-ad/redis-pvxs-ioc/issues/18) for
general types, [#19](https://github.com/fermi-ad/redis-pvxs-ioc/issues/19) for
additional specific types and [#21](https://github.com/fermi-ad/redis-pvxs-ioc/issues/21)
for Appendix A additions. Characterizing structured upstream XRM PVA values does
not by itself commit to new Redis-native type families.

See [the normative-types roadmap](normative-types-roadmap.md) for type families
and implementation guardrails.

### Product and compatibility boundaries

Native PVA search and RecCeiver catalog registration are delivered. The legacy
sidecar is retired, with historical source/images retained in the
[migration notes](legacy-sidecar.md); no replacement repository is planned.
Conventional IOC hosting and a CA facade remain outside the core release scope.
Production fleet rollout is separate from release qualification.

Existing schema-version handling, ASG assignment and runtime interfaces are
implemented behavior. Future definition, wire and type changes need explicit
compatibility decisions; they must not silently reinterpret valid YAML or Redis
payloads. Current confirmation observes readback rather than command causality,
and control limits remain advisory. Tolerance, opt-in range enforcement and causal
correlation remain distinct candidates, not delivered features.

## Acceptance principles

- invalid definitions never partially replace the active generation;
- stale callbacks from old generations cannot mutate live state;
- backend loss and reconnect do not crash the process;
- metadata and route changes become observable without a process restart;
- compatibility layers do not weaken the core PVA reload model;
- every release preserves the semver → Git revision → image digest identity chain.
