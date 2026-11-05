# Path to v1.0.0

[Issue #144](https://github.com/fermi-ad/redis-pvxs-ioc/issues/144) is the evolving
scope and decision map. The candidates below remain visible while their consumers,
benefits, interfaces and costs are understood. Listing a candidate does not commit
it to 1.0; adding, removing or accepting scope requires a recorded decision and
linked evidence. Owners are unassigned until someone accepts the work.

[v0.9.0 is released](https://github.com/fermi-ad/redis-pvxs-ioc/releases/tag/v0.9.0).
Its [archived roadmap](roadmap-v0.9.md) preserves the planning history. This roadmap
does not establish RC qualification, change `VERSION`, or alter runtime contracts.

## Settled boundaries and release gates

Linux amd64 is the only required IOC release and validation target. Existing Linux
arm64 CI remains informational. Existing Darwin build code is retained without a
macOS support or qualification promise. This IOC platform choice does not restrict
ARM devices that produce data for Redis.

- [#145](https://github.com/fermi-ad/redis-pvxs-ioc/issues/145) /
  [#146](https://github.com/fermi-ad/redis-pvxs-ioc/pull/146): remove macOS CI and
  forward qualification requirements on `main`, then synchronize integration
  through a reviewed PR. At this roadmap update, #146 is open with passing CI and
  independent review still required; it is an early mainline prerequisite.
- [#138](https://github.com/fermi-ad/redis-pvxs-ioc/issues/138): build and qualify
  the release from exact official EPICS Base and PVXS revisions, without fork-only
  APIs. Reconcile its acceptance criteria with the Linux amd64 platform decision.
  [#122](https://github.com/fermi-ad/redis-pvxs-ioc/issues/122) owns the associated
  startup/reconnect evidence and accepted bound; a fixture deadline is not a
  product guarantee.
- [#2](https://github.com/fermi-ad/redis-pvxs-ioc/issues/2): preserve the delivered
  schema-version behavior and resolve the remaining compatibility-policy decision.
- [#155](https://github.com/fermi-ad/redis-pvxs-ioc/issues/155): complete the
  compatibility, support/security maintenance and maintainer handoff contract;
  record the remaining policy choices without assuming their answers.
- [#156](https://github.com/fermi-ad/redis-pvxs-ioc/issues/156):
  define and validate the 1.0 RC qualification policy, representative workload,
  proposed seven-day soak, full regression and rollback evidence, blocker
  dispositions, and evidence package. The existing exact-v0.9.0 policy is not
  automatically a 1.0 policy. Final qualification uses the exact merged `main`
  revision and immutable candidate digest. Resolve or explicitly disposition the
  remaining [#4](https://github.com/fermi-ad/redis-pvxs-ioc/issues/4) investigation
  and [#24](https://github.com/fermi-ad/redis-pvxs-ioc/issues/24) cache/allocator report;
  their open investigation status does not establish a confirmed current defect.

Redis remains standalone only; Redis Cluster is outside the supported portfolio.
Conventional IOC hosting, support-module expansion and a CA facade are outside the
core release scope. The legacy sidecar is retired; retain historical images,
source and [migration notes](legacy-sidecar.md), and close its retired expansion
work. Production fleet rollout is a separate deployment task.

## Consumer profiles

**Allison scanner.** [The scanner](https://github.com/fermi-ad/allison-scanner)
and [engineering application](https://github.com/fermi-ad/allison-scanner-eng-app)
use XRM PVA controls and raw TCP port 4210
sample ingestion. Acquisition setup, triggers and scan sequencing remain owned by
the external application. The proposed IOC capability covers configured XRM IPs,
explicit upstream/exported PV mappings, ACF access, catalog publication, sample
forwarding to Redis and observable health. It does not make the IOC the capture
or scan controller. The established public-controls → IOC/ACF → private-bidaq
placement is the deployment model.

**Invasive BCM Raspberry Pi.**
[The independent I2C-to-Redis bridge](https://github.com/fermi-ad/invasive-current-monitor-ioc)
is not a D-TACQ integration. Its
[adopter-owned #14](https://github.com/fermi-ad/invasive-current-monitor-ioc/issues/14)
tracks producer-side compatibility evidence. Shared IOC validation must cover
payload bytes, nanosecond timestamps, command/confirmation ownership, heartbeat
and reconnect behavior. The Pi remains an ARM producer under the amd64-only IOC
release policy.

**Other adopters and operators.** Definition authors, existing Redis producers,
PVA clients, gRPC services and maintainers motivate the other candidates below.
Do not assume each consumer needs every new interface. Preserve valid existing
configuration and payload behavior unless an explicit opt-in contract is accepted.

## Scope decision matrix

Each row records consumer, benefit, evidence, dependencies, interface, uncertainty,
next validation, disposition and owner. “Evaluate” keeps a candidate open; it does
not mark its implementation as an approved release gate. Maintenance/handoff is
an established gate with policy details still to decide.

| Candidate | Consumer / benefit | Evidence / dependencies | Interface / uncertainty | Next validation | Disposition | Owner |
| --- | --- | --- | --- | --- | --- | --- |
| [XRM umbrella #148](https://github.com/fermi-ad/redis-pvxs-ioc/issues/148): [PVA gateway #149](https://github.com/fermi-ad/redis-pvxs-ioc/issues/149) / [sample bridge #150](https://github.com/fermi-ad/redis-pvxs-ioc/issues/150) | Allison; reuse XRM controls/data with IOC access, catalog and health | Consumer profile above; vendor PVA/stream interfaces; depends on source abstraction and explicit mappings | Configured IPs, upstream/exported PVs, PVA operations and TCP samples to Redis; structured types and sample contract need characterization | Characterize representative controls, grouped values, sample bytes/times and reconnects; distinguish status/heartbeat, PVA health, sample freshness and Redis delivery | Evaluate; external application keeps acquisition control | Unassigned |
| [Redis definitions #15](https://github.com/fermi-ad/redis-pvxs-ioc/issues/15) | Definition authors; live centrally managed definitions/settings | Delivered schema/reload foundation; depends on #2 compatibility decisions | Immutable snapshots, ownership/revision checks, atomic activation, last-good/file recovery; reconcile focused acceptance with #144 | Compare a concrete adopter's authoring/recovery flow with YAML and validate failed activation | Evaluate | Unassigned |
| [SecurePVA #47](https://github.com/fermi-ad/redis-pvxs-ioc/issues/47) | PVA clients/operators; authenticated transport and certificate lifecycle | Upstream readiness below; must reconcile with official-dependency gate #138 | TLS identity, trust and certificate operations; full required code remains unmerged | Establish exact official dependency capabilities and demonstrate intended client/server operations before accepting scope | Evaluate; dependency readiness unresolved | Unassigned |
| [Redis TLS](https://github.com/fermi-ad/redis-pvxs-ioc/issues/151) | Redis deployments; protected backend transport | Separate from #47; depends on adapter/client capability and deployment need | Backend TLS, identity/trust and secret inputs; supported modes/rotation not selected | Identify adopter endpoints and exercise verified connections, failures and recovery | Evaluate | Unassigned |
| [Authenticated gRPC](https://github.com/fermi-ad/redis-pvxs-ioc/issues/152) | Reflected RPC adopters; authenticated upstream calls | Existing optional reflection forwarding; separate from #47 | gRPC channel authentication and credentials; consumer model unselected | Characterize a representative service's reflection/call requirements and credential failures | Evaluate | Unassigned |
| [Acquisition timestamps](https://github.com/fermi-ad/redis-pvxs-ioc/issues/153) | Allison, BCM and other producers; explicit acquisition time independent of ordering | Existing #24 cursor/timestamp investigation and current payload contracts | Opt-in wire evolution; nanosecond timestamp compatibility and migration need evidence | Retain producer-byte/timestamp fixtures and compare old/new interpretation without changing legacy semantics | Evaluate | Unassigned |
| [Producer command correlation](https://github.com/fermi-ad/redis-pvxs-ioc/issues/154) | Command producers/clients, including BCM where applicable; distinguish causal acknowledgement from observed readback | Existing confirmed-put behavior; depends on producer ownership and adoption | Opt-in command/acknowledgement correlation; current value matching is not causality | Trace one command through producer ownership, acknowledgement, timeout and reconnect | Evaluate | Unassigned |
| [Write tolerance and range](https://github.com/fermi-ad/redis-pvxs-ioc/issues/157) | Writable scalar adopters; explicit matching/range policy | Residual #4/#2 planning; current control limits are advisory | Configurable confirmation tolerance and opt-in range enforcement; values/defaults not selected | Characterize existing exact matching and rejection, then propose compatibility-preserving acceptance cases | Evaluate | Unassigned |
| [Ubuntu dependency retention #141](https://github.com/fermi-ad/redis-pvxs-ioc/issues/141) | Release maintainers; reliable pinned candidate/qualification builds | Recorded public archive failures; depends on registry ownership, retention and runner pull access | Immutable builder/runtime dependency images and reviewed refresh; access arrangement unresolved | Build candidate/helper from fresh layers with public Ubuntu access blocked and retain inventories/provenance | Evaluate | Unassigned |
| [Maintenance and handoff #155](https://github.com/fermi-ad/redis-pvxs-ioc/issues/155) | Maintainers/adopters; explicit supported lifecycle | Existing release/security docs; depends on accepted platform/dependency/compatibility choices | Support matrix, schema/security-fix policy and handoff; ownership/duration unresolved | Review a concrete support and release-maintenance workflow with an accepting maintainer | Required gate; policy decisions open | Unassigned |
| [Broader normative types #17–#21](https://github.com/fermi-ad/redis-pvxs-ioc/issues/17) | Consumers requiring richer structures; preserve useful type semantics | Delivered read-only NTNDArray subset; XRM may identify structured pass-through needs | General framework/type families; transparent PVA forwarding differs from new Redis-native type support | Require a concrete consumer and bounded contract before selecting any new type family | Deferred; consumer-driven | Unassigned |

### Dependency readiness and interface distinctions

SecurePVA is not yet a completed official release-line capability. Basic TLS is
in the official PVXS `tls` branch, but
[PVXS #53](https://github.com/epics-base/pvxs/pull/53) into `master` remains draft
and unmerged. Full certificate/status-management work is
[PVXS #199](https://github.com/epics-base/pvxs/pull/199), from
`slac-epics:community-RC` into `tls`; Base identity/policy extensions are
[EPICS Base #942](https://github.com/epics-base/epics-base/pull/942) into `7.0`.
Both remain unmerged at this roadmap update. Keep this dependency conflict visible
when evaluating #47 against #138; do not silently allow forks or remove security.

An XRM PVA gateway, raw acquisition adapter and Redis shim have different
interfaces. Vendor XRM already exposes PVA records and grouped values, including
[SOE NTTable data](https://github.com/D-TACQ/ACQ400_XRM/blob/master/db/soe_hold.db).
Its [instrument module](https://github.com/D-TACQ/ACQ400_XRM/blob/master/xrmIocApp/src/acq400_INST.cpp)
already passes Redis destination and sample-layout inputs to external programs;
that alone does not establish compatibility with this IOC's Redis contracts.
PVXS supplies [client operations and server discovery](https://epics-base.github.io/pvxs/client.html)
and a [Source interface for gateways](https://epics-base.github.io/pvxs/source.html).
Discovery of a PVA server does not identify every XRM unit or enumerate every PV.
Characterize structured values and the explicit export catalog before choosing
transparent forwarding or typed adaptation.

## Assessment order

Finish the platform prerequisite and assess official dependencies, XRM feasibility
and SecurePVA readiness early. Evaluate Ubuntu dependency retention before making
it a final-build prerequisite. Rank the remaining candidates by consumer benefit,
interface/migration cost and evidence, then record accepted, reduced or deferred
scope in #144. Add new candidates through the same decision fields.

Consumer acceptance covers names/types/shapes, timestamp and payload compatibility,
repeated commands and confirmations, outages/recovery, freshness/heartbeats and
access denial. Keep the Pi's existing integration issue as the adopter-owned
validation record and use isolated/simulated devices for regression.

## Branch and review structure

| Branch or PR | Purpose |
| --- | --- |
| `main` | Reviewed mainline and source for final qualification. |
| `dev/v1.0.0` | Protected shared integration branch for reviewed changes. |
| Focused `dev/*` PRs into `dev/v1.0.0` | Review and validate each accepted change. |
| Draft [#147](https://github.com/fermi-ad/redis-pvxs-ioc/pull/147) into `main` | Review the combined diff and land the batch with a merge commit. |

Each focused PR links its owning issue and #144, records validation, and targets
`dev/v1.0.0`. Integrate prerequisites before branching dependent work. Keep the
umbrella's constituent PR list current and record “integrated” separately from
“released.” Completed implementation issues close when their changes reach
`main`, not merely when they enter integration.

Preserve independent Instrumentation code-owner approval and successful Linux
amd64 minimal, sanitizer and image checks. Before marking #147 ready, synchronize
current `main`, reconcile accepted scope and explicit dispositions in #144, and
obtain green combined CI and independent combined review. Use a merge commit to
retain constituent history. Bring #146 and later mainline fixes into integration
through reviewed synchronization PRs; do not force-push or rebase the shared
branch. Final candidate publication, qualification and digest promotion retain
the existing source/trust checks under the separately reviewed 1.0 policy.
