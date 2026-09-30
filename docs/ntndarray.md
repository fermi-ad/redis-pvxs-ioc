# Redis-backed NTNDArray

`kind: ntndarray` is the first richer normative-type adapter in
`redis-pvxs-ioc`. It serves an uncompressed numeric image from one Redis
Stream as `epics:nt/NTNDArray:1.0` without an AreaDetector process in the read
path.

## Configuration

```yaml
pvs:
  - name: GE1350:Image
    aliases:
      - SPIKE:CAM2:Pva1:Image
    kind: ntndarray
    read:
      backend: default
      key: ge1350
    max_frame_bytes: 33554432
```

The final stream key follows the normal RedisAdapter key convention. For
`base_key: spike` and `key: ge1350`, it is `{spike}:ge1350`.

NTNDArray entries are read-only. Configuration rejects `type`, `shape`,
`write`, `confirm`, `initial`, transforms, scalar metadata, and scalar alarms.
Aliases share the same runtime and Redis subscription. `max_frame_bytes` accepts
1–1073741824 bytes and defaults to 32 MiB. The effective frame cap is the smaller
of this setting and `limits.max_payload_bytes` (also 32 MiB by default).

## Stream contract

Each entry uses schema `fermi-ad/redis-adapter/ntndarray:1`. The Redis Stream
ID is the acquisition timestamp: the left side is POSIX milliseconds and the
right side is the nanosecond remainder within that millisecond. It is the
authoritative source for `NTNDArray.dataTimeStamp`.

Required fields:

| Field | Encoding |
| --- | --- |
| `_` | Binary, uncompressed pixel payload in little-endian element order |
| `schema` | `fermi-ad/redis-adapter/ntndarray:1` |
| `data_type` | Numeric element type, `0` through `9` |
| `shape` | JSON array with one to three positive dimensions, fastest first |
| `color_mode` | `mono`, `rgb1`, `rgb2`, or `rgb3` |
| `unique_id` | Signed NTNDArray-compatible 32-bit frame identifier |

Numeric element types match the AreaDetector/EPICS ordering:

| Value | Type | Value | Type |
| ---: | --- | ---: | --- |
| 0 | int8 | 5 | uint32 |
| 1 | uint8 | 6 | int64 |
| 2 | int16 | 7 | uint64 |
| 3 | uint16 | 8 | float32 |
| 4 | int32 | 9 | float64 |

Optional acquisition provenance fields are forward-compatible extensions:

| Field | Encoding |
| --- | --- |
| `time_source` | `unknown`, `host`, `camera_correlated`, `ptp`, or `external_trigger` |
| `time_uncertainty_ns` | Unsigned nanoseconds |
| `camera_timestamp` | Unsigned native camera-clock ticks |
| `camera_timestamp_hz` | Unsigned camera-clock ticks per second |

Consumers ignore unknown fields so producers can add later metadata without a
schema break. Recognized fields remain strictly validated. The version-1
contract intentionally omits a redundant payload timestamp and byte-order
field: the Stream ID is the timestamp and byte order is fixed.

## Validation and PVA behavior

The adapter validates the schema, Stream ID, data type, JSON dimensions, RGB
channel dimension, integer overflow, exact payload size, and
`max_frame_bytes`. It converts the little-endian payload on a big-endian host.

A valid entry populates the correct NTNDArray value-union member, dimensions,
empty codec, compressed and uncompressed sizes, unique ID, acquisition time,
and the standard numeric `ColorMode` NTAttribute. Numeric acquisition-time
attributes expose the Stream ID and optional provenance.

Before the first valid entry, the PV is present with an invalid alarm. A bad
entry retains the last good pixels and metadata, raises an invalid alarm, and
increments `SYS:<instance>:stats:ndarrayInvalidFrames`. The next valid entry
clears the alarm. Gaps in increasing frame IDs increment
`SYS:<instance>:stats:ndarraySkippedFrames`.

Frame-gap accounting compares IDs as wrapping 32-bit serial numbers. A small
forward gap across `INT32_MAX` to `INT32_MIN` counts its skipped IDs. Repeated
IDs and adjacent rollover count no gap. A backward/half-range-ambiguous change,
or a forward gap above the PV's `max_frame_gap` ceiling, establishes a new
baseline and increments `SYS:<instance>:stats:ndarrayDiscontinuities` instead
of adding an unbounded skipped-frame estimate. The ceiling defaults to 10,000
skipped IDs and accepts 0..2147483646; zero treats every non-adjacent jump as a
discontinuity. Set it to the largest plausible gap for the producer and workload.
Reloading this policy retains the PV and its current frame.

IDs alone cannot distinguish a producer restart from a plausible forward gap
within that ceiling. The discontinuity counter describes this accounting policy,
not proof of a restart; the skipped counter is not proof of transport loss.

Counters are unsigned 64-bit observations across active image runtimes; two
canonical PVs reading the same stream each count their observations, while aliases
share one runtime. Discarded staging does not change the live counters. A gap is
an observed forward ID difference, not proof of transport loss; producer skips and
malformed frames can contribute. Rollover/reset does not imply a huge gap.

Startup reads a snapshot, then subscribes after its exact opaque Redis ID. Each
runtime owns its subscription: discarding or retiring one image PV cannot remove
another PV's same-key reader. Unchanged image runtimes and monitors survive reload,
including rejected cutovers. A replacement fences an already-running callback
before it can post to a retired PV. Current source alarms use the same bounded
alarm publisher as other PVs.

Discovery advertises `epics:nt/NTNDArray:1.0`, and catalog type metadata identifies
`ntndarray` rather than a fixed scalar pixel type. The element type remains a
per-frame property of the versioned envelope.

## Repeatable transfer acceptance

The normal test suite runs a short deterministic external-IOC transfer. The same
checker can run the release imaging workload with a private Redis:

```sh
python3 scripts/run-with-redis.py --redis-server "$(command -v redis-server)" --timeout 90 -- \
  python3 tests/ndarray_transfer_e2e.py \
    --ioc build/redis-pvxs-ioc --client build/ndarray_transfer \
    --frames 600 --width 1920 --height 1080 --fps 10 \
    --report /tmp/ndarray-transfer-report.json
```

Use a new report path for every run. The checker validates every pixel, frame
order, dimensions, acquisition timestamp and alarm, with a 16-entry Redis history
and 16-entry PVA monitor queue. A gap, duplicate, corrupt pixel or disconnection
fails the run. Reports include arrival latency, pacing lateness and client peak
RSS; the harness also records the IOC's peak RSS on Linux. This test is one imaging
gate. Release qualification must run against the exact candidate image/digest and
also satisfy the capacity, outage, reload, soak and rollback gates in #99.

## Normative-type architecture

NTNDArray is a concrete vertical slice, not a special-purpose camera service.
The Redis route lifecycle, snapshot-plus-stream subscription, aliases, hot
replacement, access control, alarms, and administrative counters remain in the
generic runtime. The NTNDArray adapter owns only its versioned envelope
validation and normative-value construction.

Future normative types should follow the same boundary: a versioned Redis
envelope adapter and a focused builder plugged into the shared runtime. Common
pieces—Stream-ID handling, structured-field validation, union selection,
metadata attributes, and invalid-value policy—can then be extracted into the
normative-type framework as a second type proves each abstraction.
