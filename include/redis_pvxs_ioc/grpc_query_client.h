#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace redis_pvxs_ioc {

// Plain-old-data mirrors of the proto messages so callers (the RPC handler)
// need not depend on the generated protobuf headers. The wrapper translates to
// and from the generated types internally.

struct QueryStatus {
  bool ok = false;
  std::string message;
};

struct QueryResult {
  QueryStatus status;
  std::vector<double> values;
  std::vector<int64_t> timesNs;
  int64_t eventTimeNs = 0;
  std::string units;
};

struct OrbitResult {
  QueryStatus status;
  int64_t timeNs = 0;
  std::vector<std::string> hNames;
  std::vector<double> hOrbit;
  std::vector<double> hIntensity;
  std::vector<std::string> vNames;
  std::vector<double> vOrbit;
  std::vector<double> vIntensity;
};

struct SourceArgs {
  std::string digitizer;
  std::string subkey;
};

struct AverageArgs {
  SourceArgs source;
  int64_t startNs = 0;
  int64_t endNs = 0;
  int64_t lengthNs = 0;
  bool perEntryMean = false;
};

struct OnEventArgs {
  SourceArgs source;
  uint32_t event = 0;
  int64_t deltaNs = 0;
};

struct OnEventTimeArgs {
  SourceArgs source;
  int64_t eventTimeNs = 0;
  int64_t startNs = 0;
  int64_t lengthNs = 0;
};

struct OrbitArgs {
  std::string machine;
  std::string section;
  int64_t startNs = 0;
  int64_t endNs = 0;
  int64_t lengthNs = 0;
  int32_t startIndex = 0;
  int32_t endIndex = 0;
};

struct SliceArgs {
  SourceArgs source;
  int64_t endNs = 0;       // window.end_ns selects the entry (0 = now)
  int32_t startIndex = 0;  // first sample index
  int32_t length = 0;      // sample count (0 = to end)
  int32_t stride = 0;      // sample stride (0/1 = every sample)
};

struct DecimateArgs {
  SourceArgs source;
  int64_t endNs = 0;       // window.end_ns selects the entry (0 = now)
  int32_t factor = 0;      // keep every factor-th sample (<=1 = none)
  int32_t maxPoints = 0;   // cap on returned points (0 = no cap)
};

// Thin client over the BpmQuery gRPC service. Holds one insecure channel/stub
// for a given "host:port" endpoint. Methods are synchronous; failures (RPC
// transport errors) are reported via QueryStatus::ok == false with a message.
class GrpcQueryClient {
public:
  explicit GrpcQueryClient(const std::string& endpoint);
  ~GrpcQueryClient();

  GrpcQueryClient(const GrpcQueryClient&) = delete;
  GrpcQueryClient& operator=(const GrpcQueryClient&) = delete;

  const std::string& endpoint() const { return endpoint_; }

  QueryResult average(const AverageArgs& args);
  QueryResult onEvent(const OnEventArgs& args);
  QueryResult onEventTime(const OnEventTimeArgs& args);
  OrbitResult orbit(const OrbitArgs& args);
  QueryResult slice(const SliceArgs& args);
  QueryResult decimate(const DecimateArgs& args);

private:
  std::string endpoint_;
  // pimpl hides the generated gRPC stub/channel types from callers.
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace redis_pvxs_ioc
