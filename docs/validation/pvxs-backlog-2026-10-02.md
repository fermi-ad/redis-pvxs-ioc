# Bounded PVXS listener backlog — 2 October 2026

Linux amd64 measurements support a fixed backlog of 32. It passed every fresh
16-, 32- and 64-context burst without accept-queue overflow. Backlog 16 covered
16/32 contexts but failed six of twenty 64-context bursts.

Each cell below is successful bursts out of 20, followed by total
`ListenOverflows` for that group. `ListenDrops` matched overflow counts.

| Backlog | 16 contexts | 32 contexts | 64 contexts |
| --- | --- | --- | --- |
| 4 | 4/20; 62 overflows | 2/20; 120 overflows | 0/20; 397 overflows |
| 16 | 20/20; 0 overflows | 20/20; 0 overflows | 14/20; 54 overflows |
| 32 | 20/20; 0 overflows | 20/20; 0 overflows | 20/20; 0 overflows |

The 180 runs were interleaved across variants and client counts. Every case used
fresh private Redis/IOC/client containers, an internal network with no host
ports, the unchanged burst client from `0eb4e76`, and its unchanged five-second
initial-monitor deadline. Listener `Send-Q` was checked in every case. There
were no OOM kills.

The runtime IOC is the PR #97 development image at `47626c7`, embedded version
0.8.2. The PVXS variants use current upstream `a9f8b7b` with only the listener
constant changed; candidate local commit `587ec7c` uses 32. The exact-source
archive is `b3c7b2dbb2c459eedde05be7de822beca2dbc2726d5cc4044f22628efe90883d`.
The builder/runtime input image identities and all variant identities are
retained in [the raw record](pvxs-backlog-2026-10-02.json).

All 19 IOC CTest cases passed for each variant. The backlog-32 PVXS core run
passed all 25 suites and 2490 assertions.

The original final inventory assertion flagged disappearance of two concurrent
access-test fixture containers. A separate acceptance review confirmed that all
six persistent pre-existing services remained running with creation/start times
before this validation, and that all containers/network owned by this comparison
were removed. Both the initial failure and the acceptance review are retained.

Full evidence is packaged on adlinux3 at
`/mnt/newdrive/derekste/pvxs-backlog-bounded-20261002-evidence.tar.gz`,
SHA256 `8ef87bf328af9f2fef2ba9b822db8a1d59f6254e491b33f11c4cb9ebcef02742`.

This is development evidence for [IOC #122](https://github.com/fermi-ad/redis-pvxs-ioc/issues/122).
The bounded change is prepared locally; [PVXS #229](https://github.com/epics-base/pvxs/pull/229)
still contains the earlier SOMAXCONN proposal. Upstream acceptance, the reviewed
pin, and burst/outage/reconnect checks on the final merged candidate remain
release gates.
