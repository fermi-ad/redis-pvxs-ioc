#pragma once

#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace xrm_demux {

struct EventPulse {
  std::string adcBytes;
  // Names follow the XRM User Guide, section 12.2.2, word offsets 4 and 5.
  // Preserve these raw counters; observed clock semantics remain unqualified.
  uint32_t sampleCounter = 0;
  uint32_t sampleClockCounter = 0;
};

inline constexpr size_t kEventPulseChannels = 16;
inline constexpr size_t kEventPulseFrameBytes = 32;
inline constexpr size_t kEventPulseMaxBytes = 1024u * 1024u;

namespace event_detail {

inline uint32_t littleEndianWord(const std::string& bytes, size_t offset) {
  return static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset])) |
      (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 1])) << 8) |
      (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 2])) << 16) |
      (static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + 3])) << 24);
}

inline bool hasEventPrefix(const std::string& bytes, size_t offset) {
  const auto magic = littleEndianWord(bytes, offset);
  return (magic & UINT32_C(0xfffffff0)) == UINT32_C(0xaa55f150) &&
      littleEndianWord(bytes, offset + 4) == magic &&
      littleEndianWord(bytes, offset + 8) == magic &&
      littleEndianWord(bytes, offset + 12) == magic;
}

}  // namespace event_detail

// Only the qualified 16-channel packed-int16 profile is accepted. The caller
// supplies the ADC frame count after the one leading 32-byte ES frame. Invalid
// bounds, unsupported headers and ambiguous embedded events throw
// std::invalid_argument, without returning a partial waveform.
inline EventPulse decodeEventPulse(const std::string& payload, size_t channels,
                                  size_t samplesPerChannel) {
  if (channels != kEventPulseChannels)
    throw std::invalid_argument("XRM ES pulse requires exactly 16 packed-int16 channels");
  constexpr auto maximumSamples = kEventPulseMaxBytes / kEventPulseFrameBytes - 1;
  if (samplesPerChannel == 0 || samplesPerChannel > maximumSamples)
    throw std::invalid_argument("XRM ES ADC sample count must be 1..32767");
  // Bound the count before adding or multiplying, including SIZE_MAX inputs.
  const auto expectedBytes = (samplesPerChannel + 1) * kEventPulseFrameBytes;
  if (payload.size() != expectedBytes)
    throw std::invalid_argument("XRM ES pulse size does not match one header plus configured ADC frames");
  for (size_t offset = 0; offset < 16; offset += 4) {
    if (event_detail::littleEndianWord(payload, offset) != UINT32_C(0xaa55f154))
      throw std::invalid_argument("XRM ES pulse requires four little-endian 0xaa55f154 magic words");
  }
  const auto sampleCounter = event_detail::littleEndianWord(payload, 16);
  const auto sampleClockCounter = event_detail::littleEndianWord(payload, 20);
  if (sampleCounter != event_detail::littleEndianWord(payload, 24) ||
      sampleClockCounter != event_detail::littleEndianWord(payload, 28))
    throw std::invalid_argument("XRM ES pulse counter pairs do not match");

  // Fail closed on any additional recognizable ES prefix, even unaligned or
  // with a truncated/malformed counter area. Mixed bursts must not become ADC
  // samples. All reads are bounded by the 16-byte prefix and the 1 MiB input.
  for (size_t offset = kEventPulseFrameBytes; offset + 16 <= payload.size(); ++offset) {
    if (event_detail::hasEventPrefix(payload, offset))
      throw std::invalid_argument("XRM ES pulse contains an additional event signature");
  }

  return {payload.substr(kEventPulseFrameBytes), sampleCounter, sampleClockCounter};
}

}  // namespace xrm_demux
