#include "redis_pvxs_ioc/xrm_acquisition.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

using namespace redis_pvxs_ioc;

namespace {
void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
template<typename F> void rejects(F&& fn) {
  bool failed = false;
  try { fn(); } catch (const std::invalid_argument&) { failed = true; }
  require(failed, "invalid input was accepted");
}
std::string word(uint32_t value, unsigned width) {
  std::string result;
  for (unsigned i = 0; i < width; ++i) result.push_back(char((value >> (8 * i)) & 0xff));
  return result;
}
std::string field(const XRMRedisRecord& record, const std::string& name) {
  const auto found = std::find_if(record.fields.begin(), record.fields.end(), [&](const auto& f) { return f.first == name; });
  require(found != record.fields.end(), "missing field");
  return found->second;
}
bool hasField(const XRMRedisRecord& record, const std::string& name) {
  return std::any_of(record.fields.begin(), record.fields.end(), [&](const auto& f) { return f.first == name; });
}
void layoutAndReconnect() {
  const XRMStreamConfig config{{1, 5, 32}, 4, 2048000, 3};
  XRMFrameAssembler assembler(config);
  std::string input;
  for (uint32_t sample = 0; sample < 6; ++sample)
    for (uint32_t channel : config.channels) input += word((sample << 16) | channel, 4);
  std::vector<XRMBatch> observed;
  for (size_t cursor = 0; cursor < input.size();) {
    const auto length = std::min<size_t>((cursor % 7) + 1, input.size() - cursor);
    assembler.consume(std::string_view(input).substr(cursor, length), [&](XRMBatch&& b) { observed.push_back(std::move(b)); });
    require(assembler.bufferedBytes() < assembler.batchBytes(), "buffer exceeded bound");
    cursor += length;
  }
  require(observed.size() == 2 && assembler.bufferedBytes() == 0, "split input lost batches");
  for (size_t batch = 0; batch < 2; ++batch) {
    require(observed[batch].samples == 3 && observed[batch].channels.size() == 3, "batch dimensions changed");
    for (size_t channel = 0; channel < 3; ++channel) {
      std::string expected;
      for (uint32_t sample = uint32_t(batch * 3); sample < batch * 3 + 3; ++sample)
        expected += word((sample << 16) | config.channels[channel], 4);
      require(observed[batch].channels[channel].channel == config.channels[channel] &&
              observed[batch].channels[channel].bytes == expected, "channel mapping or bytes changed");
    }
  }
  assembler.consume(std::string_view(input).substr(0, 13), [&](XRMBatch&&) { throw std::runtime_error("partial batch emitted"); });
  require(assembler.reset() == 13 && assembler.bufferedBytes() == 0, "reconnect did not discard incomplete batch");
  assembler.consume(std::string_view(input).substr(0, 12), [&](XRMBatch&&) { throw std::runtime_error("partial batch emitted"); });
  require(assembler.reset() == 12, "reconnect retained sample-aligned partial batch");
  assembler.consume(std::string_view(input).substr(36, 36), [&](XRMBatch&& b) { observed.push_back(std::move(b)); });
  require(observed.size() == 3 && observed[2].channels[0].bytes == observed[1].channels[0].bytes,
          "reconnected data joined the preceding partial batch");

  auto unknown = makeXRMRedisRecords(config, observed[0], {}, "device", "adc", "session", 1, 1);
  require(unknown[0].key == "{device}:adc:CH1", "adapter key mismatch");
  require(field(unknown[0], "schema") == kXRMWaveformSchema && field(unknown[0], "sample_bytes") == "4", "wrong schema or width");
  require(field(unknown[0], "acq_time_valid") == "0" && !hasField(unknown[0], "acq_time_ns") &&
          field(unknown[0], "source_validity") == "unknown", "unknown XRM time or validity was invented");
  XRMAcquisitionMetadata earlier{2000000100, true, XRMSourceValidity::Valid};
  XRMAcquisitionMetadata later{1000000100, false, XRMSourceValidity::Invalid};
  auto first = makeXRMRedisRecords(config, observed[0], earlier, "device", "adc", "session", 1, 2);
  auto second = makeXRMRedisRecords(config, observed[1], later, "device", "adc", "session", 1, 3);
  require(field(first[0], "acq_time_ns") == "2000000100" && field(second[0], "acq_time_ns") == "1000000100" &&
          field(first[0], "acq_time_valid") == "1" && field(second[0], "acq_time_valid") == "0" &&
          field(second[0], "source_validity") == "invalid", "source metadata was altered");
  require(field(first[0], "batch") == "2" && field(second[0], "batch") == "3", "ordering is not independent from source time");
  rejects([&] { makeXRMRedisRecords(config, observed[0], {std::nullopt, true, XRMSourceValidity::Unknown},
                                   "device", "adc", "session", 1, 4); });
}

void bounds() {
  XRMStreamConfig config{{1}, 2, 1, 1};
  XRMFrameAssembler one(config);
  std::string test = word(0xff80, 2);
  one.consume(test, [&](XRMBatch&& batch) { require(batch.channels[0].bytes == test, "int16 bytes changed"); });
  config.channels.clear(); rejects([&] { XRMFrameAssembler bad(config); });
  config.channels = {2, 1}; rejects([&] { XRMFrameAssembler bad(config); });
  config.channels = {1, 1}; rejects([&] { XRMFrameAssembler bad(config); });
  config.channels = {33}; rejects([&] { XRMFrameAssembler bad(config); });
  config.channels = {1}; config.sampleBytes = 0; rejects([&] { XRMFrameAssembler bad(config); });
  config.sampleBytes = 5; rejects([&] { XRMFrameAssembler bad(config); });
  config.sampleBytes = 3; config.sampleRateHz = 0; rejects([&] { XRMFrameAssembler bad(config); });
  config.sampleRateHz = 1; config.samplesPerBatch = kXRMMaxBatchBytes; rejects([&] { XRMFrameAssembler bad(config); });
  config.samplesPerBatch = 1;
  XRMFrameAssembler three(config); // 24-bit containers are preserved, not guessed/converted.
  three.consume(word(0x123456, 3), [&](XRMBatch&& b) { require(b.channels[0].bytes == word(0x123456, 3), "24-bit word changed"); });
}

void throughput() {
  XRMStreamConfig config{{}, 4, 1000000, 65536};
  for (unsigned c = 1; c <= 32; ++c) config.channels.push_back(c);
  XRMFrameAssembler assembler(config);
  std::string input;
  input.reserve(assembler.batchBytes());
  for (uint32_t sample = 0; sample < config.samplesPerBatch; ++sample)
    for (uint32_t channel : config.channels) input += word((sample << 8) | channel, 4);
  uint64_t bytes = 0, batches = 0;
  const auto start = std::chrono::steady_clock::now();
  for (int repetition = 0; repetition < 8; ++repetition) {
    for (size_t offset = 0; offset < input.size();) {
      const auto count = std::min<size_t>(65521, input.size() - offset);
      assembler.consume(std::string_view(input).substr(offset, count), [&](XRMBatch&& batch) {
        ++batches;
        for (size_t c = 0; c < batch.channels.size(); ++c) {
          const auto& payload = batch.channels[c].bytes;
          require(payload.size() == config.samplesPerBatch * 4 &&
                  payload.substr(0, 4) == word(config.channels[c], 4) &&
                  payload.substr(payload.size() - 4) == word(((config.samplesPerBatch - 1) << 8) | config.channels[c], 4),
                  "throughput run corrupted channel");
        }
      });
      offset += count; bytes += count;
    }
  }
  const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
  require(batches == 8 && assembler.bufferedBytes() == 0, "throughput run lost data");
  require(seconds < 30, "bounded synthetic 64 MiB processing exceeded 30 seconds");
  std::cout << "synthetic demultiplexing: " << bytes / (1024.0 * 1024.0) << " MiB in " << seconds
            << " s (" << bytes / (1024.0 * 1024.0 * seconds) << " MiB/s)\n";
}
}  // namespace

int main() {
  try { layoutAndReconnect(); bounds(); throughput(); }
  catch (const std::exception& error) { std::cerr << "XRM acquisition test: " << error.what() << '\n'; return 1; }
}
