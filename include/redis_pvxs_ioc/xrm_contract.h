#pragma once

#include <cstddef>
#include <string>

namespace redis_pvxs_ioc {

inline std::string xrmPublicationGroup(const std::string& stream) {
  const auto pos = stream.rfind(":CH");
  if (pos == std::string::npos || pos + 3 == stream.size()) return stream;
  unsigned channel = 0;
  for (std::size_t i = pos + 3; i < stream.size(); ++i) {
    if (stream[i] < '0' || stream[i] > '9' || channel > 32) return stream;
    channel = channel * 10 + static_cast<unsigned>(stream[i] - '0');
  }
  return channel >= 1 && channel <= 32 ? stream.substr(0, pos) : stream;
}

inline std::string xrmLatestHashKey(const std::string& stream) { return stream + ":latest:v1"; }
inline std::string xrmCommitMarkerKey(const std::string& stream) {
  return xrmPublicationGroup(stream) + ":latest:v1:commit";
}
inline std::string xrmCommitStreamKey(const std::string& stream) {
  return xrmPublicationGroup(stream) + ":latest:v1:commits";
}

}  // namespace redis_pvxs_ioc
