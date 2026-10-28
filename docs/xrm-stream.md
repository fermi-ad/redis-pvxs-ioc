# XRM waveform stream companion

**Opening the XRM streaming socket can start hardware acquisition.** Run this
companion only when acquisition is authorized and the device has been
characterized. No live acquisition was performed by these tests.

`xrm-stream` is a TCP-to-Redis companion. Its decoder models a headerless
interleaved sample stream with one little-endian storage word per selected
channel, in ascending channel order.
It does not configure the instrument. Its channel list, storage width and
sample rate describe the configuration expected on the socket; they do not
change or independently verify the device. `samples-per-batch` controls how
many samples from each channel make one Redis waveform. At a sustained,
verified device rate, the nominal update frequency is sample rate divided by
samples per batch; actual delivery depends on the source and Redis.

Before use with a particular XRM, establish its firmware, socket ownership,
channel mask and order, byte order, signedness, storage width, bit alignment,
sample rate, connection-boundary alignment, and behavior of SPAD words and
event signatures. The model does not establish any instrument's packing or
timing. This decoder does not parse SPAD or event
signatures. Since this stream has no characterized synchronization marker,
it cannot detect a connection that begins partway through a sample frame.
Use only after the device's format and connection alignment are verified.

The source accepts 1–32 distinct ascending channels numbered 1–32, 1–4 storage
bytes per word, an integer rate from 1 to 100,000,000 Hz, and a nonempty batch
of at most 8 MiB interleaved data. Words are copied byte-for-byte; 18-bit
values are not truncated or converted. The IOC's numeric primitive types use
1, 2 or 4 bytes, so a 3-byte storage format requires a separately verified
conversion before it can be served as a numeric waveform. Signedness is an
explicit IOC type choice, not inferred by this companion.

The program requires numeric IPv4 addresses and currently supports neither
Redis authentication nor TLS. Run it only on an appropriately isolated,
authorized network. There is no default XRM address. For example, after
verifying a suitable device format and Redis access, choose values explicitly:

```sh
xrm-stream --xrm-host 192.0.2.10 --xrm-port 4210 \
  --redis-host 192.0.2.20 --redis-port 6379 --base-key my-xrm \
  --key-prefix adc --channels 1,5,8 --sample-bytes 4 \
  --sample-rate-hz 1000000 --samples-per-batch 10000 --maxlen 128 \
  --publish-latest
```

These documentation-only addresses and settings are not a device
configuration. `--duration-ms` optionally limits a run; SIGINT and SIGTERM
stop it. `--maxlen` bounds each Redis history to 1–1,000,000 entries (128 by
default), using exact trimming. The assembled batch, demultiplexed channels,
and in-flight Redis request are bounded by the configured 8 MiB input cap.
Redis and XRM socket operations have timeouts. Redis pressure can delay reads;
there is no lossless-acquisition or sustained-rate guarantee.

## Redis contract

Each channel's history stream uses `{base-key}:key-prefix:CH<n>`, with schema
`fermi-ad/xrm-waveform:1`. The optional `--publish-latest` mode also publishes
`{base-key}:key-prefix:CH<n>:latest:v1` hashes, a shared
`{base-key}:key-prefix:latest:v1:commit` marker, and a
`{base-key}:key-prefix:latest:v1:commits` stream. The IOC's `xrm_v1` mode
requires this latest-value contract; it never reads the history stream. Without
`--publish-latest`, the companion publishes only history and cannot feed an
`xrm_v1` IOC.

The IOC requires Redis ACL permission for `EVAL`, `TYPE`, `OBJECT ENCODING`,
`MEMORY USAGE`, `HLEN`, `HSTRLEN`, `HGET`, `STRLEN`, and `GET` on its latest
hash and marker. It fails closed on a denied command, missing or invalid marker,
invalid hash, or unsupported encoding, with no Stream fallback. The script
reads a fixed set of fields only after checking each field's size. It rejects
hashes with more than 32 fields and compact hashes over 64 KiB before field
lookup; unknown fields are never retrieved. The configured payload limit bounds
the accepted waveform. These are per-read bounds, not a global Redis memory or
CPU budget; concurrent readers and writers require separate resource planning.
The optional publisher script requires `EVAL`, `TYPE`, `XLEN`, `OBJECT
ENCODING`, `MEMORY USAGE`, `HLEN`, `STRLEN`, `SET`, `HSET`, `HDEL`, and `XADD`
on its group keys. Grant read and write permissions only to the intended
principals; `xrm-stream` itself currently cannot authenticate.

This contract assumes one trusted producer exclusively owns each stream group,
its hashes, marker, and commit stream. Other principals may read them but must
not write, replace, trim, expire, or forge them. ACLs do not validate values or
force writers to use the publication script. Start with a clean namespace and
enforce that ownership independently. In particular, the producer still writes
and trims history streams; the IOC's bounded hash reads do not bound arbitrary
preexisting or externally written Stream data or Redis storage.

| Field | Meaning |
| --- | --- |
| `_` | Little-endian sample words copied from the stream |
| `schema` | `fermi-ad/xrm-waveform:1` |
| `sample_bytes`, `sample_rate_hz`, `sample_count`, `channel` | Decimal configured width, configured rate, sample count and channel number |
| `session`, `connection`, `batch` | Producer identity and sequence for correlation across channels |
| `source_validity` | `unknown`, `valid`, or `invalid`; the raw socket source emits `unknown` |
| `acq_time_valid` | `0` or `1` for the source timestamp validity |
| `acq_time_ns` | Optional decimal Unix nanoseconds only when its epoch and units are verified |

The raw socket source has no characterized acquisition timestamp or validity
indicator. It emits `acq_time_valid=0`, omits `acq_time_ns`, and leaves
`source_validity=unknown`. The library's metadata interface can preserve both
a verified Unix timestamp and its independent invalid flag if a future source
provides them. It never substitutes a Redis ID or receive time. Redis assigns
each stream ID with `*` for ordering, independent of acquisition time, including
when a future source clock moves backwards. Consumers must implement the
versioned metadata contract: the legacy generic Redis waveform path uses the
stream ID as a timestamp and is therefore not suitable for these records.
IOC PVs using `stream_protocol: xrm_v1` must explicitly set a nonzero
`source_health.stale_after_ms`. Set it for the expected acquisition and trigger
cadence so the IOC marks an interrupted stream invalid while allowing normal
quiet intervals.

### PVA waveform metadata and loss

An `xrm_v1` waveform is an NTScalarArray with an `xrm` structure delivered in
the same PVA update as `value` and `timeStamp`. Legacy PV structures are
unchanged. `xrm.present` is false until the IOC has received a complete valid
envelope. The structure contains:

| PVA field | Type | Meaning |
| --- | --- | --- |
| `xrm.session` | string | Producer session identifier |
| `xrm.connection`, `xrm.batch` | uint64 | Connection and batch numbers within that session |
| `xrm.channel` | uint32 | Channel number, 1–32 |
| `xrm.sampleRateHz`, `xrm.sampleCount` | uint64 | Reported sample rate and number of words in this channel's batch |
| `xrm.sampleBytes` | uint32 | Storage bytes per word |
| `xrm.sourceValidity` | string | `unknown`, `valid`, or `invalid` |
| `xrm.acqTimeValid` | bool | Source's acquisition-time validity flag |
| `xrm.acqTimePresent`, `xrm.acqTimeParsed` | bool | Whether a Unix-ns field was present and whether it parsed successfully |
| `xrm.missingBatches` | uint64 | Batch numbers skipped between IOC observations within a session and connection |
| `xrm.discontinuities` | uint64 | Total observed session, connection and sequence discontinuities |
| `xrm.sessionChanges`, `xrm.connectionChanges`, `xrm.sequenceDiscontinuities` | uint64 | Discontinuities by cause |

The IOC validates the schema, bounded session identifier, positive connection
and batch, channel, rate, sample count, storage width, and source/time flags.
If the last component of `read.key` is `CH<n>`, the envelope's channel must
match it. A malformed envelope keeps the previous complete waveform, time and
metadata and raises an INVALID alarm. A complete envelope is displayed even
if its source validity is `unknown` or `invalid`, or its acquisition time is
invalid; the alarm is INVALID in each case. A parseable Unix timestamp is
preserved with an invalid flag. An absent or unparseable timestamp is zero
and is never replaced with a Redis ID or the receiver clock. In particular,
the current raw socket source produces INVALID waveform alarms because its
validity and acquisition time are unverified.

Batch numbers enable clients to compare channels and detect partial or missing
publications. A forward jump within the same session and connection increments
`missingBatches` by the number of missing batch numbers; this can include
producer-side gaps, rejected entries and values coalesced between IOC polls. A session
change, connection change, or duplicate/backward batch increments a
discontinuity counter without guessing the number lost. A session change is
counted separately from a connection change because connection numbers belong
to a session. Counters saturate at the uint64 maximum. The first observed
latest snapshot establishes a baseline, so earlier losses are unknown. The
generic Redis reader's `retentionGaps` counter is not populated for XRM;
use these per-PV counters to observe sequence gaps. Counters
persist across metadata-only reloads and reset when the PV runtime is
recreated or the IOC restarts.

The IOC observes only the latest committed hash and may skip intermediate
waveforms even while their Stream entries remain retained. After startup, a
Redis outage, a rejected read or a stream-ID reset, a retained hash establishes
a baseline but remains INVALID until a subsequent publication is observed.
For that new value, the previous successful poll's request-start time is a
conservative bound on how old the publication could be. Unchanged polls do not
refresh it. A delayed response may therefore be stale on arrival. This is
producer-liveness evidence under the trusted-writer contract, not an
acquisition timestamp or proof that no newer value has since arrived.
When a hash disappears or Redis becomes unavailable, the reader records that
state immediately; the PVA health alarm is refreshed by the application's
normal source-health pump, typically once per second, rather than instantly.
Source-health readiness remains invalid while source validity or time is
invalid, even if records continue arriving. For receipt-liveness evidence,
the source diagnostics expose `lastReceivedAgeMs` from the reader's local
monotonic clock; a size-accepted Redis response can update that field before full XRM
envelope validation, and it does not establish acquisition-time validity.
When the raw source stops, its readiness was already invalid.

For the illustrative command above, a matching IOC configuration is:

```yaml
server: {instance: xrm-example, namespace: XRM}
redis: {base_key: my-xrm, host: 192.0.2.20, port: 6379}
pvs:
  - name: ADC:CH1
    type: int32
    shape: array
    stream_protocol: xrm_v1
    read: {key: 'adc:CH1'}
    source_health: {stale_after_ms: 1000}
  - name: ADC:CH5
    type: int32
    shape: array
    stream_protocol: xrm_v1
    read: {key: 'adc:CH5'}
    source_health: {stale_after_ms: 1000}
  - name: ADC:CH8
    type: int32
    shape: array
    stream_protocol: xrm_v1
    read: {key: 'adc:CH8'}
    source_health: {stale_after_ms: 1000}
```

The signed type and one-second freshness interval are illustrative. Confirm the
device's actual encoding and expected time between acquisitions before choosing
either value; the example does not authorize connecting to a live device.

With `--publish-latest`, one Redis Lua execution first marks the group pending,
then appends each channel's Stream entry and updates its hash. Only after every
channel succeeds does it append the batch token and selected channels to the
commit stream and set the shared marker to that token. Redis does not roll back
earlier writes on a script error: history consumers can see orphan or partial
entries, while IOC hash readers reject all channels while the marker is pending
or mismatched. History consumers must reconcile each entry's
`session/connection/batch` token against the retained commit stream and the
listed channels; when the commit record is absent, the batch is uncommitted or
its status is unknown because the commit stream was trimmed. The current marker
alone cannot prove that an older batch committed. Each IOC channel polls and
publishes independently, so the shared marker does not make PVA updates across
channels simultaneous; clients must compare session, connection, and batch
before combining channels. Both history and commit
streams use `--maxlen`; the script rejects an existing stream already above that
limit. A connection loss can make the outcome unknown, and the companion does
not replay the batch. Without `--publish-latest`, the older Stream-only
transaction has no commit journal. On source or Redis disconnect it closes both sockets,
discards the incomplete buffered batch, and reconnects. This prevents joining
partial data from different connections, but cannot establish that the next
connection starts on a source frame boundary. Reconnect attempts back off from
250 ms to at most five seconds, resetting after a successful publication.
Counters and discarded byte
counts are printed on disconnect and stop; they are not persistent history.

The automated tests use simulated byte streams and isolated local Redis only.
They exercise splits, reconnects, timestamps moving backwards, ambiguous
writes, history bounds, and synthetic 32-channel throughput. They do not
characterize or qualify a live XRM or its operating rate.
