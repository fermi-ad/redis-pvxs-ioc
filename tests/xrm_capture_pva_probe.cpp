// Read-only peer for the isolated TCP capture -> Redis -> IOC qualification.
#include <pvxs/client.h>
#include <pvxs/data.h>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iostream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
void require(bool value, const char* message) {
  if (!value) throw std::runtime_error(message);
}
}

int main(int argc, char** argv) {
  try {
    require(argc == 5, "usage: probe ENDPOINT PV EXPECTED_FLOAT32_LE_FILE UNITS");
    std::ifstream input(argv[3], std::ios::binary);
    require(input.good(), "cannot read expected waveform");
    const std::vector<unsigned char> expected{std::istreambuf_iterator<char>(input), {}};
    require(expected.size() == 16383u * 4u, "unexpected reference waveform size");
    pvxs::client::Config config;
    config.autoAddrList = false;
    config.addressList = {argv[1]};
    config.nameServers.clear();
    auto client = config.build();
    auto value = client.get(argv[2]).exec()->wait(2.0);
    require(value.id() == "epics:nt/NTScalarArray:1.0", "waveform normative type differs");
    require(value["value"].type() == pvxs::TypeCode::Float32A, "waveform numeric type differs");
    auto samples = value["value"].as<pvxs::shared_array<const float>>();
    require(samples.size() == 16383u, "waveform sample count differs");
    for (size_t i = 0; i < samples.size(); ++i) {
      const auto offset = i * 4u;
      const uint32_t wanted = uint32_t(expected[offset]) | (uint32_t(expected[offset + 1]) << 8u) |
          (uint32_t(expected[offset + 2]) << 16u) | (uint32_t(expected[offset + 3]) << 24u);
      uint32_t actual = 0;
      const float sample = samples[i];
      std::memcpy(&actual, &sample, sizeof(actual));
      require(actual == wanted, "waveform payload differs from independent float32 reference");
    }
    require(value["display.units"].as<std::string>() == argv[4], "waveform units differ");
    require(value["alarm.severity"].as<int32_t>() == 3, "unknown acquisition time is not INVALID");
    require(value["timeStamp.secondsPastEpoch"].as<int64_t>() == 0 &&
            value["timeStamp.nanoseconds"].as<int32_t>() == 0, "acquisition timestamp was fabricated");
    client.close();
    std::cout << "capture waveform type, value, units and unavailable time passed\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
