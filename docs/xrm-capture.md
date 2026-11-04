# XRM TCP capture

`xrm-proxy --capture-config FILE --demux-config FILE` enables an independent C++
TCP producer for the existing Redis raw stream. Capture, raw demultiplexing,
current conversion, and PVA inventory run independently. Capture can receive and
publish while PVA discovery is unavailable. Omitting `--capture-config` leaves
hardware acquisition sockets disabled, including port 4210.

The supported capture profile is sixteen packed int16 channels, one leading
32-byte ACQ4xx event-signature frame, and 16,383 ADC frames: exactly 524,288 bytes
per complete pulse. Acquisition must already be configured for this profile.
Capture does not send control commands or change acquisition settings. Other
channel counts, formats, lengths, SPAD layouts, and rates require separate
qualification; there is no general transport-rate guarantee.

```yaml
schema_version: 1
source_stream: '{XRM}:acq2206-097-raw'
device:
  host: 10.200.8.253
  port: 4210
reconnect:
  initial_ms: 1000
  max_ms: 30000
queue_limit_bytes: 33554432
```

`source_stream` must match a source in the demux configuration. That source must
declare `format: acq4xx-es-int16-le`, sixteen channels,
`samples_per_channel: 16383`, `payload_field: '_'`, and explicit positive
`input_retention` equal to its output `retention`. The ICM profile uses 1,000 for
both retention settings. Its timestamp mode must be `absent`: capture cannot
supply hardware acquisition-time metadata. Redis endpoint, database, connection and command
deadlines, geometry, payload field, and retention come from that demux binding;
capture does not create another interpretation of these fields or change its
fingerprint or checkpoint. See [demultiplexing](xrm-demux.md) and the
[event format](xrm-event-format.md).

`device.host` must be a numeric unicast IPv4 address and must equal the runtime
`--ip`. The TCP port is explicit, from 1 through 65,535; the live XRM profile uses
4210. `reconnect` and `queue_limit_bytes` are optional with the defaults shown.
Reconnect intervals are bounded to 1–30,000 ms initially and up to 300,000 ms
maximum, with the maximum at least the initial interval. The queue accepts from
one complete pulse through 64 MiB; a nonintegral pulse remainder is unused.
Unknown fields, duplicate keys, multiple YAML documents, and configuration
larger than 1 MiB are rejected. The derived capture lease key must be disjoint
from every declared source, checkpoint, waveform, and current-conversion key.

Validate both files offline before deployment:

```sh
xrm-proxy --check-capture-config /etc/xrm-proxy/capture.yaml \
  --demux-config /etc/xrm-proxy/demux.yaml
```

This mode needs no `--ip` or manifest, opens no network connection, acquires no
manifest lock, and starts no worker. It can run in the exact deployment image
with networking disabled. Runtime capture requires `--demux-config` and rejects
`--once` or `--inventory`. An invalid capture file or host mismatch logs
`severity=ERROR event=startup_failed`; valid demux and PVA inventory continue.
Static configuration corrections require a sidecar restart.

## Framing and reconnects

The socket reader accepts arbitrary TCP segmentation and coalescing. It searches
for an event prefix with bounded storage and validates every complete pulse
using the shared event decoder: all four magic words must be `0xaa55f154`, both
counter pairs must agree, and an embedded recognizable event prefix is rejected.
Only the complete original bytes enter the queue. Initial or recovery search
discards unrecognized bytes, retaining only the possible prefix tail; two pulse
lengths of unsuccessful search latch a visible capture fault. A persistent
unsupported stream therefore requires correction and restart, rather than
indefinite automatic reconnects. No partial waveform is published.

EOF and socket errors immediately log `severity=ERROR event=disconnect` with
endpoint and reason. Partial assembly is discarded before a new connection
generation. Retry logs include the actual delay; the defaults double from 1 to
2, 4, 8, 16, and 30 seconds. Backoff resets and `event=recovery` is reported only
after a validated complete pulse. Ordinary silence does not cause a disconnect
or framing fault. Connection attempts have a two-second deadline; socket polling
and shutdown checks are bounded to 50 ms. Shutdown also wakes queue and retry
waits immediately.

## Redis failure and ownership

A dedicated Redis writer drains a bounded queue of complete pulses while the
socket reader continues receiving. The default queue holds 64 complete pulses
(32 MiB); the writer can additionally hold one in-flight pulse, and the reader
holds at most one pulse under assembly. When full, the queue drops its oldest
queued complete pulse, never the in-flight pulse, and increases `localDrops`.
Redis failures retain known-unpublished in-flight data and keep draining the
healthy hardware socket. They do not trigger hardware reconnection. The queue
is volatile; shutdown discards and counts queued/in-flight unpublished pulses.

Before opening a hardware connection, capture must acquire the Redis string
lease `<source_stream>:capture:lease:v1`. Each process has an independent token.
The lease expires after five seconds and is renewed every second. Local expiry
uses claim-request start with a conservative margin, not reply-arrival time.
Contention prevents capture from opening a new hardware socket or publishing;
loss of a lease leaves an already healthy socket draining into its bounded
queue. Every publish checks token ownership and unexpired TTL atomically in the
same Lua invocation as `XADD`. A Redis restart requires reacquisition before any
further publication. These tokens fence cooperating C++ producers; an old Python
producer does not honor this lease and must be stopped before handoff.

The writer uses Redis-generated IDs and exact `MAXLEN` retention from the
binding. Each raw entry has only the existing binary `_` field. There are no
additional raw fields, latest-value hashes, journals, or fabricated acquisition
timestamps. The unknown acquisition-time contract remains unchanged through
the existing demux/current lanes.

A complete dispatched `XADD` with a lost or invalid reply has an uncertain
outcome: Redis may have committed it. Capture drops that in-flight pulse,
increments `uncertainWrites`, and never internally replays it. A failure before
the complete command was dispatched, or a known fenced rejection, can retain
the pulse for a later attempt. The `published` counter counts confirmed replies;
it cannot resolve an uncertain delivery. This is not an exactly-once claim.

## Diagnostics and isolated checks

Capture logs state transitions and status every five seconds independently of
inventory timing. Status includes `received` validated complete pulses,
confirmed `published` writes, reconnect attempts, `queueDepth`, `queueBytes`,
`queueHighWater`, `localDrops`, framing `rejects`, `discardedBytes`,
`redisFailures`, `leaseState`, and `uncertainWrites`. `lastReceiveAgeMs` and
`lastPublishAgeMs` measure time since the last validated pulse and confirmed
publish; `-1` means none has occurred in this process. Queue depth excludes the
one in-flight pulse. Counts describe local observations only and do not infer
upstream lost pulses, event-counter clock semantics, or sustained device rate.
Capture log writes are bounded and best effort through a private nonblocking
descriptor; `logDrops` counts unavailable or full logging sinks. A stalled log
consumer cannot hold the capture worker's shutdown joins.

`xrm_capture_tests` covers strict parsing, arbitrary segmentation, initial junk,
generation reset, invalid headers, embedded event signatures, and bounded fault
latching. `xrm_capture_integration` uses a private loopback Redis and synthetic
TCP device to exercise the process boundary, reconnects, ownership, uncertain
acknowledgement handling, and queue pressure without hardware or deployment
Redis. Live acceptance must still verify the named hardware and deployment.
