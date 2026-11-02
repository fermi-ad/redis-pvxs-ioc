#include "xrm_demux.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kEventFormat = "acq4xx-es-int16-le";
constexpr const char* kSourceId = "1700000000123-7";
constexpr size_t kChannels = 16, kSamples = 3;
using Frames = std::array<std::array<int16_t, kChannels>, kSamples>;
const Frames kADC{{
    {{-3, -32768, 32767, -1, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11}},
    {{-32767, 100, -100, 256, -256, 1024, -1024, 7, -7, 4096, -4096, 32767, -32768, 2, -2, 0}},
    {{3, -1, 1, -2, 2, -3, 3, -4, 4, -5, 5, -6, 6, -7, 7, -8}}
}};

void check(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}

template<typename F> void rejects(F action, const std::string& contains = "") {
  try {
    action();
  } catch (const std::runtime_error& error) {
    check(contains.empty() || std::string(error.what()).find(contains) != std::string::npos,
          "unexpected rejection: " + std::string(error.what()));
    return;
  }
  throw std::runtime_error("expected a runtime/input rejection containing '" + contains + "'");
}

std::string changed(std::string value, const std::string& before, const std::string& after) {
  const auto position = value.find(before);
  check(position != std::string::npos, "missing fixture replacement: " + before);
  value.replace(position, before.size(), after);
  return value;
}

void appendU32(std::string& bytes, uint32_t value) {
  for (unsigned shift = 0; shift < 32; shift += 8)
    bytes.push_back(static_cast<char>((value >> shift) & 0xffu));
}

void appendI16(std::string& bytes, int16_t value) {
  const auto bits = static_cast<uint16_t>(value);
  bytes.push_back(static_cast<char>(bits & 0xffu));
  bytes.push_back(static_cast<char>(bits >> 8));
}

void appendFloat(std::string& bytes, float value) {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE754 float32 required");
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  appendU32(bytes, bits);
}

uint32_t u32At(const std::string& bytes, size_t offset) {
  check(offset <= bytes.size() && bytes.size() - offset >= 4, "truncated fixture u32");
  uint32_t value = 0;
  for (unsigned byte = 0; byte < 4; ++byte)
    value |= static_cast<uint32_t>(static_cast<unsigned char>(bytes[offset + byte])) << (8 * byte);
  return value;
}

std::string eventHeader(uint32_t first = UINT32_MAX, uint32_t second = UINT32_C(0x80000000),
                        uint32_t magic = UINT32_C(0xaa55f154)) {
  std::string result;
  for (unsigned word = 0; word < 4; ++word) appendU32(result, magic);
  appendU32(result, first); appendU32(result, second);
  appendU32(result, first); appendU32(result, second);
  return result;
}

std::string adcPayload() {
  std::string result;
  for (const auto& frame : kADC)
    for (const auto value : frame) appendI16(result, value);
  return result;
}

std::string profile(size_t channels = kChannels, size_t samples = kSamples,
                    const std::string& format = kEventFormat) {
  std::ostringstream yaml;
  yaml << "schema_version: 1\nredis: {host: 127.0.0.1}\nsources:\n"
       << "  - stream: '{TEST}:raw'\n    checkpoint: '{TEST}:event-checkpoint'\n"
       << "    format: " << format << "\n    layout: sample-major\n"
       << "    samples_per_channel: " << samples << "\n    start_id: '0-0'\n"
       << "    retention: 10\n    timestamp: {mode: absent}\n    channels:\n";
  for (size_t channel = 1; channel <= channels; ++channel) {
    yaml << "      - {channel: " << channel << ", raw: '{TEST}:CH" << channel
         << "-raw', volts: '{TEST}:CH" << channel << "-VOLTS', eslo: "
         << (channel == 1 ? "0.1" : "0.5") << ", eoff: "
         << (channel == 1 ? "0.1" : std::to_string(channel)) << "}\n";
  }
  return yaml.str();
}

void compareBytes(const std::string& actual, const std::string& expected, const std::string& description) {
  check(actual.size() == expected.size(), description + " size differs: actual " +
      std::to_string(actual.size()) + ", expected " + std::to_string(expected.size()));
  const auto mismatch = std::mismatch(actual.begin(), actual.end(), expected.begin());
  if (mismatch.first != actual.end())
    throw std::runtime_error(description + " differs at byte " +
        std::to_string(static_cast<size_t>(mismatch.first - actual.begin())));
}

void checkMetadata(const xrm_demux::Record& record, const xrm_demux::Source& source,
                   const xrm_demux::Channel& channel, const std::string& format,
                   uint32_t first, uint32_t second, bool gap = false) {
  const auto& fields = record.fields;
  check(fields.at("schema") == "fermi-ad/xrm-demux:1", "wrong output schema");
  check(fields.at("format") == format, "wrong output binary format");
  check(fields.at("source_stream") == source.stream && fields.at("source_id") == kSourceId,
        "output lost source identity");
  check(fields.at("channel") == std::to_string(channel.channel), "output lost physical channel label");
  check(fields.at("event_counter_0") == std::to_string(first), "first event counter changed");
  check(fields.at("event_counter_1") == std::to_string(second), "second event counter changed");
  check(fields.at("source_continuity") == (gap ? "unknown" : "observed"), "wrong continuity status");
  check(fields.at("acquisition_time_valid") == "0" && !fields.count("acquisition_time_ns"),
        "event counters or Redis ID fabricated acquisition time");
}

void syntheticTests() {
  using xrm_demux::decode;
  using xrm_demux::parseConfig;
  const auto config = parseConfig(profile());
  check(config.sources.size() == 1 && config.sources[0].channels.size() == kChannels,
        "ES profile did not parse all sixteen channels");
  const auto& source = config.sources.front();
  check(source.format == kEventFormat && source.samplesPerChannel == kSamples, "wrong ES profile interpretation");
  check(xrm_demux::Source{}.format == "int16-le", "plain raw Source default changed");
  check(source.fingerprint == parseConfig(profile()).sources[0].fingerprint, "unstable ES fingerprint");
  const auto plainSource = parseConfig(profile(kChannels, kSamples, "int16-le")).sources[0];
  check(source.fingerprint != plainSource.fingerprint, "ES framing not bound to checkpoint fingerprint");
  const auto adc = adcPayload(), pulse = eventHeader() + adc;
  const auto records = decode(source, kSourceId, pulse, "", "");
  check(records.size() == kChannels * 2, "expected raw and volts output for every channel");

  for (size_t channel = 0; channel < kChannels; ++channel) {
    const auto& raw = records[channel * 2];
    const auto& volts = records[channel * 2 + 1];
    check(raw.key == source.channels[channel].raw && volts.key == source.channels[channel].volts,
          "output routes no longer follow configured wire order");
    checkMetadata(raw, source, source.channels[channel], "int16-le", UINT32_MAX, UINT32_C(0x80000000));
    checkMetadata(volts, source, source.channels[channel], "float32-le", UINT32_MAX, UINT32_C(0x80000000));
    std::string expectedRaw, expectedVolts;
    for (const auto& frame : kADC) {
      appendI16(expectedRaw, frame[channel]);
      // For channels 2..16, halves plus small integers are exactly representable.
      if (channel != 0) appendFloat(expectedVolts, static_cast<float>(double(frame[channel]) / 2 + channel + 1));
    }
    if (channel == 0) {
      // Pinned IEEE754 results of float32 multiply, then float32 add for
      // (-3, -32767, 3) * 0.1f + 0.1f. A single double/FMA evaluation differs.
      for (const auto bits : {UINT32_C(0xbe4cccce), UINT32_C(0xc54cc999), UINT32_C(0x3ecccccd)})
        appendU32(expectedVolts, bits);
    }
    compareBytes(raw.fields.at("_"), expectedRaw, "synthetic raw CH" + std::to_string(channel + 1));
    compareBytes(volts.fields.at("_"), expectedVolts, "synthetic volts CH" + std::to_string(channel + 1));
  }
  const auto plain = decode(plainSource, kSourceId, adc, "", "");
  check(u32At(plain[1].fields.at("_"), 0) == UINT32_C(0xbe4ccccd) &&
        u32At(plain[1].fields.at("_"), 4) == UINT32_C(0xc54cc99a), "plain-int16 calibration behavior changed");
  check(!plain[0].fields.count("event_counter_0") && !plain[1].fields.count("event_counter_1"),
        "ADC-only output acquired an event envelope");
  const auto gap = decode(source, kSourceId, pulse, "", "", true);
  checkMetadata(gap.back(), source, source.channels.back(), "float32-le", UINT32_MAX, UINT32_C(0x80000000), true);

  auto reversed = source;
  std::reverse(reversed.channels.begin(), reversed.channels.end());
  const auto reordered = decode(reversed, kSourceId, pulse, "", "");
  check(reordered[0].key == source.channels.back().raw && reordered[0].fields.at("channel") == "16",
        "decoder sorted physical labels instead of preserving wire order");
  compareBytes(reordered[0].fields.at("_"), records[0].fields.at("_"), "reordered first lane raw");

  for (const auto count : {uint32_t(0), UINT32_C(0x80000000), UINT32_MAX}) {
    const auto counters = decode(source, kSourceId, eventHeader(count, UINT32_MAX - count) + adc, "", "");
    for (const auto& record : counters)
      checkMetadata(record, source, source.channels.at(static_cast<size_t>(std::stoul(record.fields.at("channel"))) - 1),
          record.fields.at("format"), count, UINT32_MAX - count);
  }

  rejects([&] { decode(source, kSourceId, adc, "", ""); }, "size");
  rejects([&] { decode(source, kSourceId, pulse.substr(0, pulse.size() - 1), "", ""); }, "size");
  rejects([&] { decode(source, kSourceId, pulse + '\0', "", ""); }, "size");
  rejects([&] { decode(source, "0-0", pulse, "", ""); }, "zero");
  rejects([&] { decode(source, "01-0", pulse, "", ""); }, "canonical");
  for (const auto offset : {size_t(0), size_t(4), size_t(8), size_t(12), size_t(24), size_t(28)}) {
    auto bad = pulse; bad[offset] ^= 1;
    rejects([&] { decode(source, kSourceId, bad, "", ""); }, offset < 16 ? "magic" : "counter pairs");
  }
  for (const auto magic : {UINT32_C(0xaa55f151), UINT32_C(0xaa55f152), UINT32_C(0xaa55f155), UINT32_C(0x54f155aa)})
    rejects([&] { decode(source, kSourceId, eventHeader(1, 2, magic) + adc, "", ""); }, "magic");
  for (const auto offset : {size_t(32), size_t(33), pulse.size() - 32}) {
    auto mixed = pulse; mixed.replace(offset, 32, eventHeader(1, 2));
    rejects([&] { decode(source, kSourceId, mixed, "", ""); }, "additional event");
  }
  auto overflow = source;
  overflow.channels[0].eslo = std::numeric_limits<float>::max();
  rejects([&] { decode(overflow, kSourceId, pulse, "", ""); }, "float32 overflow");

  auto timed = source; timed.timestampMode = "unix-nanoseconds";
  for (const auto& timestamp : {std::string("0"), std::string("-9223372036854775808"), std::string("9223372036854775807")}) {
    const auto explicitTime = decode(timed, kSourceId, pulse, timestamp, "1");
    for (const auto& record : explicitTime)
      check(record.fields.at("acquisition_time_ns") == timestamp && record.fields.at("acquisition_time_valid") == "1",
            "ES extraction changed explicit signed acquisition time");
  }
  const auto invalidTime = decode(timed, kSourceId, pulse, "-7", "0");
  check(invalidTime[0].fields.at("acquisition_time_ns") == "-7" && invalidTime[0].fields.at("acquisition_time_valid") == "0",
        "ES extraction dropped invalid-but-present acquisition time");
  rejects([&] { decode(source, kSourceId, pulse, "0", "1"); }, "unexpected acquisition");
  rejects([&] { decode(timed, kSourceId, pulse, "", "1"); }, "missing timestamp");
  rejects([&] { decode(timed, kSourceId, pulse, "1.0", "0"); }, "int64");
  rejects([&] { decode(timed, kSourceId, pulse, "0", "true"); }, "validity");

  for (const auto count : {size_t(1), size_t(15), size_t(17), size_t(32)})
    rejects([&] { parseConfig(profile(count)); }, "16 channels");
  for (const auto count : {size_t(0), size_t(32768), std::numeric_limits<size_t>::max()})
    rejects([&] { parseConfig(profile(kChannels, count)); });
  for (const auto& replacement : std::vector<std::pair<std::string, std::string>>{
      {"format: acq4xx-es-int16-le", "format: acq4xx-es-int32-le"},
      {"format: acq4xx-es-int16-le", "format: auto"},
      {"layout: sample-major", "layout: channel-major"},
      {"layout: sample-major", "layout: sample-major\n    spad: 4"},
      {"samples_per_channel: 3", "samples_per_channel: -1"},
      {"samples_per_channel: 3", "samples_per_channel: 1.5"},
      {"format: acq4xx-es-int16-le", "format: acq4xx-es-int16-le\n    format: int16-le"}})
    rejects([&] { parseConfig(changed(profile(), replacement.first, replacement.second)); });
  auto badBounds = source; badBounds.samplesPerChannel = std::numeric_limits<size_t>::max();
  rejects([&] { decode(badBounds, kSourceId, pulse, "", ""); }, "1 MiB");
  badBounds = source; badBounds.channels.pop_back();
  rejects([&] { decode(badBounds, kSourceId, pulse, "", ""); }, "16 channels");
  const auto maximum = parseConfig(profile(kChannels, 32767)).sources[0];
  const auto largest = decode(maximum, kSourceId, eventHeader() + std::string(32767 * 32, '\0'), "", "");
  check(largest.size() == 32 && largest[0].fields.at("_").size() == 32767 * 2 &&
      largest[1].fields.at("_").size() == 32767 * 4, "maximum-sized ES pulse lost ADC frames");
}

std::string readBinary(const std::filesystem::path& path, size_t limit) {
  std::ifstream input(path, std::ios::binary);
  check(input.good(), "cannot read fixture: " + path.string());
  std::string bytes;
  char buffer[8192];
  while (input) {
    input.read(buffer, sizeof buffer);
    const auto count = static_cast<size_t>(input.gcount());
    check(count <= limit - bytes.size(), "fixture exceeds size bound: " + path.string());
    bytes.append(buffer, count);
  }
  check(input.eof(), "cannot finish reading fixture: " + path.string());
  return bytes;
}

void retainedFixture(const std::string& configPath, const std::string& rawPath,
                     const std::filesystem::path& goldenDirectory) {
  const auto config = xrm_demux::loadConfig(configPath);
  check(!config.sources.empty(), "fixture profile has no raw source");
  const auto& source = config.sources.front();
  check(source.format == kEventFormat && source.channels.size() == kChannels, "fixture requires the sixteen-channel ES profile");
  const auto payload = readBinary(rawPath, 1024u * 1024u);
  const auto records = xrm_demux::decode(source, kSourceId, payload, "", "");
  check(records.size() == kChannels * 2, "fixture output count differs");
  const auto first = u32At(payload, 16), second = u32At(payload, 20);
  for (size_t lane = 0; lane < source.channels.size(); ++lane) {
    const auto& channel = source.channels[lane];
    const auto& raw = records[lane * 2];
    const auto& volts = records[lane * 2 + 1];
    check(raw.key == channel.raw && volts.key == channel.volts, "fixture output route differs");
    checkMetadata(raw, source, channel, "int16-le", first, second);
    checkMetadata(volts, source, channel, "float32-le", first, second);
    std::string expectedRaw;
    for (size_t sample = 0; sample < source.samplesPerChannel; ++sample)
      expectedRaw.append(payload, 32 + 2 * (sample * kChannels + lane), 2);
    compareBytes(raw.fields.at("_"), expectedRaw, "retained raw CH" + std::to_string(channel.channel));
    const auto goldenPath = goldenDirectory / ("CH" + std::to_string(channel.channel) + "-VOLTS.bin");
    const auto golden = readBinary(goldenPath, (source.samplesPerChannel + 1) * 4);
    check(golden.size() == (source.samplesPerChannel + 1) * 4,
          "legacy golden must contain one misdecoded ES sample plus ADC samples: " + goldenPath.string());
    compareBytes(volts.fields.at("_"), golden.substr(4), "retained volts CH" + std::to_string(channel.channel));
  }
  std::cout << "retained fixture: all sixteen raw/volts channels match after removing the legacy ES sample; counters preserved\n";
}

}  // namespace

int main(int argc, char** argv) {
  if (argc != 1 && !(argc == 5 && std::string(argv[1]) == "--fixture")) {
    std::cerr << "usage: " << argv[0] << " [--fixture PROFILE_YAML RAW_FILE GOLDEN_DIRECTORY]\n";
    return 2;
  }
  try {
    syntheticTests();
    if (argc == 5) retainedFixture(argv[2], argv[3], argv[4]);
    std::cout << "XRM ES demux: sixteen channels, float32 calibration, strict framing, bounds and timing metadata passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "XRM ES demux test failed: " << error.what() << '\n';
    return 1;
  }
}
