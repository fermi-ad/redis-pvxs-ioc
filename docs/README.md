# Documentation

## Users and operators

- [Redis-only quick start](redis-only-quickstart.md): adopt the released image
  with a deployment-owned YAML file.
- [Configuration reference](configuration.md): supported YAML sections, fields,
  defaults, types, and validation rules.
- [Operations and diagnostics](operations.md): hot reload, built-in PVs, health,
  and troubleshooting.
- [Access control](access-control.md): opt-in native ACF policy, hot reload,
  watcher behavior, auditing, and diagnostics.
- [Live ACF validation](access-live-validation.md): controlled acceptance and
  rollback procedure for a representative deployed IOC.
- [PVAccess networking](pva-networking.md): container discovery and routable
  deployment patterns.
- [Demo guide](demo.md): full core demo and validation sequence.

## Integrators

- [ChannelFinder sync](channelfinder-sync.md): preview and publish configured PV
  catalog entries.
- [PVA RPC to gRPC forwarding](rpc-forwarding.md): expose reflected gRPC methods
  as RPC PVs.
- [Discovery](reccaster.md): native RecCeiver catalog registration and PVA search.

## Contributors

- [Architecture](design.md): implemented runtime boundaries and data flow.
- [Building from source](building-from-source.md): prerequisites, pinned or
  existing EPICS/PVXS builds, tests, and local images.
- [Development guide](development.md): the contributor build/test loop.
- [Performance measurement](performance.md): repeatable ACF baseline, mixed,
  and reload-under-load benchmark report.
- [Feature state and roadmap](feature-state-roadmap.md): current capabilities and
  tracked future work.
- [Normative types roadmap](normative-types-roadmap.md): expansion beyond
  `NTScalar` and `NTScalarArray`.

## Maintainers

- [Pinned dependency remotes](submodule-remotes.md): public submodule sources and
  pinned revision checks.
- [Release process](releasing.md): semver, image identity, publishing, and
  post-release digest synchronization.
- [Contributing](../CONTRIBUTING.md), [security policy](../SECURITY.md), and
  [third-party notices](../THIRD_PARTY_NOTICES.md).

## Historical design material

- [Sidecar retirement and migration](legacy-sidecar.md)
- [Original design proposal](history/original-design.md)
- [Initial MVP specification](history/mvp-spec.md)
- [Architecture decisions](adr/)

Historical documents explain how the project reached its current design. They
are not the current configuration or runtime contract.
