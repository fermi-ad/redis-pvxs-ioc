# Redis XRM waveform demultiplexing

`xrm-proxy --demux-config FILE` optionally runs an independent Redis worker
alongside PVA discovery. A demux configuration or Redis failure is reported
without stopping mirror discovery. The worker never connects to a digitizer,
opens an acquisition socket, changes controls, discovers Redis keys, or deletes
keys. Only explicitly configured source streams are read. Writes target their
exact outputs, checkpoints, and gain keys; raw input history is trimmed only
with an explicit `input_retention` ownership grant. Inputs are otherwise
unchanged. Without this option, no Redis worker or Redis connection is created.

The same worker supports an optional `current_conversions` sequence for
float32 VOLTS to MAMPS conversion. A configuration may contain raw `sources`,
`current_conversions`, or both, with at least one nonempty sequence. This uses
the existing sidecar binary and image; it introduces no Python service or
hardware connection. Conversion-only operation can consume existing VOLTS
streams while a separately managed raw capture and ADC processor continue.

When a profile contains both raw sources and current conversions, raw ingestion
and current conversion run in separate execution lanes and Redis connections.
The configured VOLTS streams provide their existing bounded buffer; no in-memory
queue is added. All current channels stay together for shared-gain ordering and
lease fencing. A conversion retry does not stop raw ingestion, and aggregate
status retains the degraded lane's error. Source-only and conversion-only
profiles each use one lane.

The default `int16-le` input is one complete pulse per Redis entry, containing
aligned sample-major signed little-endian int16 ADC words and no event header.
The separately selected `acq4xx-es-int16-le` profile validates and removes one
leading 32-byte event frame from the observed sixteen-channel packed-int16
layout. Other ES layouts, SPAD, and arbitrary TCP chunks remain unsupported.
Configure the actual producer's channel order, pulse length, calibration and
timestamp fields before enabling it. Source declarations are
an operator contract: same-sized SPAD bytes falsely declared as ADC cannot be
identified from their binary values. Unknown fields, mixed layouts, int32 data,
missing or incorrectly sized pulses are rejected. Do not infer a format from
an apparent 32-channel payload.

PM STOP freezes history; it does not stop capture. A provisioned 4 MHz setting
does not guarantee Ethernet throughput for the selected channel count and
format. Verify the producer's actual output rate and sustained transport
capacity before sizing this pipeline.

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
be unique per source. For the default `int16-le` format, the complete pulse size is exactly
`samples_per_channel * channels.size() * 2` bytes. In the example, successive
int16 values are channel 5 sample 0, channel 8 sample 0, channel 5 sample 1,
channel 8 sample 1, and so on. Every channel supplies explicit ESLO and EOFF;
ESLO and EOFF are stored as finite float32 constants. The worker evaluates
`ADC * ESLO + EOFF` in double precision, then rounds the result to float32;
overflow rejects the pulse. This worker does not fetch calibration PVs or
invent defaults.

The optional event profile uses the same source fields with
`format: acq4xx-es-int16-le` and exactly sixteen configured channels. Its
`samples_per_channel` is the number of ADC frames **after** the header; total
input length is `(samples_per_channel + 1) * 32` bytes. For the observed
524,288-byte transfers, select 16,383 ADC samples per channel, not 16,384.
Each raw output then contains 32,766 bytes and each VOLTS/current output
contains 65,532 bytes. A three-frame fixture similarly contains 128 input
bytes, three int16 samples per raw channel, and three float32 output samples.

The header must contain four little-endian `0xaa55f154` magic words followed
by two unsigned32 counters and their exact duplicates. Unsupported magic,
wrong sizes or counter pairs, and an additional recognizable event signature
inside the ADC area reject the entire pulse. No format is inferred from byte
count. The event profile preserves the legacy NumPy calibration sequence:
round `ADC * float32(ESLO)` to float32, then add `float32(EOFF)` and round
again. This differs from the default ADC-only double-intermediate calculation;
the existing ADC-only interpretation and fingerprints remain unchanged.
See the [event byte contract](xrm-event-format.md) for the observed profile,
its strict helper, and the unresolved physical meanings of the counters.
This is a declared software format, not hardware timing qualification.

All fields are strict: unknown or duplicate YAML fields are errors. All
source, output, and checkpoint keys must be globally distinct, except that a
declared raw VOLTS output may feed one declared current conversion. Conversions
may also share an identical global gain setting/applied pair and bootstrap.
Other key-role collisions are rejected. Redis host is
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

## Current conversion and global gain

The following illustrates conversion-only operation. Before enabling a live
replacement, stop its previous MAMPS writer and replace `start_id` with the
actual last published MAMPS ID. This example ID is a placeholder, not a safe
deployment cursor. The worker never selects a cursor automatically. A durable
checkpoint takes precedence after first adoption.

```yaml
schema_version: 1
redis:
  host: 127.0.0.1
  port: 6379
  connect_timeout_ms: 200
  command_timeout_ms: 500
  poll_interval_ms: 100
  retry_ms: 1000
current_conversions:
  - stream: '{XRM}:acq2206-097-CH1-VOLTS'
    output: '{XRM}:acq2206-097-CH1-MAMPS'
    checkpoint: '{XRM}:current:acq2206-097:CH1:v1'
    payload_field: '_'
    samples_per_pulse: 16384
    start_id: '1700000000123-7' # replace with the explicit cutover boundary
    retention: 1000
    timestamp: {mode: absent}
    gain:
      setting: '{XRM}:acq2206-097-BCM_M_S'
      applied: '{XRM}:acq2206-097-BCM_M_R'
      bootstrap: 2.5
```

Each input contains exactly `samples_per_pulse * 4` bytes of little-endian
IEEE754 float32 VOLTS. The sample bound is 1–16,384; a decoder that removes an
event header may explicitly select 16,383. The worker evaluates one float32
product per sample, matching the legacy NumPy float32 current calculation.
It accepts signed and zero gain. Nonfinite input, overflow, partial pulses,
unknown metadata, and invalid acquisition timestamps are durably rejected.
There is no gain CLI override or implicit offset.

Commands use a Redis stream entry containing exactly the literal `_` field,
with four little-endian float32 bytes. Only finite float32 commands are
accepted. `bootstrap` is appended only when the setting key is absent. A
persisted operator command, including zero or a negative value, is retained
on restart. A malformed or nonfinite command records `gain_last_rejected_id`
and `gain_last_error` in its checkpoint and produces no applied readback.
A prior valid gain remains usable; no waveform is fabricated for a setting.

The worker validates and applies a command to every configured conversion
sharing that gain pair before appending one applied `_` readback. It does so
while idle, without waiting for a waveform. Readback includes the original
`gain_command_id`. Readback IDs encode host wall-clock nanoseconds using the
legacy `RA_Time` mapping (`milliseconds` plus the nanosecond remainder), and
increase beyond the previous applied ID. These are setting timestamps for IOC
write confirmation, not acquisition timestamps. The IOC owns the public
`SL:INST:ICM:XRM:BCM_M` float32 PV, its mA/V units, access policy, and a
5,000 ms read/write confirmation; the sidecar performs Redis I/O only.

Shared commands are processed once at a worker-round boundary, before retaining
any channel's next waveform. A command arriving during that round waits for
the next boundary. Gain is captured once when a complete source waveform is retained as pending.
Its binary value and command ID remain attached through failed writes and
restart, even when a newer idle command has been acknowledged. Output IDs
equal the source VOLTS IDs. A new gain affects only newly retained waveforms;
it never rewrites historical MAMPS entries. A concurrent legacy writer must
be stopped explicitly before enabling the replacement.

Current output metadata uses `schema: fermi-ad/xrm-current:1`, `format:
float32-le`, `source_stream`, `source_id`, binary float32 `gain`,
`gain_command_id`, and the same acquisition-time and continuity fields as
raw demux outputs. With `timestamp: {mode: absent}`, acquisition time is
explicitly invalid and absent; Redis IDs and receipt times never replace it.
The optional `unix-nanoseconds` timestamp declaration has the same exact
signed-int64 semantics as raw sources. When a configured raw VOLTS output
feeds conversion, its bounded schema metadata is accepted; original raw
provenance is retained in `upstream_source_stream` and `upstream_source_id`.
Physical `channel` and neutral unsigned32 `event_counter_0` and
`event_counter_1`, if supplied by raw demux, are copied exactly. These counters
do not imply an epoch, synchronized clock, or qualified acquisition timing.

Each current checkpoint also contains a server-time writer lease. Both read
and commit operations fence the lease token; a second worker cannot operate
the same conversion while that lease is held. Clean shutdown releases the
lease by expiring its checkpoint state, and a finite expiry permits crash
recovery. Lease duration bounds cover configured polling, retry, and command
deadlines. Checkpoint, output, gain-setting, and readback history are never
deleted or discovered through key scans. Exact output retention bounds each
named output; setting bootstrap/readback writes use the declared retention.

This software contract does not qualify the hardware wire format. Raw TCP
capture remains independently controlled; port 4210 is never contacted by
this worker, and enabling conversion does not start acquisition or change
device controls.

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
| `event_counter_0` | Event profile only: exact raw unsigned32 word 4/6, preserved as provenance |
| `event_counter_1` | Event profile only: exact raw unsigned32 word 5/7, preserved as provenance |

Output streams use exact `MAXLEN = retention`; retention is 1–100,000 entries
per output. Retention limits are not deletions of the stream key. No receipt
timestamp is placed in the acquisition fields. A downstream IOC must honor
`acquisition_time_valid` and the timestamp's presence rather than treating a
Redis-generated ID as acquisition time.

Raw source declarations may additionally set `input_retention: N`, where
`N` is 1–100,000. This is an explicit ownership grant to trim that one named
input stream with exact `XTRIM MAXLEN = N` after a source entry has committed
its outputs or durable rejection. It is omitted by default; its internal
default is zero, which performs no input trimming and preserves existing
configuration fingerprints. To disable trimming, omit the field. The option
is not accepted on `current_conversions`; their input VOLTS history is already
bounded by the upstream raw source's output retention when chained.

Input trimming is a separate bounded Redis command after the atomic output
and checkpoint commit. A failed commit cannot trigger it. A trim failure
leaves the committed checkpoint intact and is reported as a retryable worker
error; this option does not promise a strict bound while Redis commands fail.
It grants no ownership of other sources, settings, gain readbacks, or keys
selected by a pattern. Changing the grant changes the configuration binding.

This is a storage bound, not a drain-until-consumed policy. If a pre-existing
backlog exceeds `N`, the first committed entry can trim unprocessed history.
The retained pending entry remains replayable, but skipped source rows cannot
be reconstructed. For example, a six-row backlog with `N=2` can commit ID1,
retain only IDs5/6, then report `source_continuity: unknown` at ID5 and advance
the durable gap counter. Size retention for the intended backlog and retain
the gap diagnostics when an explicit storage-pressure policy accepts loss.

`redis-pvxs-ioc` provides that adapter for ordinary read-only numeric arrays via
the opt-in policy below. Configure each raw output as int16 and each volts
output as float32, on a little-endian IOC host, and resolve its read route to
the exact output key:

```yaml
source_time:
  mode: unix-nanoseconds-v1
  field: acquisition_time_ns
  valid_field: acquisition_time_valid
```

See [source time policy](configuration.md#source-time-policy) for a complete PV
example. The IOC preserves exact signed acquisition nanoseconds, including zero
and backward time. A payload with unknown or invalid acquisition time updates
with an INVALID alarm; absent time publishes zero rather than a fabricated
timestamp. Malformed timing retains the last accepted waveform and time. The
default legacy Redis-ID timestamp mode is unchanged and must be explicitly
replaced for these outputs. Native PVA forwarding and NTNDArray use their own
timestamp contracts.

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

Configuration is limited to 1 MiB, 32 raw sources, 32 current conversions,
64 channels per ADC-only source, and a 1 MiB complete input pulse. The event
profile requires exactly sixteen channels and includes its header in that
byte bound. Each source is polled once per round with
`XRANGE COUNT 1`; server-side validation limits the returned payload and
metadata. Each execution lane holds one pulse and its fanout at a time; each source
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
workers. It also checks a sixteen-channel ES→VOLTS→MAMPS pipeline against
literal NumPy float32 bytes, exact event-counter provenance, rejected ES input
followed by recovery, and explicit input trimming with a durable backlog gap.
These tests do not connect to deployment Redis or hardware.
