# Candidate qualification for v0.9.0

This workflow and proof contract define the candidate qualification procedure.
The tooling alone is not evidence that a candidate, development image, or release
has passed qualification. The collector requires exact final `VERSION=0.9.0`,
all merged feature tools, successful CI on that same main revision, an attested
candidate digest, and a published qualified v0.8.2 rollback release. The final
runtime, dependency pins and representative policy choices require independent
Instrumentation review and protected-main merge before execution.
No final soak or 600-frame acceptance is run as part of tooling unit tests.

## Run and retain evidence

1. Merge the complete reviewed runtime/dependency/release source into `main`,
   with final VERSION 0.9.0 and its changelog. Review the representative workload,
   pacing and resource choices below in the same PR; dispatch has no approval
   checkbox, check assertions, duration override, image override, or existing
   container/project input. Subsequent runtime or policy changes require a new
   same-source candidate and CI runs.
2. Publish and qualify v0.8.2 first using its existing candidate/release process.
   Retain its successful candidate run, immutable image, `candidate.json`, source
   inventory, attestations, and `release-evidence.tar.gz` in its published release.
   A version string or mutable `v0.8.2` registry tag alone is insufficient.
3. Obtain successful **Native Redis/PVA validation**, **Validate redis-pvxs-ioc
   image** (on adlinux3), and **Publish redis-pvxs-ioc candidate image** runs on the
   exact final main revision. PR merge refs, forks, failed matrix jobs, omitted
   tests, stale rerun attempts and another revision are rejected.
4. Dispatch **Qualify redis-pvxs-ioc candidate image** from that same main revision
   with those three numeric run IDs. The read-only workflow runs on adlinux3 and
   is bounded to 26 hours, including setup and the measured 24-hour soak. It
   builds a local helper image using the exact candidate source, a digest-pinned
   isolated BuildKit builder limited to four CPUs/4 GiB, and a narrow patch that
   adds steady-clock send timing to the existing exact-pixel frame client.
   The qualified immutable IOC image is pulled and never rebuilt or retagged.
5. Retain `release-qualification-<run>-<attempt>`. `qualification.json` is written
   only after actual collection, rollback, private runtime cleanup and complete
   proof verification. Failure evidence may be uploaded without this record.
   Promotion also requires the entire qualification workflow to finish
   successfully, including artifact upload. A record from a later-failed run
   cannot authorize release.
6. Use the existing release workflow with the candidate run and qualification
   run IDs. It rechecks authenticated CI/run/attempt/source identities and the
   complete bundle before its existing image/tag/smoke/manifest-copy gates. It
   promotes the exact qualified digest without rebuilding and retains all
   evidence in the GitHub Release archive.

GitHub evidence acquisition is read-only. The collector does not publish,
dispatch another workflow, target production containers, mount a Docker socket
into the helper, expose host ports, or modify stable registry tags. It creates
private IDs, an internal network and fixture-only config/ACF files under a new
mode-0700 evidence directory. Every Docker mutation rechecks its owned ID/label.
Container/network/helper-image cleanup verifies literal scope owner labels.
Buildx supports an environment owner marker rather than a driver label: its
private instance, exact node name/driver/options and daemon marker are checked
before removing its own builder and state volume without `--keep-state`.
Planned exact identities are journaled before create/build calls, and fsynced
atomic journal replacement preserves the prior readable plan if a post-create
update is interrupted. Lost CLI returns are reconciled only against these
planned names and validated owner identities; collisions remain untouched.
Collector stop errors still attempt private-scope cleanup and retain diagnostics
without masking the original error on host Python 3.9. The workflow's always
cleanup step provides another bounded attempt. A successfully closed scope is
left byte-identical so its already-hashed evidence remains valid.

The IOC is limited to one CPU/512 MiB, Redis to half a CPU/128 MiB with a 64 MiB
no-eviction data limit, and the helper to two CPUs/2 GiB. Runtime roots are read
only, capabilities are dropped, and processes use the runner's non-root UID.

## Coverage and proof boundary

| Gate | Actual retained evidence | Limits and review focus |
| --- | --- | --- |
| Final merged source | Candidate/qualification head SHA, main ancestry, exact VERSION from GitHub, pinned recursive source inventory and full SBOM/provenance/manifest hashes | Protected-main/code-owner review establishes acceptance of merged code and upstream dependency pins; attestations do not establish code review by themselves. |
| Native macOS and minimal | Same-SHA native matrix jobs, successful configure/test/real RecCeiver steps, raw logs containing the expected passed runtime/image/discovery tests and integrations OFF | Linux ARM is informational. The macOS and Linux x64 jobs are required. |
| Sanitizers and full feature | Successful x64 ASan/UBSan job with integrations ON, raw passed endpoint RPC, RPC hardening/recovery, ChannelFinder HTTP, source-health, imaging and discovery test evidence | `source_health_e2e` is the registered CTest name; `channelfinder_http_tests` is required for full-feature HTTP coverage. Required names are cross-checked against the actual candidate's CMake source, not only synthetic logs. Skipped optional features or omitted tests fail. |
| Redis/PVA and discovery/catalog | Candidate smoke/access checks and native Redis/PVA test logs; real RecCeiver using its ChannelFinder processor in the existing in-memory client acceptance fixture | This reuses the accepted native receiver procedure; it does not claim a production ChannelFinder deployment was qualified. |

Each required native matrix job uploads its bounded raw qualification log as an
immutable, run/attempt/matrix-named artifact. Qualification authenticates that
artifact's workflow run and source SHA, verifies its archive digest, and retains
the extracted log in the proof bundle. It does not depend on the job-log download
endpoint, which is unavailable to this repository's workflow installation token.
| 600-frame imaging | Existing external C++ producer/PVA monitor validates all 1,244,160,000 pixels, ordered unique IDs and exact acquisition timestamps; zero gaps/duplicates; all 600 steady-clock send times | Exact 1920x1080 Mono8, nominal 10 fps, existing 90-second whole-run bound. Source cursors are ordering/acquisition metadata, not an independently measured producer clock. |
| Capacity | Scalar 1k/10k Hz, 4096-element array 100/2500 Hz, 16-client fan-out 100/1000/5000 Hz; exact-content client reports with actual rate/latency/loss/coalescing/queue/RSS; imaging report; actual reload and 250 ms Redis-delay actions while capacity clients run | All six axes are required. Lossy exploratory points remain visible and never become lossless capacity claims. This is steady fan-out, not a connection-burst benchmark. |
| 24-hour representative amd64 soak | Actual monotonic start/end, 30-second CPU/RSS/Docker/PVA/producer observations; exact source-cursor/content checks, continuing 100 Hz scalar + 4096-element array; every hour a full 600-frame transfer and 16-client 1000 Hz run; actual SIGHUP reload, backend delay and paused-Redis outage/recovery | Unobserved intervals over 45 seconds, unexplained write/probe/read failures, missing hourly work, non-progressing values, OOM/restarts, invalid readiness or resource-bound changes fail. The profile is the 0.9.0 release fixture, not universal fleet capacity. |
| Qualified 0.8.2 rollback | Published non-prerelease release/tag/candidate provenance, saved compatible JSON/YAML and ACF bytes, both images' actual check-config exits, running binary/digest/platform/inspect evidence, actual exact PVA source content before candidate and after restore | Restores original saved bytes and recreates only the owned IOC. A version label, file hash without runtime content, or old image with new configuration is insufficient. |

The checked-in [qualification policy](qualification-policy.json) is the 0.9.0
acceptance profile submitted for independent Instrumentation approval in this
release PR. It allows
0.1% average cadence variation (9.99–10.01 Hz) measured across all 599 producer
steady-clock intervals, with the existing 90-second total bound. It records
every send time, interval and maximum scheduling lateness; there is no invented
100 ms maximum-lateness acceptance requirement. The roadmap's nominal 10 fps
requirement did not specify this tolerance. Approval of this release PR accepts
the choice explicitly. The 64 MiB final RSS-growth limit is likewise a release
acceptance choice; the full trace/peaks remain available for review.

Active sources retain the IOC's default 1000 ms inspection policy. The collector
does not disable inspection or change payload semantics to ease qualification.
Completed image/capacity routes are removed by real observed config reloads,
and only their owned Redis keys are deleted, so idle multi-megabyte snapshots do
not accumulate for 24 hours. Imaging and independent multi-consumer traffic recur
in every hourly interval alongside the continuous scalar/array workload. Review
this duty cycle and fixture against the intended production portfolio before
main merge; passing it would not prove a continuous 24-hour 1080p deployment.

Fault exemptions cover only the precisely timed owned Redis delay/outage
windows. Producer failures are cumulative and must not increase in healthy
intervals; PVA content must continue progressing. Each failed or ambiguous
producer request retains its start/end monotonic times, attempt number and
success/failure counters. A failed request is attributable only if its bounded
interval (at most four seconds, with one-second socket timeouts) intersects the
actual recorded pause operation. This permits a request already in flight when
the fault begins, but never excuses a later failure elsewhere in the same
30-second observation interval. Scheduling gaps above the fixture's two-second
source-freshness window and requests exceeding their bound fail independently;
catch-up traffic cannot hide those gaps. Event-boundary producer snapshots must
match the precise failure ledger. The verifier checks observed PVA progression
and source counters in the healthy subsegments before and after each fault.
The collector records actual
invalid readiness/retained last-good data during the outage and fresh valid
content after recovery. A reload must publish the observed next generation and
different config bytes while continuous sources stay valid. No percentage of
unexplained failures or stalls is accepted.

The hourly fan-out gate checks measured `produced_hz`, not only the requested
1000 Hz label. The release acceptance interval is 999–1002 Hz. Its small
asymmetry includes the existing capacity client's sample-count/span convention
(10000 samples span 9999 scheduled intervals). Slower exploratory capacity
points remain in the curve but cannot replace this representative hourly load.
The 30-second PVA observations establish progress at that observation resolution;
they are not a claim that every scalar/array monitor update was delivered or
that sub-observation transients cannot occur. Exact delivery is separately
checked by the imaging and hourly fan-out clients.

No manual boolean attestation closes any gate. This procedure automates the bounded
private actions needed for this fixture. Existing manual production cutover and
rollback procedures remain separate operational tasks. Operator judgment still
matters when reviewing the retained traces and whether this profile represents
the planned deployment, but operator-typed checks cannot manufacture the
qualification record.

## Bundle and promotion validation

`qualification.json` binds every regular file by SHA-256, including `proof.json`,
the exact source policy, candidate attestations, authenticated run/job metadata,
raw CI logs, workload commands/reports/timings, observation journal, controlled
events, saved/restored config/ACF and rollback runtime evidence. The verifier
derives all required checks from these files. Missing, extra, changed, stale,
partial or mismatched evidence fails before writing or using a successful record.
It cross-checks measured UTC/monotonic duration with the eventual successful
GitHub run's authenticated elapsed interval. The same-run collector is trusted
reviewed code; file hashes are integrity checks, not independent attestations.

Before collection starts, the workflow authenticates and seals the exact GitHub
responses needed to validate its source, candidate, CI and published v0.8.2
rollback evidence. `proof.json` binds that closed endpoint set, its SHA-256,
source revision, run attempt and capture time. Post-soak verification has no
network fallback: a missing, extra, changed or stale response fails. This keeps
the mandatory 24-hour soak independent of the self-hosted job's 24-hour
[`GITHUB_TOKEN` refresh limit](https://docs.github.com/en/actions/concepts/security/github_token).
Release promotion separately reauthenticates the completed qualification run and
all external evidence live with a fresh token. Final artifact upload remains a
required fail-closed workflow step; an upload failure cannot authorize promotion.

Downloads are size-limited and extracted only after preflighting all member
names, duplicate paths, link/type collisions and expanded sizes. Absolute paths,
traversal, backslashes, symlinks, non-regular entries, duplicate JSON keys and
non-finite metrics are rejected. Bounds are 1024 files, 32 MiB per evidence file
and 128 MiB total. The runner retains compact reports, not image pixel payloads
or continuous console output. Registry credentials are in a separate private
Docker-config directory and are excluded from evidence.

Preparation validation consists of the deterministic positive/negative proof
fixtures (`python3 -m unittest discover -s tests -p '*_tools_test.py'`), the short
independent `qualification_probe_e2e` CTest, and the short private Redis-helper
check (`python3 tests/qualification_traffic_e2e.py --redis-server PATH`). The latter
owns a new ephemeral loopback Redis process, briefly pauses that process to
check actual bounded request attribution, and restores/stops it. These helper
checks do not start candidate qualification or create a passing release record.

This policy intentionally supports exact final 0.9.0 only. An RC cannot be
relabeled, and a later stable version cannot silently reuse the 24-hour/0.8.2
rollback policy. Longer 1.0 qualification and future rollback baselines require
their own reviewed policy changes.
