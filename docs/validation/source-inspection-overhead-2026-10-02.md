# Source inspection overhead — development comparison, 2 October 2026

A healthy command connection cannot establish individual stream health. Source inspection makes missing/reset streams observable, but `XINFO STREAM FULL COUNT 1` transfers one retained payload. This experiment measures that idle cost before choosing the IOC default.

All six private Linux amd64 workloads passed: every required source stayed valid, connected, active and ready through warmup and three steady windows; first/last PVA payloads matched; disabled inspection issued zero XINFO; no new entries/callbacks appeared during the idle windows; no container was OOM-killed. Six preexisting stable containers kept their identifiers and start times. All twelve task containers and the private internal network were removed.

## Immutable inputs

- IOC development source: `8ce55fcc17d66090f7eac7888d6c3b49ac8a6edf` from [#128](https://github.com/fermi-ad/redis-pvxs-ioc/pull/128).
- Adapter development source: `94470f5918c4bb544312f32f0e4a09847a122572` from [adapter #111](https://github.com/fermi-ad/redis-adapter/pull/111), including batch-admission rejection evidence. These sources are not merged release inputs.
- Runtime image: `sha256:cce2f0233f458facdddc53e1d6bb031bf9229d5b22879808dbf12f240f54c793`.
- Retained builder: `sha256:96568ee65d50aa5ab06c9c3e707114e51a6dab4c922082cdaa295ca8e5972ac9`; dependency revisions, archive checksums and actual IOC/Base/PVXS/helper binary hashes are in the [machine-readable record](source-inspection-overhead-2026-10-02.json).
- Host: adlinux3.fnal.gov, Linux x86_64. This development image embeds VERSION `0.8.2`; it does not qualify a v0.9.0 release.

## Method

Fresh private standalone Redis 8.0.5/IOC containers were used for each probe setting. The actual pinned adapter wrote two valid, identical UInt8 arrays per stream with exact MAXLEN 2. Both workloads represent 1 MiB of logical data per generation and 2 MiB of retained payload, divided across one or 256 keys. The IOC serves ordinary UInt8 array PVs. One reader and one worker served required sources, with `stale_after_ms:0` (cadence unspecified), discovery disabled and no steady-state writes.

Startup observation is separate. Warmup was 10/16/80 seconds at probe 0/1000/5000 to reach the scheduler's idle backoff. Three nominal 30-second CPU windows followed. Redis byte/probe rates use their actual INFO-to-INFO elapsed intervals (approximately92–99 seconds total), including measurement scheduling. Raw byte deltas and the measured RESP size of the before-INFO response are retained; the table subtracts only that response. Ordinary Redis read/health traffic remains included.

CPU is actual IOC PID1 utime+stime as percent of one core, excluding helper/docker-exec CPU. RSS is actual PID1 VmRSS sampled approximately once per second. Public `inspected` is a per-source boolean; Redis commandstats supplies successful XINFO call totals, while actual PVA diagnostics verify coverage, configured policy and rejection/failure counters.

## Results

| Idle workload | reader_probe_ms | XINFO calls across 3 windows | Adjusted Redis output, KiB/s | IOC CPU, % of one core | IOC RSS, MiB | Startup observation, s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 1 × 1 MiB | 0 | 0 | 0.023 | 0.100–0.100 | 16.91–16.93 | 1.48 |
| 1 × 1 MiB | 1000 | 13 | 134.407 | 0.067–0.100 | 19.63–19.71 | 3.17 |
| 1 × 1 MiB | 5000 | 3 | 31.070 | 0.099–0.132 | 18.93–18.96 | 2.32 |
| 256 × 4 KiB | 0 | 0 | 0.023 | 0.200–0.264 | 19.54–19.56 | 2.35 |
| 256 × 4 KiB | 1000 | 3072 | 140.115 | 0.233–0.266 | 20.00–20.00 | 0.77 |
| 256 × 4 KiB | 5000 | 512 | 24.110 | 0.200–0.233 | 19.88–19.88 | 0.77 |

The 1000 ms setting costs approximately 134–140 KiB/s for these 1 MiB logical workloads, compared with approximately 24 B/s when inspection is disabled. At 5000 ms, measured output falls to 24–31 KiB/s. CPU remains below 0.27% of one core and RSS below 20.1 MiB in every measured window.

The 256-key/5000 ms trial has one 30-second window with zero probes; this is consistent with a 40-second nominal idle interval after eightfold backoff. Enabled trials inspect all keys during warmup and perform XINFO during the complete steady observation. These observations do not establish a detection deadline.

## Default-policy decision and limits

The draft IOC deliberately enables a 1000 ms default; adapter callers still default to disabled inspection. Instrumentation owners must accept the measured payload traffic or select an explicit opt-in/slower policy before release. Larger retained arrays can make the cost material. A slower interval trades traffic for delayed observation; disabled inspection leaves idle continuity evidence unavailable. Configured freshness is a separate receipt-age rule and cannot be inferred from payload timestamps or an unspecified producer cadence.

This is an idle development comparison. It does not establish imaging acceptance, sustained capacity, asymptotic key-count scaling, lossless throughput, maximum detection latency, an approved production operating point, reconnection behavior, or soak qualification. Final capacity, 600 exact 1080p Mono8 frames at 10 fps, 24-hour isolated soak and rollback must use the reviewed merged candidate embedding 0.9.0.

## Retained evidence

The full source-host archive is `/mnt/newdrive/derekste/source-health-overhead-20261002-evidence.tar.gz`, SHA256 `1ecdbac7203c3dd7c556f4c0e1f90621012f6702576f6dbb4e16a25272560ac2`. Its internal manifest was verified before packaging. It retains the frozen sources/recipe, build log, startup records, eighteen raw windows, process samples, actual PVA source/ready snapshots, commandstats, binary hashes and before/after service metadata. The public JSON retains every window's numerical metrics and source snapshots.
