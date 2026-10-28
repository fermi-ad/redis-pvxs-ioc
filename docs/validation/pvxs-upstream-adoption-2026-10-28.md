# Official PVXS adoption — 28 October 2026

Development acceptance passed for official PVXS
`96345ddbbad50144b9f4464b9a24a2e490bbbf45` on Linux amd64. This is the focused
adoption step in [PR #160](https://github.com/fermi-ad/redis-pvxs-ioc/pull/160),
targeting `dev/v1.0.0`; it is not a qualified 1.0 release candidate.

## Exact inputs

- Tested IOC source: `87c2d03fd0341c19f226931613b7995d54172443`, embedded version
  `0.9.0`. The follow-up changes correct one test assertion and add documentation
  and evidence; IOC runtime sources, configuration and dependency pins are unchanged.
- Official PVXS remote: `https://github.com/epics-base/pvxs.git`, exact revision
  `96345ddbbad50144b9f4464b9a24a2e490bbbf45`. Upstream applied the bounded backlog
  with changes: 32 on non-RTOS targets, 4 on RTEMS/VxWorks. Tag `1.5.3` predates it.
- Unchanged Base/adapter/YAML pins and their remotes are recorded in the
  [machine-readable record](pvxs-upstream-adoption-2026-10-28.json).
- Local runtime image ID:
  `sha256:529de653b1a2b12d5e5acff16bd7f0f938f87765ba3c0fea0bc715ff359a74c8`.
  This image was not pushed to a registry and is not a promoted release digest.

The adlinux3 build used a fresh checkout, recursive core submodule initialization,
and `docker build --no-cache --platform linux/amd64` with the tested version and
revision build arguments. No personal PVXS source substitution was used. The
EPICS Base fork remains a separate dependency-exit gate.

## Results

| Check | Result |
| --- | --- |
| Release/qualification tool tests | 117 passed |
| Clean full-feature image CTest | 35/35 passed |
| Debug minimal CTest, optional integrations disabled | 30/30 passed |
| Debug ASan/UBSan CTest, optional integrations enabled | 35/35 passed |
| Image identity, license inventory, non-root and secret-mount checks | Passed |
| Isolated image smoke and ACF tests | Passed |
| Corrected RPC counter scenario | 20 repetitions passed, then 35/35 full CTests |
| Fresh simultaneous-client bursts | 20/20 each at 16, 32 and 64 contexts |
| Linux listener overflow/drop deltas | Zero in every accepted burst |

The CTest/image suites include source-health, partial outage/recovery,
aliases, reload/resource lifetime, access denial and write-confirmation coverage.
Native test commands, source identities and full logs are retained in the raw
archive. Sanitizers use `-fsanitize=address,undefined -fno-omit-frame-pointer`,
with leak detection and halt-on-undefined-behavior enabled.

The burst client is the unchanged simultaneous-context source from
`0eb4e76fe2ee5260e88feabbc922a40852f9e195`, rebuilt against the candidate
libraries. Its source SHA256 is
`50f17b4ba05dd572724cd8ebfa81c453e02b4031c85a4156e87090169139c003`;
the rebuilt executable SHA256 is
`2ab98a4781c40c9049f2743f46b56bd31431dd0b680ff46b25afcf76a69cfd2e`.
The current sequential steady-state capacity benchmark was not used as startup
evidence.

Twenty rounds interleaved 16/32/64 independent contexts. Every case used fresh
Redis/IOC/client containers on a task-owned internal network with no host ports,
the unchanged five-second initial-monitor deadline, and a listener Send-Q of 32.
All transfers were valid; there were no OOM kills. The five-second threshold is
a fixture acceptance bound, not an adopted product startup guarantee.

## CI correction and evidence review

The initial GitHub native matrix passed, including Linux ASan/UBSan. Its image
job failed an existing RPC test assertion that assumed the cumulative denial
counter was exactly one. An earlier legitimate revocation can also increment
that counter. The test now snapshots the total immediately before the final
revocation and requires exactly one additional denial. Audit ordering, error
and backend-exclusion assertions are preserved; no access-control runtime code
is changed. Required CI and independent review still apply to the final PR head.

The first burst run passed all 60 functional cases but failed its broad final
container-inventory assertion when a concurrent, task-owned RPC test container
completed. That result is retained as a failed overall acceptance run. A separate
60-case acceptance run after the auxiliary tests finished passed every check,
including preservation of all pre-existing container IDs. The five original
persistent services retained their image IDs, start times and running state.
All task-owned test containers and networks were removed; validation images and
evidence are retained for reproduction.

Full evidence is retained on adlinux3 at
`/mnt/newdrive/derekste/official-pvxs-20261028/evidence.tar.gz`, SHA256
`7bde2bec288bd8a36b56ccffcd77b9b25e2471a6ac950fb755cf470cd67cc937`.
The archive includes the accepted and preliminary runs, per-case logs and network
counters, exact fixture/build commands, source/image identities, native logs,
the initial CI failure and cleanup review. The linked JSON retains compact
per-case results and provenance without requiring archive access.

Keep [#122](https://github.com/fermi-ad/redis-pvxs-ioc/issues/122) and
[#138](https://github.com/fermi-ad/redis-pvxs-ioc/issues/138) open through their
remaining acceptance. [#156](https://github.com/fermi-ad/redis-pvxs-ioc/issues/156)
still owns qualification of the exact merged source and immutable release image.
This work changes no version, platform/soak policy or production deployment.
