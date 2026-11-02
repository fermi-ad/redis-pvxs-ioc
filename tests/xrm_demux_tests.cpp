#include "xrm_demux.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <functional>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace xrm_demux;

static const std::string configuration = R"YAML(
schema_version: 1
redis: {host: 127.0.0.1}
sources:
  - stream: 'test:input'
    checkpoint: 'test:checkpoint'
    format: int16-le
    layout: sample-major
    samples_per_channel: 3
    start_id: '0-0'
    retention: 10
    timestamp: {mode: absent}
    channels:
      - {channel: 5, raw: 'test:ch5:raw', volts: 'test:ch5:volts', eslo: 0.5, eoff: 1}
      - {channel: 8, raw: 'test:ch8:raw', volts: 'test:ch8:volts', eslo: 2, eoff: -1}
)YAML";

void rejects(const std::function<void()>& operation) {
  bool rejected = false;
  try { operation(); } catch (const std::exception&) { rejected = true; }
  assert(rejected);
}
std::string replace(std::string value, const std::string& old, const std::string& next) {
  const auto position = value.find(old); assert(position != std::string::npos);
  value.replace(position, old.size(), next); return value;
}

int main() {
  const auto config = parseConfig(configuration);
  assert(config.sources.size() == 1 && config.sources[0].channels.size() == 2);
  assert(config.sources[0].channels[0].channel == 5 && config.sources[0].channels[1].channel == 8);
  assert(config.sources[0].fingerprint == parseConfig(configuration).sources[0].fingerprint);
  assert(config.sources[0].fingerprint != parseConfig(replace(configuration, "eslo: 0.5", "eslo: 0.25")).sources[0].fingerprint);

  // Three complete sample-major frames: channel 5 = -32768, 0, 32767;
  // channel 8 = 1, -1, 256. Assert exact binary output, including signed extrema.
  const std::string input("\x00\x80\x01\x00\x00\x00\xff\xff\xff\x7f\x00\x01", 12);
  const auto records = decode(config.sources[0], "1700000000123-7", input, "", "");
  assert(records.size() == 4);
  assert(records[0].key == "test:ch5:raw");
  assert(records[0].fields.at("_") == std::string("\x00\x80\x00\x00\xff\x7f", 6));
  assert(records[2].fields.at("_") == std::string("\x01\x00\xff\xff\x00\x01", 6));
  assert(records[1].fields.at("_") == std::string("\x00\xfc\x7f\xc6\x00\x00\x80\x3f\x00\x01\x80\x46", 12));
  assert(records[3].fields.at("_") == std::string("\x00\x00\x80\x3f\x00\x00\x40\xc0\x00\x80\xff\x43", 12));
  assert(records[0].fields.at("source_id") == "1700000000123-7");
  assert(records[0].fields.at("acquisition_time_valid") == "0");
  assert(!records[0].fields.count("acquisition_time_ns"));
  assert(records[1].fields.at("format") == "float32-le");
  rejects([&] { decode(config.sources[0], "1-0", input.substr(1), "", ""); });
  rejects([&] { decode(config.sources[0], "1-0", input + input, "", ""); });
  rejects([&] { decode(config.sources[0], "1-0", input, "1", "1"); });
  rejects([&] { decode(config.sources[0], "01-0", input, "", ""); });
  rejects([&] { decode(config.sources[0], "0-0", input, "", ""); });

  auto timed = config.sources[0]; timed.timestampMode = "unix-nanoseconds";
  auto output = decode(timed, "1-0", input, "9223372036854775807", "1", true);
  assert(output[0].fields.at("acquisition_time_ns") == "9223372036854775807");
  assert(output[0].fields.at("acquisition_time_valid") == "1");
  assert(output[0].fields.at("source_continuity") == "unknown");
  output = decode(timed, "2-0", input, "-9223372036854775808", "0");
  assert(output[0].fields.at("acquisition_time_ns") == "-9223372036854775808");
  assert(output[0].fields.at("acquisition_time_valid") == "0");
  output = decode(timed, "3-0", input, "1234567890123456789", "1");
  assert(output[0].fields.at("acquisition_time_ns") == "1234567890123456789");
  output = decode(timed, "4-0", input, "1234567890123456788", "1");
  assert(output[0].fields.at("acquisition_time_ns") == "1234567890123456788");
  rejects([&] { decode(timed, "1-0", input, "9223372036854775808", "1"); });
  rejects([&] { decode(timed, "1-0", input, "1.5", "1"); });
  rejects([&] { decode(timed, "1-0", input, "", "1"); });
  rejects([&] { decode(timed, "1-0", input, "1", "true"); });
  timed.channels[0].eslo = std::numeric_limits<float>::max();
  rejects([&] { decode(timed, "1-0", input, "1", "1"); });

  for (const auto& change : std::vector<std::pair<std::string,std::string>>{
      {"format: int16-le", "format: int32-le"},
      {"layout: sample-major", "layout: sample-major-with-spad"},
      {"samples_per_channel: 3", "samples_per_channel: 0"},
      {"samples_per_channel: 3", "samples_per_channel: 524289"},
      {"retention: 10", "retention: 0"},
      {"retention: 10", "retention: 100001"},
      {"eslo: 0.5", "eslo: .nan"},
      {"eslo: 0.5", "eslo: 1.0e99"},
      {"test:ch5:raw", "test:input"},
      {"channel: 8", "channel: 5"},
      {"host: 127.0.0.1", "host: localhost"},
      {"host: 127.0.0.1", "host: 127.0.0.1, port: 4210"},
      {"host: 127.0.0.1", "host: 127.0.0.1, command_timeout_ms: 2001"},
      {"timestamp: {mode: absent}", "timestamp: {mode: unknown}"},
      {"timestamp: {mode: absent}", "timestamp: {mode: absent, field: ts}"},
      {"schema_version: 1", "schema_version: 1\nschema_version: 1"},
      {"layout: sample-major", "layout: sample-major\n    spad: 4"}}) {
    rejects([&] { parseConfig(replace(configuration, change.first, change.second)); });
  }
  rejects([&] { parseConfig(configuration + "\n---\n{}"); });
  rejects([&] { parseConfig(std::string(1024 * 1024 + 1, ' ')); });
  rejects([&] { parseConfig(replace(configuration, "timestamp: {mode: absent}", "timestamp: {mode: unix-nanoseconds, field: _, valid_field: valid}")); });
  // Length-prefix every serialized field, including numeric channel labels;
  // adjacent labels/key lengths must not permit an ambiguous config binding.
  auto bindingA = replace(configuration, "channel: 5", "channel: 1");
  bindingA = replace(bindingA, "test:ch5:raw", "abc25:" + std::string(17,'q'));
  bindingA = replace(bindingA, "test:ch5:volts", "zzzzzz");
  auto bindingB = replace(configuration, "channel: 5", "channel: 12");
  bindingB = replace(bindingB, "test:ch5:raw", "abc");
  bindingB = replace(bindingB, "test:ch5:volts", std::string(17,'q') + "6:zzzzzz");
  assert(parseConfig(bindingA).sources[0].fingerprint != parseConfig(bindingB).sources[0].fingerprint);
  std::cout << "xrm demux unit tests passed\n";
}
