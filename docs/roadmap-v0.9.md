# Path to v0.9.0 and a v1.0 release candidate

The release sequence is **v0.8.2 → v0.9.0 candidates → v0.9.0 → v1.0.0-rc.1**.
The [v0.9.0 milestone](https://github.com/fermi-ad/redis-pvxs-ioc/milestone/1)
and [tracking issue #99](https://github.com/fermi-ad/redis-pvxs-ioc/issues/99)
coordinate the existing backlog. An implementation PR is evidence of progress;
items below remain open until their code is reviewed, merged and qualified.

Valid v0.8 configurations, PV names, aliases and payload contracts remain
compatible. Unsafe inputs are rejected explicitly and changed failure behavior
is documented. Easy automatic PVA/RecCeiver discovery is a release requirement.
CA and conventional IOC hosting are outside this product.

## Ordered delivery

1. Repair and merge the shared adapter boundary upstream; pin that merged
   revision in the IOC. No permanent IOC-specific adapter fork.
2. Publish v0.8.2 correctness fixes and their regression evidence. Update image
   examples only after the validated release digest exists.
3. Retire the legacy sidecar after native discovery qualification, preserving
   historical source/images and migration notes. Retire expansion issues when
   the removal lands; do not create a replacement repository.
4. Separate optional integrations, version and validate configuration, and add
   offline differences and centralized endpoint reservation.
5. Complete transactional reload, bounded asynchronous work, cancellation,
   freshness/readiness, diagnostics and alarm reconciliation.
6. Harden RPC, ChannelFinder HTTP, secret handling, packaging and audit semantics.
7. Integrate the existing NTNDArray PR on the corrected lifecycle.
8. Complete native architecture, capacity, outage, soak, rollback and provenance
   gates, then promote matching source revisions to v0.9.0.

## Review findings and required outcomes

| Done | Finding | Fix, test and documentation outcome | Existing work |
| --- | --- | --- | --- |
| [ ] | Same-key replacement and rejected staging remove active readers | Individually owned handles; replacement/rollback/alias traffic regressions | [adapter #108](https://github.com/fermi-ad/redis-adapter/pull/108), [#100](https://github.com/fermi-ad/redis-pvxs-ioc/pull/100) |
| [ ] | Snapshot/live gap and timestamp/cursor conflation | Exact snapshot cursor, overlap deduplication, reconnect/trim/deletion discontinuity counters; retain legacy timestamps | #24, adapter #108, #100 |
| [ ] | Callback lifetime, shared moved data and worker failure | Immutable shared batches, retired callback fencing, safe last-owner destruction, sanitizer coverage | adapter #108, #100 |
| [ ] | Warning hysteresis suppresses major escalation | Major thresholds first; high/low escalation, clearing and nonfinite tests | #100 |
| [ ] | Wrong confirmation route and cache contamination | Separate displayed readback, confirmation observation and pending command; test distinct routes and metadata reload | #100 |
| [ ] | Stale/repeated matching observations | Stream-position fences and configurable absolute/relative tolerance; document observed readback versus causal acknowledgement | #100; remaining tolerance work in #4 |
| [ ] | Malformed/empty/oversized payloads and unsafe dereferences | Exact scalar size, valid arrays/encoding, safe copying, last-good preservation, 32 MiB default payload limit and byte-order tests | adapter #108, #100, #4 |
| [ ] | Silent alarm loss and blocking delivery | Bounded asynchronous publication, finite timeouts, current-state recovery reconciliation, failed/coalesced counters and outage tests | #4 |
| [ ] | Partial reload failure after staging/cutover | One coordinator, no staged external effects, commit-point rollback tests, old values/policy/reader preservation | #4, #100 |
| [ ] | Unaffected backends/runtimes rebuilt | Reuse unchanged adapters, runtimes and RPCs; metadata changes preserve subscriptions and readback | #4, #70 |
| [ ] | Blocking puts/RPC callbacks and unbounded queues | Serial per-canonical-PV writes shared by aliases; count and byte bounds; async RPC/alarm execution; overload/cancellation tests | #4, #70 |
| [ ] | Write ambiguity and cancellation semantics | No automatic retry after ambiguous acceptance; queued cancellation, authorization recheck at dispatch, finite total deadline, reload cancellation | #4 |
| [ ] | Unknown/duplicate keys, narrowing and nonfinite configuration | schema_version 1, compatible omitted version, strict semantic validation and machine-readable checks | #2 |
| [ ] | Control limits mistaken for enforcement | Advisory default; explicit opt-in write-range enforcement and tests | #2, #4 |
| [ ] | Names and reload differences not centrally modeled | One reservation model for canonical names, aliases, admin and reflected RPC endpoints; offline --diff-config and restart reasons | #3, #66, #70 |
| [ ] | PING-only health, misleading fallback quality, stopped producers | Invalid fallback until valid data, per-reader progress/source age/freshness, invalid/coalesced counters and local readiness | #100, #4 |
| [ ] | Pending work and backend outcomes not observable | Structured queue, operation outcome/latency, alarm delivery, RPC and reload diagnostics | #3, #4, #70 |
| [ ] | RPC availability can silently remove endpoints | Required by default; optional flag, finite discovery budget, retained unchanged services, background optional retry | #70 |
| [ ] | RPC names, arguments and schemas unchecked | Collision checks, strict numbers/unknown fields, method-specific defaults, reject streaming/recursive unsupported shapes, reflection-enabled fixture | #70 |
| [ ] | ChannelFinder HTTP lacks budgets and follows redirects | Connect/total timeouts, bounded responses, redirects opt-in; one-shot tool remains explicitly scoped | #27 |
| [ ] | Discovery tied to legacy product and incomplete catalog | Native RecCeiver registration for actual registry, hot reload/recovery, bounded worker and real catalog/PVA acceptance | [#102](https://github.com/fermi-ad/redis-pvxs-ioc/pull/102), #68 |
| [ ] | Non-root packaging, secret inputs and audit ambiguity | Secret files, restrictive ACF deployment, documented UID override, bounded peer/audit bookkeeping; separate authorization from backend success | #4 |
| [ ] | ACF monitor revocation/watcher recovery need routine qualification | CI allow/deny, live monitor revocation, watcher recovery and audit-bound tests | #4 |
| [ ] | Mandatory optional libraries and undocumented Base extension | Independent gRPC/ChannelFinder flags, explicit disabled-feature errors, asRefreshHag configure/link check and ownership documentation | [#101](https://github.com/fermi-ad/redis-pvxs-ioc/pull/101), #2 |
| [ ] | Native PVA startup timeout | Remove duplicate libevent linkage; real native server/client and access tests on all three hosted architectures | #101 |
| [ ] | Legacy support-module expansion dominates checkout/builds | Remove implementation, overlays, scripts, 14 submodules and vendored PCRE; preserve history and migration path | #68; retire #29–#46 after merge |
| [ ] | Imaging lifecycle and acceptance incomplete | Rework NTNDArray PR: ten numeric types, 1–3 dimensions, Mono/RGB, exact pixels, envelope compatibility, invalid recovery and frame-gap counters | #97 |
| [ ] | Throughput measured only at ACF mailbox | Whole Redis/PVA scalar/array/image/client/reload/backend-delay sweep with throughput, p95/p99, queues and memory | #4; qualification below |
| [ ] | CI omits complete integration and ownership/concurrency gates | Unit, Redis/PVA, access E2E, reflection RPC, imaging and sanitizer jobs; hosted untrusted PR execution | #101; qualification below |
| [ ] | Mutable build inputs and promotion before validation | Pin images/packages/actions, retain SBOM/provenance/logs/digests, validate before stable/latest, full SemVer prereleases | #67; release qualification |
| [ ] | Stale image references, support policy and feature docs | Synchronize README/config/security/image examples/support/migration material; refresh remote smoke tags; explicit local-image override | #67; release preparation |
| [ ] | No representative soak or rollback evidence | Both Linux architectures: 24-hour isolated soak with reload/outage injection; previous-stable configuration rollback | qualification below |

## Operating contracts

- One active write per canonical PV; aliases share the same FIFO. Defaults:
  **16 queued writes per PV**, **64 MiB aggregate queued writes**, and **32 MiB
  arrays/frames**. Every queue has count and byte bounds with documented overrides.
- Retain the 250 ms default confirmation wait; add a separate finite total
  operation deadline. Cancellation cannot undo an already accepted command.
- Process received samples for confirmation and alarm semantics before display
  coalescing. Alarm recovery reconciles current state; durable preservation of
  every transition is not promised.
- Discovery requires no manual catalog synchronization. Static catalog replacement
  follows successful activation; rejected staging preserves the previous session.
- Linux amd64/arm64 images and native macOS arm64 development builds are required.

## v0.9.0 qualification

- [ ] Hosted ubuntu-24.04, ubuntu-24.04-arm and macos-14 unit/integration runs.
- [ ] Deterministic 600-frame 1920×1080 Mono8 test at 10 fps, exact pixels and zero unexplained gaps.
- [ ] Published scalar/array/imaging/fan-out/reload/backend-delay capacity sweep.
- [ ] 24-hour isolated soak on Linux amd64 and arm64 with bounded resources.
- [ ] ACF, RPC, Redis reconnect/trim/deletion, imaging overload and recovery tests.
- [ ] Configuration rollback to the previous stable image in isolated environments.
- [ ] Trusted amd64 build/promotion on adlinux3 and native hosted arm64 build;
      assemble the manifest only from matching validated revisions.
- [ ] Pinned base images/package snapshots/actions; dependency inventory, SBOM,
      provenance, logs and image digests retained with the candidate.
- [ ] Stable/latest tags promoted only after validation; prereleases never update latest.
- [ ] Documentation and immutable deployment examples agree with the released behavior.

Production fleet rollout is a separate deployment task.

## After v0.9.0: v1.0 RC

| Done | Deferred finding/program item | Completion contract |
| --- | --- | --- |
| [ ] | Redis-backed definitions (#15) | Reuse validated model; immutable versioned snapshots, atomic activation, ownership/revision authorization and last-good/file recovery |
| [ ] | Transport identity/security (#47) | Qualify SecurePVA/certificates, Redis TLS and authenticated gRPC; publish supported trust and migration models |
| [ ] | Timestamp/order and causal command evolution (#24) | Explicit opt-in wire contracts separating acquisition timestamps from Redis ordering and supporting producer command correlation |
| [ ] | Compatibility/maintenance contract | Supported platform/dependency matrix, schema evolution, security-fix policy and maintainer handoff |
| [ ] | RC qualification | Full suite/rollback, seven-day representative soak, release-blocker resolution and v1.0.0-rc.1 evidence package |

Additional normative types (#17–#21) need an identified consumer and acceptance
criteria. Conventional IOC hosting, support-module expansion and a CA facade
(#13) do not block the RC and are not core product work.
