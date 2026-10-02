// A bounded independent PVA observation; the Python collector verifies each
// returned acquisition cursor against the corresponding private Redis entry.
#include <pvxs/client.h>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
void require(bool test, const char* error) { if (!test) throw std::runtime_error(error); }
int64_t timestamp(const pvxs::Value& value) {
  const auto seconds = value["timeStamp.secondsPastEpoch"].as<int64_t>();
  const auto nanos = value["timeStamp.nanoseconds"].as<int32_t>();
  require(seconds > 0 && nanos >= 0 && nanos < 1000000000, "invalid source timestamp");
  return seconds * 1000000000 + nanos;
}
std::string quote(const std::string& value) {
  std::string result = "\"";
  for (unsigned char c : value) {
    if (c == '\\' || c == '"') { result += '\\'; result += char(c); }
    else if (c >= 32 && c < 127) result += char(c);
    else { const char* hex = "0123456789abcdef"; result += "\\u00"; result += hex[c >> 4]; result += hex[c & 15]; }
  }
  return result + '"';
}
}

int main(int argc, char** argv) {
  try {
    require(argc == 2 || (argc == 3 && std::string(argv[2]) == "--legacy"), "usage: qualification_probe ADDRESS [--legacy]");
    const bool legacy = argc == 3;
    pvxs::client::Config config;
    config.autoAddrList = false; config.addressList = {argv[1]};
    auto client = config.build();
    const auto get = [&](const std::string& name) { return client.get(name).exec()->wait(2.0); };
    const auto scalar = get("Q:scalar"), alias = get("Q:alias"), array = get("Q:array");
    require(scalar["value"].type() == pvxs::TypeCode::UInt32 && alias["value"].type() == pvxs::TypeCode::UInt32
            && array["value"].type() == pvxs::TypeCode::UInt32A, "wrong primitive PVA type");
    const auto data = array["value"].as<pvxs::shared_array<const uint32_t>>();
    require(data.size() == 4096 && data[0] > 0, "wrong array content/length");
    for (size_t i = 0; i < data.size(); ++i) require(data[i] == data[0] + i, "array corruption");
    const auto a = scalar["value"].as<uint32_t>(), b = alias["value"].as<uint32_t>();
    require(a > 0 && b > 0, "missing scalar/alias content");
    const auto backend = get("SYS:qualification:backend:health")["value"].as<std::string>();
    const auto slash = backend.find('/');
    const auto space = backend.find(' ', slash);
    const bool connected = slash != std::string::npos && space != std::string::npos &&
      backend.substr(0, slash) == backend.substr(slash + 1, space - slash - 1) &&
      backend.find(" connected", space) == space && std::stoul(backend.substr(0, slash)) > 0;
    const auto generation = get("SYS:qualification:config:generation")["value"].as<int64_t>();
    const auto status = get("SYS:qualification:config:lastStatus")["value"].as<std::string>();
    const auto error = get("SYS:qualification:config:lastError")["value"].as<std::string>();
    bool ready = connected;
    uint64_t readFailures = 0, readRejections = 0, resets = 0, gaps = 0;
    if (!legacy) {
      ready = get("SYS:qualification:ready")["value"].as<bool>();
      const auto sources = get("SYS:qualification:source:status");
      const auto names = sources["sources.pv"].as<pvxs::shared_array<const std::string>>();
      for (const auto& field : {"readFailures", "readRejections", "streamResets", "retentionGaps"}) {
        uint64_t total = 0;
        const auto values = sources[std::string("sources.") + field].as<pvxs::shared_array<const uint64_t>>();
        require(values.size() == names.size(), "malformed source diagnostics");
        for (size_t i = 0; i < names.size(); ++i) if (names[i] == "Q:scalar" || names[i] == "Q:array") total += values[i];
        if (std::string(field) == "readFailures") readFailures = total;
        if (std::string(field) == "readRejections") readRejections = total;
        if (std::string(field) == "streamResets") resets = total;
        if (std::string(field) == "retentionGaps") gaps = total;
      }
    }
    std::cout << "{\"scalar\":" << a << ",\"alias\":" << b << ",\"array\":" << data[0]
              << ",\"array_elements\":4096,\"array_exact\":true,\"scalar_time_ns\":" << timestamp(scalar)
              << ",\"alias_time_ns\":" << timestamp(alias) << ",\"array_time_ns\":" << timestamp(array)
              << ",\"scalar_alarm\":" << scalar["alarm.severity"].as<int32_t>()
              << ",\"array_alarm\":" << array["alarm.severity"].as<int32_t>()
              << ",\"generation\":" << generation << ",\"last_status\":" << quote(status)
              << ",\"last_error\":" << quote(error) << ",\"backend_connected\":" << (connected ? "true" : "false")
              << ",\"ready\":" << (ready ? "true" : "false") << ",\"read_failures\":" << readFailures
              << ",\"read_rejections\":" << readRejections << ",\"stream_resets\":" << resets
              << ",\"retention_gaps\":" << gaps << "}\n";
    client.close();
  } catch (const std::exception& error) {
    std::cerr << "qualification PVA observation failed: " << error.what() << '\n'; return 1;
  }
}
