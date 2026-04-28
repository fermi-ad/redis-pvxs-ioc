# Feature State and Roadmap

`redis-pvxs-ioc` is currently a standalone PVAccess-only Redis-backed value server with a YAML-defined control plane. It already serves values, writes, confirms, alarms, metadata, transforms, and hot reloads without an IOC database layer.

This roadmap is not business-priority ordered. It is grouped into capability tracks, with only dependency sequencing called out.

Assumed defaults:

- compatibility model: sidecar lane
- long-term Redis direction: everything dynamic
- core product stays PVA-first and does not become a general-purpose IOC host

## Current State

### Supported now

- PVA only
- Redis-backed read, write, and confirm/readback routes
- one or many standalone Redis backends in a config generation
- `NTScalar` / `NTScalarArray` serving with `alarm`, `timeStamp`, `display`, `control`, and `valueAlarm`
- metadata fields such as units, description, precision, form, display/control limits
- scalar alarm evaluation plus Redis alarm-stream publishing
- linear transforms for floating-point PVs
- hot config reload with generation swap
- released container image and semver flow

### Not supported now

- PV settings through the full Redis path
  - values flow through Redis today
  - metadata, alarm thresholds, transforms, routes, namespace/bind settings, and future policy assignment still come from YAML
- ACF enforcement or ACF reload
- recaster or any other EPICS support module integration
- any backend adapter besides Redis
- CA service or CA compatibility layer
- broader EPICS normative-type coverage beyond `NTScalar` and `NTScalarArray`
- Redis Cluster
- general-purpose IOC hosting or in-process support-module loading

## Capability Tracks

### 1. Control Plane and Schema Foundation

This is the base track for every later feature.

- Complete config schema versioning and compatibility rules in issue `#2`.
- Separate the product model into:
  - value plane: runtime value/read/write/confirm/alarm data
  - definition plane: PV definition, metadata, transforms, alarm config, ASG assignment, routing
- Add source revision reporting, config diff/reporting, and reload diagnostics in issue `#3`.
- Finish runtime hardening in issue `#4` so backend loss, bad replacement generations, and stale callback fencing are proven before larger feature tracks build on top.

### 2. Normative Types Coverage

This track captures the long-term goal of broader alignment with the EPICS V4 Normative Types specification.

- The umbrella is issue `#17`.
- The groundwork is issue `#20`.
- General normative types are issue `#18`.
- Specific normative types are issue `#19`.
- Appendix A future additions are issue `#21`.
- Current support remains limited to `NTScalar` and `NTScalarArray`.
- The long-term target includes:
  - `NTEnum`, `NTMatrix`, `NTURI`, `NTNameValue`, `NTTable`, `NTAttribute`
  - `NTMultiChannel`, `NTNDArray`, `NTContinuum`, `NTHistogram`, `NTAggregate`
  - `NTUnion`, `NTScalarMultiChannel`
- This track should build a reusable internal framework for:
  - self-identification and type IDs
  - richer optional metadata handling
  - validation
  - structured and union-bearing payload construction
- This track must remain compatible with the zero-downtime generation model and the long-term Redis-backed definition/settings direction.

### 3. Full Redis Settings Path

This is the long-term "everything dynamic" track.

- This track is captured in issue `#15`.
- Add a Redis-backed definition source so PV definitions are no longer file-only.
- Move these settings into the dynamic definition plane:
  - PV names and types
  - read/write/confirm routes
  - metadata
  - alarm thresholds
  - transforms
  - backend selection
  - ASG assignment
- Keep the served PVA contract unchanged:
  - clients still see standard `NTScalar` / `NTScalarArray`
  - `display`, `control`, and `valueAlarm` stay in served units
- Preserve generation-based apply:
  - build the next definition generation off-thread
  - validate before cutover
- reject bad definitions without disturbing the live generation
- Keep YAML as bootstrap/fallback initially, then treat it as just one control-plane source.

### 4. Security and ACF

ACF remains a built-in product requirement after MVP.

- Implement the standalone ACF subset in issue `#5`.
- Use EPICS access security directly rather than inventing a product-specific ACL format.
- First ACF milestone should include:
  - `ASG`, `RULE`, `UAG`, `HAG`
  - read/write checks
  - long-lived permission recomputation
  - HAG DNS refresh behavior
  - trapwrite-style audit events
- Explicitly defer:
  - `INPA..INPU` / `CALC()` parity
  - record-field-specific historical semantics
- Keep ACF as a separate policy source at first.
  - dynamic Redis-backed ASG assignment is in scope early
  - Redis-delivered ACF text can be a later extension after file/text-based ACF is stable

### 5. Compatibility Lane for Support Modules

The core runtime should not become a support-module host.

- This track is captured in issue `#14`.
- Add client/backend adapters so external IOC sidecars can be consumed and re-exported.
- Default compatibility path:
  - run the support module in a conventional IOC
  - consume its PVs from `redis-pvxs-ioc`
  - compose or mirror them into the served namespace
- Recast/recaster belongs here:
  - no in-process recaster support in the core
  - first viable path is a sidecar IOC plus a new backend adapter, preferably PVA first
- This track should produce a clean answer to "can you run our support module?" without compromising zero-downtime guarantees.

### 6. CA Compatibility Track

CA stays out of the core runtime.

- This track is captured in issue `#13`.
- If backward compatibility is needed, add CA as a separate facade or bridge track over the same runtime and definition model.
- The CA roadmap should assume:
  - legacy CA clients talk to a compatibility layer
  - the compatibility layer consumes the same Redis-backed definitions and/or PVA-served state
  - the PVA core remains the primary product architecture
- Do not let CA requirements drag record/device/database behavior back into the core server.

### 7. Release and Operational Maturity

Release automation is a separate ops track.

- Keep image publishing and release automation in issue `#8`.
- Keep product-facing observability with track 1 rather than burying it in release tooling.

## Public Interface Changes To Expect

- New control-plane abstraction beyond file YAML:
  - file source
  - Redis definition source
- Versioned PV-definition schema that can represent:
  - metadata
  - alarms
  - transforms
  - backend routing
  - ASG assignment
- Future compatibility adapters:
  - Redis remains the current backend
  - PVA client adapter is the likely first non-Redis adapter
  - CA compatibility, if added, should be a separate surface, not a change to the core PVA contract

## Test and Acceptance Scenarios

### Control plane

- invalid next-generation definitions are rejected and the current generation stays live
- reload reporting shows added, removed, and structurally changed PVs
- backend loss and reconnect do not crash the process

### Full Redis settings path

- updating units, limits, transforms, alarm thresholds, or routing in the definition source updates live behavior without restart
- clients see updated metadata on the served PVs after cutover
- bad dynamic settings do not partially apply

### ACF

- read/write allow/deny matches sample ACF files
- HAG DNS changes affect long-lived clients correctly
- trapwrite audit records are emitted with user, host, PV, value, result, and timestamp

### Support-module compatibility

- a sidecar IOC can be consumed and re-exported without embedding it in-process
- sidecar outage does not corrupt or stall unrelated Redis-backed PVs
- recaster-style functionality can be surfaced through the compatibility lane

### CA compatibility

- legacy CA clients can read/write through the compatibility layer
- CA support does not weaken PVA reload guarantees or reintroduce IOC database coupling into the core
