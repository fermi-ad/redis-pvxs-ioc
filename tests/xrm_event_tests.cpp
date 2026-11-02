#include "xrm_event.h"

#include <cassert>
#include <cstdint>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>

namespace {

void appendWord(std::string& bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes.push_back(static_cast<char>((value >> shift) & 0xffu));
}

std::string header(uint32_t count, uint32_t clock, uint32_t magic = UINT32_C(0xaa55f154)) {
  std::string bytes;
  for (unsigned i = 0; i < 4; ++i) appendWord(bytes, magic);
  appendWord(bytes, count); appendWord(bytes, clock);
  appendWord(bytes, count); appendWord(bytes, clock);
  return bytes;
}

void appendADC(std::string& bytes, int16_t sample) {
  const auto word = static_cast<uint16_t>(sample);
  bytes.push_back(static_cast<char>(word & 0xffu));
  bytes.push_back(static_cast<char>(word >> 8));
}

template<typename F> bool rejects(F action) {
  try {
    action();
    return false;
  } catch (const std::invalid_argument&) {
    return true;
  }
}

}  // namespace

int main() {
  using xrm_demux::decodeEventPulse;
  constexpr size_t channels = 16, samples = 3;
  std::string adc;
  for (size_t frame = 0; frame < samples; ++frame) {
    for (size_t channel = 0; channel < channels; ++channel) {
      const int16_t value = channel == 0 ? std::numeric_limits<int16_t>::min()
          : channel == 1 ? std::numeric_limits<int16_t>::max()
          : static_cast<int16_t>(int(frame * channels + channel) - 24);
      appendADC(adc, value);
    }
  }
  const auto pulse = header(UINT32_C(0x12345678), UINT32_C(0x90abcdef)) + adc;
  const auto decoded = decodeEventPulse(pulse, channels, samples);
  assert(decoded.adcBytes == adc && decoded.adcBytes.size() == samples * channels * 2);
  assert(decoded.sampleCounter == UINT32_C(0x12345678));
  assert(decoded.sampleClockCounter == UINT32_C(0x90abcdef));
  assert(static_cast<unsigned char>(decoded.adcBytes[0]) == 0x00);
  assert(static_cast<unsigned char>(decoded.adcBytes[1]) == 0x80); // -32768
  assert(static_cast<unsigned char>(decoded.adcBytes[2]) == 0xff);
  assert(static_cast<unsigned char>(decoded.adcBytes[3]) == 0x7f); // +32767
  for (const auto count : {uint32_t(0), UINT32_C(0x80000000), UINT32_MAX}) {
    const auto result = decodeEventPulse(header(count, UINT32_MAX - count) + adc, channels, samples);
    assert(result.sampleCounter == count && result.sampleClockCounter == UINT32_MAX - count);
    assert(result.adcBytes == adc); // No sign conversion, wrapping or clock interpretation.
  }

  for (const auto count : {size_t(0), size_t(1), size_t(4), size_t(8), size_t(15),
      size_t(17), size_t(32), size_t(64), std::numeric_limits<size_t>::max()})
    assert(rejects([&] { decodeEventPulse(pulse, count, samples); }));
  for (const auto count : {size_t(0), size_t(32768), std::numeric_limits<size_t>::max()})
    assert(rejects([&] { decodeEventPulse(pulse, channels, count); }));
  for (size_t size = 0; size < 32; ++size)
    assert(rejects([&] { decodeEventPulse(pulse.substr(0, size), channels, samples); }));
  assert(rejects([&] { decodeEventPulse(pulse.substr(0, pulse.size() - 1), channels, samples); }));
  assert(rejects([&] { decodeEventPulse(pulse + '\0', channels, samples); }));
  assert(rejects([&] { decodeEventPulse(pulse, channels, samples + 1); }));

  for (const size_t offset : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(24), size_t(28)}) {
    auto malformed = pulse;
    malformed[offset] ^= 1;
    assert(rejects([&] { decodeEventPulse(malformed, channels, samples); }));
  }
  for (const auto magic : {UINT32_C(0xaa55f151), UINT32_C(0xaa55f152), UINT32_C(0xaa55f155), UINT32_C(0x54f155aa)})
    assert(rejects([&] { decodeEventPulse(header(1, 2, magic) + adc, channels, samples); }));

  // Another event anywhere inside the declared ADC bytes is ambiguous, even
  // if its counters disagree or it is not aligned with an expected frame.
  for (const auto magic : {UINT32_C(0xaa55f150), UINT32_C(0xaa55f151), UINT32_C(0xaa55f152),
      UINT32_C(0xaa55f154), UINT32_C(0xaa55f15f)}) {
    for (const auto offset : {size_t(32), size_t(33), size_t(64)}) {
      auto mixed = pulse;
      mixed.replace(offset, 32, header(99, 123, magic));
      assert(rejects([&] { decodeEventPulse(mixed, channels, samples); }));
    }
  }
  auto partial = pulse;
  partial.replace(partial.size() - 16, 16, header(1, 2).substr(0, 16));
  assert(rejects([&] { decodeEventPulse(partial, channels, samples); }));
  auto mismatchedEmbedded = pulse;
  mismatchedEmbedded.replace(32, 32, header(1, 2)); mismatchedEmbedded[32 + 24] ^= 1;
  assert(rejects([&] { decodeEventPulse(mismatchedEmbedded, channels, samples); }));
  auto oneMagicWord = pulse;
  oneMagicWord.replace(32, 4, header(1, 2).substr(0, 4));
  assert(decodeEventPulse(oneMagicWord, channels, samples).adcBytes == oneMagicWord.substr(32));

  // The observed 512 KiB record contains one ES frame and 16383 ADC frames.
  const auto observed = header(10, 20) + std::string(16383 * 32, '\0');
  assert(observed.size() == 524288);
  assert(decodeEventPulse(observed, channels, 16383).adcBytes.size() == 524256);
  const auto maximum = header(UINT32_MAX, 0) + std::string(32767 * 32, '\0');
  assert(maximum.size() == xrm_demux::kEventPulseMaxBytes);
  assert(decodeEventPulse(maximum, channels, 32767).adcBytes.size() == maximum.size() - 32);

  std::cout << "XRM ES pulse: strict 16-channel framing, exact ADC bytes, raw counters and mixed-event rejection passed\n";
}
