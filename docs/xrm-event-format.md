# Qualified 16-channel XRM event pulse

The explicit `acq4xx-es-int16-le` profile handles one observed packed-int16
producer layout. It removes one validated 32-byte Event Signature (ES) before
ordinary channel demultiplexing. It does not generalize the decoder to other
ACQ4xx formats, open a hardware stream, or change the existing `int16-le`
interpretation. Generic `int16-le` input still means ADC words only.

## Evidence and scope

Read-only examination of three retained raw Redis entries on November 2, 2026
found this same structure in each entry:

- 524288 bytes, with configured channel mask `0xffff` and
  `RTM_TRANSLEN=16384`;
- four little-endian u32 words equal to `0xaa55f154` at the beginning;
- two raw u32 counters, repeated once, completing the 32-byte ES frame; and
- 16383 sample-major ADC frames, each containing sixteen signed
  little-endian int16 values.

Thus the configured transfer length includes one ES frame in this observed
profile: there are 16383 ADC samples per channel, not 16384. The first header
bytes `54 f1 55 aa` represent ES magic, not ADC channel values. Misreading their
first int16 as ADC yields -3756 before calibration, creating a spurious first
sample. The helper returns only the 524256 ADC bytes, without modifying them.
Physical channel labels and their order remain a separate configured contract.

The reference is the D-TACQ *XRM User Guide*, Rev 1, October 31, 2026,
section 12.2.2, page 43. Its table describes an eight-u32 ES block. The format
tables on pages 42-43 show that the number of ES blocks and their relation to
samples vary with product, packing and channel count. This implementation
accepts only the observed sixteen-channel packed-int16 profile. It does not
claim that all XRM modules use this format.

## Demux profile and calibration

A configured raw source selects `format: acq4xx-es-int16-le` with
`layout: sample-major`, sixteen explicit channel mappings, and
`samples_per_channel: 16383` for the observed 512 KiB geometry. The ADC count
excludes the leading ES frame. The complete pulse size includes that frame,
so replacing `16383` with the hardware transfer length `16384` is incorrect.
Existing source, output, checkpoint, calibration and retention requirements
still apply. The format is part of the checkpoint fingerprint; a framing
change requires a new checkpoint and fresh output keys.

Each raw output contains only the selected channel's ADC int16 bytes. Each
volts output contains calibrated IEEE754 float32 values, both little-endian.
For this ES profile, ESLO and EOFF are float32 constants; calibration rounds
`ADC * ESLO` to float32, then adds EOFF and rounds to float32 again. The
multiply and add remain separate even when the compiler supports fused
multiply-add. This matches the reference NumPy float32 operations and can
differ by a bit from evaluating the entire expression in double precision.
The generic `int16-le` profile retains its original double-intermediate
calibration behavior.

Every raw and volts output preserves the unsigned header counters as decimal
`event_counter_0` and `event_counter_1`, alongside the ordinary source stream,
source ID, channel and continuity metadata. Output `format` remains
`int16-le` for raw and `float32-le` for volts: the ES frame has been removed.
Counter presence does not make acquisition time valid. With
`timestamp: {mode: absent}`, outputs carry `acquisition_time_valid: '0'`
and no `acquisition_time_ns`. A separately configured, verified producer
timestamp remains independent of these event counters.

## Byte contract

Each complete input payload must contain exactly
`(samples_per_channel + 1) * 32` bytes. `samples_per_channel` means the number of
ADC frames after the ES frame. It must be 1-32767; the entire input, including
the ES frame, is limited to 1 MiB. The channel count must be exactly sixteen.

| u32 word offset | Byte offset | Required content |
| --- | --- | --- |
| 0-3 | 0-15 | Four identical little-endian `0xaa55f154` magic words |
| 4 | 16-19 | First raw unsigned counter |
| 5 | 20-23 | Second raw unsigned counter |
| 6 | 24-27 | Exact duplicate of word 4 |
| 7 | 28-31 | Exact duplicate of word 5 |
| 8 onward | 32 onward | Sixteen signed int16 ADC lanes per frame, sample-major, little-endian |

Missing or unsupported magic, a counter-pair mismatch, an incorrect byte count,
unsupported channel count, truncation and trailing bytes all reject the pulse.
No partial waveform is returned. After the initial frame, the decoder also
rejects any four repeated ES-family magic words with high 28 bits
`0xaa55f15`, regardless of alignment or event flag. This includes a truncated
or malformed additional event signature. A data sequence indistinguishable
from that signature prefix is deliberately rejected: mixed bursts must not be
silently interpreted as ADC samples.

Other channel counts, multiple leading ES blocks, event codes other than the
observed initial `0xaa55f154`, SPAD, unpacked/32-bit ADC words and arbitrary TCP
chunks remain unsupported. A payload without a leading ES frame must use a
separately qualified ADC-only format. This helper does not auto-detect formats
or remove bytes based on an apparent channel count.

## Counters are provenance, not acquisition time

The manual labels words 4/6 **Sample Counter** and words 5/7 **Sample Clock
Counter**. It describes the first as acquired sample count and the second as
sample-clock count, with a difference during burst idle intervals. The helper's
`sampleCounter` and `sampleClockCounter` names follow those documented offsets.
Both values remain raw uint32 values, including their full unsigned range;
there is no rollover extension, subtraction or clock conversion.

The retained entries showed a disagreement with those labels: the first
counter included idle intervals, while the second advanced by 16383 between
adjacent entries. This is observed behavior, not a validated remapping of the
manual's counter definitions. Preserve neutral downstream names
`event_counter_0` for word 4/6 and `event_counter_1` for word 5/7 until the actual
firmware semantics are established.

Neither counter establishes a Unix epoch, sample frequency, White Rabbit
validity, TAI-to-Unix conversion or nanosecond acquisition timestamp. Acquisition
time remains unknown unless the producer supplies a separately verified time
contract. Do not substitute the Redis entry ID, receipt time or the sidecar's
clock. An IOC consuming outputs without valid acquisition time must retain an
explicit invalid-time policy and alarm.

## Dependency-free helper and tests

`tools/xrm_event.h` provides:

```cpp
namespace xrm_demux {
struct EventPulse {
  std::string adcBytes;
  uint32_t sampleCounter;
  uint32_t sampleClockCounter;
};
EventPulse decodeEventPulse(const std::string& payload,
                           size_t channels, size_t samplesPerChannel);
}
```

Validation failures throw `std::invalid_argument`. A worker integration should
classify those exceptions as rejected input, preserving its ordinary durable
rejection/checkpoint rules. They must not become successful empty waveforms or
indefinite transport retries.

The standalone tests require only a C++17 compiler:

```sh
c++ -std=c++17 -UNDEBUG -Itools tests/xrm_event_tests.cpp -o /tmp/xrm_event_tests
/tmp/xrm_event_tests
```

Fixtures cover exact header removal, signed ADC extrema, unsigned counter
extrema, duplicate checks, unsupported layouts, malformed/truncated/oversized
input, embedded ES prefixes and the observed 512 KiB geometry. These tests
operate on synthetic memory buffers, without Redis or hardware access.

`xrm_event_demux_tests` also exercises the actual configuration parser and
demultiplexer across all sixteen channels. Its default run uses only synthetic
buffers, including literal IEEE754 fixtures that distinguish two-step float32
calibration from double or fused evaluation. It checks unchanged generic
`int16-le` behavior, raw deinterleaving, neutral counter metadata, explicit and
unknown acquisition time, malformed pulse rejection, and size bounds.

An optional local fixture mode compares a retained raw entry with legacy
per-channel volts goldens, without contacting Redis or hardware:

```sh
xrm_event_demux_tests --fixture PROFILE_YAML RAW_FILE GOLDEN_DIRECTORY
```

The profile must select the sixteen-channel ES format and provide the exact
native calibration constants. The command loads its first raw source, decodes
the file at a fixed test Redis ID, and compares every raw channel with the ADC
bytes after the header. For each physical channel number, the golden directory
must contain `CH<n>-VOLTS.bin` from the same retained entry. Each legacy golden
must contain exactly one extra leading float32 value: the ES frame previously
misdecoded as ADC. The test removes those first four bytes, then requires every
remaining volts byte to match. It also checks unchanged unsigned event
counters and unknown acquisition time. Real fixture data stays outside the
repository and is never needed by the default CI test.
