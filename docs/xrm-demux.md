# Redis XRM waveform demultiplexing

`xrm-proxy --demux-config FILE` optionally runs an independent Redis worker
alongside PVA discovery. A demux configuration or Redis failure is reported
without stopping mirror discovery. The worker never connects to a digitizer,
opens an acquisition socket, changes controls, discovers Redis keys, or deletes
keys. Only explicitly configured source streams are read; only their exact
output and checkpoint keys are written. Without this option, no Redis worker
or Redis connection is created.

The supported input is one complete pulse per Redis entry, containing aligned
sample-major signed little-endian int16 ADC words. It is not a TCP framing or
SPAD decoder. Configure the actual producer's channel order, pulse length,
calibration and timestamp fields before enabling it. Source declarations are
an operator contract: same-sized SPAD bytes falsely declared as ADC cannot be
identified from their binary values. Unknown fields, mixed layouts, int32 data,
missing or incorrectly sized pulses are rejected. Do not infer a format from
an apparent 32-channel payload.

## Configuration

This example is intentionally not enabled by a default configuration or
Compose service. `verified-uut` and its numeric values are placeholders; they
do not assert that a producer or calibration exists at a deployment.

```yaml
schema_version: 1
redis:
  host: 127.0.0.1
  port: 6379
  db: 0
  connect_timeout_ms: 200
  command_timeout_ms: 500
  poll_interval_ms: 100
  retry_ms: 1000
sources:
  - stream: '{AES}:verified-uut-raw'
    checkpoint: '{AES}:demux:verified-uut:v1'
    payload_field: '_'
    format: int16-le
    layout: sample-major
    samples_per_channel: 4096
    start_id: '0-0'
    retention: 1000
    timestamp: {mode: absent}
    channels:
      - channel: 5
        raw: '{AES}:verified-uut-CH5-raw'
        volts: '{AES}:verified-uut-CH5-VOLTS'
        eslo: 0.001
        eoff: 0.0
      - channel: 8
        raw: '{AES}:verified-uut-CH8-raw'
        volts: '{AES}:verified-uut-CH8-VOLTS'
        eslo: 0.001
        eoff: 0.0
```

The `channels` sequence is the wire order, not a sorted list. Each `channel`
is an explicit physical channel label; an ordered physical list can represent
multiple modules using the producer's absolute numbering. Channel labels must
be unique per source. The complete pulse size is exactly
`samples_per_channel * channels.size() * 2` bytes. In the example, successive
int16 values are channel 5 sample 0, channel 8 sample 0, channel 5 sample 1,
channel 8 sample 1, and so on. Every channel supplies explicit ESLO and EOFF;
ESLO and EOFF are stored as finite float32 constants. The worker evaluates
`ADC * ESLO + EOFF` in double precision, then rounds the result to float32;
overflow rejects the pulse. This worker does not fetch calibration PVs or
invent defaults.

All fields are strict: unknown or duplicate YAML fields are errors. All
source, output, and checkpoint keys must be globally distinct. Redis host is
a numeric IPv4 address, avoiding unbounded DNS resolution. Port 4210 is
explicitly forbidden because it is the digitizer acquisition socket. This initial worker
supports standalone Redis without authentication or TLS; Redis Cluster and
redirects are unsupported. A dedicated service account or authenticated
deployment needs an explicit connection configuration extension before use.

The timestamp mode must be explicit. For a producer that supplies genuine
Unix acquisition nanoseconds and validity, replace `timestamp` with its exact
field names:

```yaml
timestamp:
  mode: unix-nanoseconds
  field: producer_acquisition_time_ns
  valid_field: producer_acquisition_time_valid
```

Nanoseconds are parsed directly as signed int64 decimal text, with no floating
point conversion. The timestamp and validity fields may be absent; validity
defaults to false. A validity field of `1` requires a supplied valid int64
timestamp; `0` retains any supplied timestamp as invalid. Other validity
values and malformed timestamp text reject the entry. Backward acquisition
times, negative timestamps, and exact nanoseconds are preserved. The worker
never substitutes receipt time, the Redis ID, or the local clock.

`xrm-proxy --check-demux-config FILE` validates configuration offline and exits
without starting Redis, PVA discovery, or a worker. It does not validate that
a producer exists or that its declared byte format matches hardware.

## Outputs and restart behavior

Each valid source entry produces one raw and one volts stream entry per
configured channel. Their Redis IDs match the source Redis ID; the payload
field is `_`. Raw payloads are signed int16 little-endian, and calibrated
payloads are IEEE754 float32 little-endian. Other fields are:

| Field | Meaning |
| --- | --- |
| `schema` | `fermi-ad/xrm-demux:1` |
| `format` | `int16-le` or `float32-le` |
| `channel` | Configured physical channel label |
| `source_stream` | Exact input stream key |
| `source_id` | Original Redis entry ID, separate from acquisition time |
| `acquisition_time_valid` | `1` only when explicitly valid producer time exists; otherwise `0` (INVALID time) |
| `acquisition_time_ns` | Original supplied acquisition nanoseconds; absent if producer time is absent |
| `source_continuity` | `observed`, or `unknown` when the previous checkpoint entry is no longer in source history |

Output streams use exact `MAXLEN = retention`; retention is 1–100,000 entries
per output. Retention limits are not deletions of the stream key. No receipt
timestamp is placed in the acquisition fields. A downstream IOC must honor
`acquisition_time_valid` and the timestamp's presence rather than treating a
Redis-generated ID as acquisition time.

The checkpoint is a per-source Redis hash. It binds the input interpretation
and output keys to a deterministic configuration fingerprint, then stores
`last_id`, `processed`, `rejected`, `gaps`, and `last_error`. `last_rejected_id`
and `last_rejection` preserve the most recent rejected source ID and reason
even after later valid data; `last_gap_id` identifies the latest continuity
uncertainty. A malformed pulse is deliberately skipped by advancing the
checkpoint and increasing the durable rejection counter; it is never silently
dropped or transformed into a waveform. One bounded pending input tuple is
retained in this hash before fanout, including its bytes, source ID, timestamp,
and continuity decision. It is replayed before fetching another input, so a
producer can trim the original source entry while an incomplete fanout is
retried. The cursor, counters, rejection information, and cleared pending state
commit together in one final HSET. Worker status also exposes session
processed/rejected/error/gap counters. `xrm-proxy` logs changes.

`start_id` is used only when creating a new checkpoint: `0-0` begins with the
oldest retained entry; another canonical Redis ID explicitly begins after
that ID. Restart reads strictly after the durable checkpoint. A changed
interpretation, calibration, destination, initial cursor, or retention setting
with the same checkpoint fails closed. Use a new checkpoint **and fresh output
keys** for a changed pipeline, preserving prior outputs for inspection.

Lua preflights all output/checkpoint types, output IDs, and same-ID contents
before the first fanout write. All fanout writes and the checkpoint commit run
within one Lua invocation. A Redis Lua runtime failure does not provide
rollback; retries therefore recognize matching existing same-ID records and
finish missing outputs. A same-ID record with different metadata or payload
stops that source with a visible collision error. Compare-and-set checkpoint
advancement prevents duplicate processing by concurrent identical workers.
These guarantees assume checkpoint/output keys remain under this pipeline's
ownership; external mutation or Redis data loss is not repaired by guessing.

Redis retention does not expose a count of missing source entries. If the
checkpoint entry has disappeared before the next available entry is read,
the worker reports continuity as unknown and increments `gaps`. This is a
conservative continuity signal, not an exact lost-pulse count. The worker
cannot detect input loss that happened before a new checkpoint's `start_id`.
If the source remains empty, no waveform is fabricated to report a gap.

## Resource bounds and validation

Configuration is limited to 1 MiB, 32 sources, 64 channels per source, and a
1 MiB complete input pulse. Each source is polled once per round with
`XRANGE COUNT 1`; server-side validation limits the returned payload and
metadata. The worker holds only one pulse and its fanout at a time; each source
checkpoint additionally retains at most one pending pulse in Redis. A producer
can still impose server-side cost by writing enormous Redis records; size
limits prevent returning those records to this client, not their original
creation or temporary representation inside Redis.

Connection and command deadlines are 1–2,000 ms. Redis command I/O is
nonblocking, with an absolute deadline across all writes and reply reads;
continuous slow response bytes cannot extend that deadline. Shutdown checks
interrupt command waits every 50 ms. Idle rounds pause for the
configured polling interval, and failures pause for `retry_ms`. A busy source
can progress without an idle pause, but cannot starve the other configured
sources in its round. Shutdown wakes backoff immediately and waits only for
the currently bounded connection attempt or command operation. A command with
an unknown result causes connection disposal, so its late reply cannot be
mistaken for another source's result. One failed source does not stop other
configured sources or PVA discovery.

`xrm_demux_tests` exercises binary decoding, signed extrema, calibration,
strict configuration, and exact timestamps. `xrm_demux_redis_tests` runs through
`scripts/run-with-redis.py` against a newly spawned private loopback Redis,
testing restart, durable rejection, partial fanout recovery, collisions,
wrong-type isolation, timestamps, retention, continuity, and concurrent
workers. These tests do not connect to deployment Redis or hardware.
