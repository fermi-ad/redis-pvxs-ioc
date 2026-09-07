# Legacy sidecar retirement

The conventional IOC sidecar is retired from the core product on the path to
v0.9.0. Its source, build overlays, support-module build scripts, fourteen
legacy-only submodules, and vendored PCRE tree are removed from the active
checkout. Historical Git tags and published images remain available. There is
no replacement repository or new sidecar release track.

Discovery is preserved in the standalone service. Native RecCeiver registration
advertises the actual Redis/PVXS PVs, aliases, diagnostics and reflected RPCs,
updates on successful reload, and recovers after receiver restart. It needs no
EPICS database, support modules or CA server. See [Discovery](reccaster.md).

## Historical identity

- Last published sidecar image:
  `adregistry.fnal.gov/instrumentation/redis-pvxs-ioc-legacy-sidecar:v0.5.0@sha256:91848ceecca14f65195be671a5fd78982366d9e28ad93bd91e046b69f75bd401`.
- Historical release source: tag `v0.5.0`, commit
  `add1dd8577fdb33f9cfb79ce7ba3e3db88e6d618` in this repository.
- Source and build instructions remain accessible through that tag. Preserving
  this source reference does not create new provenance for the old image.

The inspected image has no source-revision label (only an inherited Ubuntu
version label). The release tag above identifies the historical source; it is
not an assertion of reproducible-build provenance for that old image.

## Migration

1. Keep the deployment's existing immutable image references, configuration,
   startup files and persistent data available for rollback.
2. Validate the standalone release in isolation. Ensure the service receives
   RecCeiver announcements on UDP 5049 and that PVA clients can reach its
   advertised address and TCP port.
3. Verify the canonical PVs and aliases in ChannelFinder, read the structured
   `SYS:<instance>:discovery:status`, and perform PVA reads using the registered
   address and port. Verify alias/metadata changes and receiver restart before
   changing the deployment.
4. If the old sidecar exists only for discovery, remove it from the deployment's
   Compose definition during that deployment's normal change procedure.
   If it hosts device logic or conventional records, retain the historical image
   until those consumers have a separately reviewed migration. Such hosting is
   outside this service's product boundary.

The legacy RecCaster enumerated the sidecar's own database records. Native
discovery enumerates the standalone service's configured registry; it does not
invent replacements for conventional support records. The legacy
`<prefix>:RecCaster:State-Sts` and `Msg-I` records are replaced operationally by
the structured native discovery status PV. Existing device PV names and aliases
remain governed by their YAML definitions.

Qualification used an isolated real RecCeiver, ChannelFinder 4.7.3 and
Elasticsearch 8.11.4, including catalog-driven PVA reads with UDP disabled,
rejected staging, alias/metadata updates, and receiver restart on a different
port. Production fleet rollout is a separate deployment task.
