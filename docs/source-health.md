# Redis Source Health

`SYS:<instance>:source:status` describes each active canonical PV read route and
each distinct confirmation route. Aliases reuse the same row and reader. A shared
read/confirmation route has role `read-confirm` and one reader. Scalar, array and
NTNDArray sources use the same status contract; source diagnostics do not alter
the Redis payload or timestamp encoding.

## Readiness and freshness

`ready` and the NTScalar `SYS:<instance>:ready` are true exactly when every
required Redis source is ready. The structure reports active `generation`,
`total`, `required`, `readySources`, and `unreadyRequired`. An empty required set
is ready. RPC, discovery, access policy, write destinations and alarm delivery
are reported by their own diagnostics; this is a Redis data-source gate.

A source is ready when its owned reader is active and connected, its key is not
known missing/wrong-type, and the latest source sample is valid in the current
reader epoch and within its configured freshness deadline. A read rejection
makes it unready until a valid sample is observed after that rejection. Inspection
permission is separate: normal valid XREAD delivery remains ready when XINFO is
denied or disabled. This permits existing read ACLs and configurations to keep
working while exposing the lost continuity evidence.

Rejection recovery uses immutable read evidence captured with each batch before
callback queueing. A delayed valid callback can update retained data but cannot
acknowledge an ACL rejection that happened after its successful read. Callback
receipt time and a fresh poll of the mutable rejection counter are not used to
establish that ordering.

`source_health.required` defaults to true and `stale_after_ms` defaults to zero.
Zero declares no cadence: a valid idle source never becomes stale merely because
it is quiet. Missing initial data is unready, retains `initial` fallback with
zero source time, and is not labeled stale. Age starts when a valid snapshot or
update is received locally; an old snapshot's acquisition age is not known from
that observation. A configured deadline uses the steady clock, not wall time or
the Redis ID's legacy timestamp interpretation. Health refresh runs once per
second and after successful reload, so it is not a subsecond deadline service.

Failures retain the last-good scalar/array value or frame, and its source or
acquisition timestamp. The read PV receives an INVALID/UDF alarm. Confirmation
failures affect confirmation readiness and waiting commands without changing
displayed readback. Malformed data cannot restore readiness. Stream replacement
requires a valid sample in the current epoch; queued/executing old-epoch callbacks
are fenced before IOC mutation. Ordinary transport recovery preserves cursors
and epoch, so an already valid sample in that epoch can become ready again,
subject to any freshness deadline. Pending confirmations are failed when their
source epoch changes and accepted commands are never replayed.

| Failure | Expected behavior |
| --- | --- |
| One backend unavailable at startup | Start serving unaffected sources and diagnostics; failed snapshots use invalid fallback/zero source time and remain unready until a new valid observation arrives |
| Ordinary transport outage/restart | Keep exact cursors, source epoch and last-good values; unaffected backends keep serving; current-epoch validity and configured cadence determine readiness after reconnect |
| Observed deletion/lower-ID replacement | Advance epoch, retain last-good data/time with INVALID alarm, cancel affected confirmations and accept valid lower-ID data in the new epoch |
| Malformed payload/frame | Count invalid input, retain last-good data/time and require a valid decoded sample to recover |
| Configured freshness deadline | Retain value/time with INVALID alarm until a valid fresh sample or accepted health-policy change restores readiness |
| Invalid replacement configuration | Preserve active generation, reader ownership, counters, canonical/alias access and source readiness policy |

## Structure fields

Rows are ordered by canonical PV and role, and all `sources.*` arrays have equal
length. No credential fields are included.

| Fields | Meaning |
| --- | --- |
| `pv`, `backend`, `key`, `role` | Canonical route identity; role is `read`, `confirm`, or `read-confirm` |
| `state`, `error`, `required`, `ready` | `inactive`, `disconnected`, `missing`, `wrong-type`, `read-rejected`, `waiting`, `invalid`, `stale`, or `ready`; a concise failure reason |
| `valid`, `fresh`, `stale`, `staleAfterMs`, `lastValidAgeMs` | Sample validity in current epoch and local monotonic age; `-1` means no valid sample observed; zero deadline leaves cadence unspecified |
| `active`, `connected`, `hasData`, `streamKind` | Owned reader flags and observed key kind; connection is read-path evidence, not adapter command-pool ping |
| `inspection`, `inspected`, `readerProbeMs` | `disabled`, `pending`, `available`, `rejected`, or `unavailable`; inspection flags do not claim guaranteed continuity |
| `cursor`, `observedCursor` | Exact delivered and observed Redis stream IDs; observed can advance before callback processing; empty means unresolved future-only, not `0-0` |
| `epoch`, `lastValidCursor`, `lastValidEpoch` | Current fencing token and provenance of the retained last-good sample, independently of the served source timestamp |
| `lastReceivedAgeMs`, `callbacks`, `entries`, `callbackErrors` | Adapter callback receipt age (`-1` if none), batches/entries handed to callbacks, and callback exceptions; receipt can include malformed data |
| `readFailures`, `socketTimeouts`, `reconnects`, `readRejections` | Transport/read outcomes; idle NIL replies keep connectivity and do not count as socket timeouts |
| `streamResets`, `disappearances`, `retentionGaps` | Observed replacement/deletion and possible retained-history loss; a gap counter is evidence, not an exact missed-entry count |
| `inspectionFailures`, `inspectionRejections` | Inspection transport failures and server rejection, separate from XREAD failures |
| `invalidSamples`, `staleTransitions`, `staleCallbacks` | IOC decode failures, observed entries into configured stale state, and old-epoch callback batches rejected before mutation |

Counters belong to the current owned runtime/reader, persist through metadata,
alias and health-policy reloads, and reset when a route/backend is recreated.
They are not process-lifetime totals. The active generation's table excludes
rejected staged owners. The existing `backend:health` remains a command-pool
connection summary; it cannot establish source readiness.
Transport/inspection failure counters are observations of the associated reader
bucket and can appear in several source rows. Read rejection counters identify
the rejected registration; they are separate from those shared failures.

## Inspection policy and limits

The upstream adapter defaults `readerProbeMs` to zero. The IOC intentionally sets
the backend default `reader_probe_ms` to 1000 ms and applies it to owned readers.
The scheduler groups shared keys rather than sending one request per alias or
read/confirmation role. It batches at most 16 due metadata requests through an
existing pooled connection. Active keys skip unnecessary probes. Idle intervals
back off to at most eight times the configured minimum; denied/unsupported
inspection backs off for 60 seconds or is reconsidered after subscription or
reconnect restart. Scheduling, socket delays and backlog can extend observation
time. Zero disables inspection; 100–60000 ms selects another minimum interval.

Inspection requires read-only `XINFO STREAM FULL COUNT 1` access in addition to
the normal read/tail commands. It transfers one retained entry's field payload,
which can still be large. Missing streams and lower-ID replacement are recovered
when observed; delete/recreate between checks with equal/higher IDs cannot be
guaranteed detectable. Read-path wrong-type quarantine/repair remains operational
without XINFO. With inspection disabled/denied, quiet missing or lower-ID stream
replacement may remain undetected. Qualification is standalone Redis only.

## Validation and release qualification

`source_health_tests` uses private Redis for exact IDs with nontrivial sequences,
invalid/latest versus last-good samples, no-cadence and configured staleness,
deletion/lower-ID recreation, outage/retention evidence, distinct/shared
confirmation, denied inspection/read permission transitions and NTNDArray
acquisition-time retention. `source_health_e2e` uses two private Redis processes
and real PVA to verify partial transport outage, unaffected alias/read access,
unavailable-backend startup, recovery without cursor rewind, rejected and retained-policy reloads, aggregate
required/optional readiness, and confirmation cancellation across source epochs.

The IOC pins merged adapter `69bf18ec403c21ce396759172b24a91506283155`,
including the upstream source-recovery API changes. Native regressions do not
qualify a stable release, the configured inspection traffic, capacity or the
24-hour Linux soak. Repeat qualified image/capacity/soak gates against this
merged adapter pin and the final candidate, with independent Instrumentation
review. None of these continuity fixes establishes a root cause for the
historical allocator/cache report.

This revision includes immutable batch-admission metadata for recovery after
ACL read rejection. Delayed scalar or NDArray callbacks cannot acknowledge a
later read rejection. Final release qualification remains required.

Use a private loopback Redis and identical immutable IOC image/source for each
inspection comparison. Retain revision, adapter pin, image identity, Redis
version, key count, payload sizes, duration, configured interval, CPU/RSS,
`INFO stats` `total_net_output_bytes`, `INFO commandstats` `cmdstat_xinfo.calls`,
source diagnostics and PVA latency/update counts. Measure startup separately.

1. Seed an idle NTNDArray stream with two retained 1 MiB entries and no producer.
   Compare 30-second steady windows with `reader_probe_ms: 0`, `1000`, and
   `5000`. Subtract start/end network and XINFO counters; retain raw counters as
   well. Counter reads add small traffic, so use the same sampling in every run.
2. Repeat with 256 independent idle scalar streams on one backend (`readers: 1`)
   for at least 60 seconds. Capture every source row, scheduler convergence,
   detection time when 16 selected keys are deleted, healthy-key PVA latency,
   CPU/RSS and aggregate XINFO/network rates. Repeat at the intended deployment
   key count/reader count; a batch bound does not bound total work across keys.
3. Repeat active scalar/array and 600 exact 1080p Mono8/10 fps capacity gates with
   default inspection enabled, including a producer pause long enough to enter
   idle probing. Compare the pre-probe baseline; prior published capacity
   evidence excludes this cost and cannot be reused as final qualification.

Report measured traffic and observation delay; do not label the minimum interval
a guaranteed detection latency or claim lossless throughput from these overhead
trials. The production policy decision must account for idle large frames and
many-key load before release.
