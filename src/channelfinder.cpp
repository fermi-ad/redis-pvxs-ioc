#include "redis_pvxs_ioc/channelfinder.h"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <set>
#include <sstream>
#include <stdexcept>

namespace redis_pvxs_ioc {
namespace {

constexpr unsigned short kDefaultPvaServerPort = 5075;

void addUniqueTag(std::vector<std::string>& tags, std::set<std::string>& seen, const std::string& tag) {
  if (!tag.empty() && seen.insert(tag).second) {
    tags.push_back(tag);
  }
}

std::string jsonEscape(const std::string& input) {
  std::ostringstream output;
  for (const unsigned char ch : input) {
    switch (ch) {
    case '"': output << "\\\""; break;
    case '\\': output << "\\\\"; break;
    case '\b': output << "\\b"; break;
    case '\f': output << "\\f"; break;
    case '\n': output << "\\n"; break;
    case '\r': output << "\\r"; break;
    case '\t': output << "\\t"; break;
    default:
      if (ch < 0x20) {
        output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << static_cast<int>(ch)
               << std::dec << std::setfill(' ');
      } else {
        output << static_cast<char>(ch);
      }
      break;
    }
  }
  return output.str();
}

void appendJsonString(std::ostringstream& output, const std::string& value) {
  output << '"' << jsonEscape(value) << '"';
}

bool endsWith(const std::string& input, const std::string& suffix) {
  return input.size() >= suffix.size() &&
         input.compare(input.size() - suffix.size(), suffix.size(), suffix) == 0;
}

std::string trimTrailingSlash(std::string url) {
  while (!url.empty() && url.back() == '/') {
    url.pop_back();
  }
  return url;
}

}  // namespace

std::vector<ChannelFinderChannel> buildChannelFinderChannels(const AppConfig& config,
                                                             const std::string& syncTime) {
  std::vector<ChannelFinderChannel> channels;
  channels.reserve(config.pvs.size());

  const auto pvaPort = config.server.tcpPort.value_or(kDefaultPvaServerPort);

  for (const auto& pv : config.pvs) {
    ChannelFinderChannel channel;
    channel.name = fullPVName(config.server, pv);
    channel.owner = config.channelFinder.owner;

    std::set<std::string> seenTags;
    for (const auto& tag : config.channelFinder.tags) {
      addUniqueTag(channel.tags, seenTags, tag);
    }
    addUniqueTag(channel.tags, seenTags, "redis-pvxs-ioc");
    addUniqueTag(channel.tags, seenTags, "pva");

    channel.properties["source"] = "redis-pvxs-ioc";
    channel.properties["protocol"] = "pva";
    channel.properties["iocName"] = config.server.instance;
    channel.properties["namespace"] = config.server.nameSpace;
    channel.properties["pvStatus"] = "Active";
    channel.properties["time"] = syncTime;
    channel.properties["pvaPort"] = std::to_string(pvaPort);
    channel.properties["type"] = toString(pv.type);
    channel.properties["shape"] = toString(pv.shape);
    channel.properties["description"] = pv.metadata.description;
    channel.properties["units"] = pv.metadata.units;
    if (pv.metadata.precision) {
      channel.properties["precision"] = std::to_string(*pv.metadata.precision);
    }
    channel.properties["redisBackend"] = pv.read.backend;
    channel.properties["redisReadKey"] = pv.read.key;
    if (pv.write) {
      channel.properties["redisWriteKey"] = pv.write->key;
    }
    if (pv.confirm) {
      channel.properties["redisConfirmKey"] = pv.confirm->key;
    }

    for (const auto& entry : config.channelFinder.properties) {
      channel.properties[entry.first] = entry.second;
    }

    channels.push_back(std::move(channel));
  }

  return channels;
}

std::string channelFinderChannelsJson(const std::vector<ChannelFinderChannel>& channels) {
  std::ostringstream output;
  output << "[\n";
  for (size_t channelIndex = 0; channelIndex < channels.size(); ++channelIndex) {
    const auto& channel = channels[channelIndex];
    output << "  {\n";
    output << "    \"name\": ";
    appendJsonString(output, channel.name);
    output << ",\n    \"owner\": ";
    appendJsonString(output, channel.owner);
    output << ",\n    \"tags\": [";
    for (size_t tagIndex = 0; tagIndex < channel.tags.size(); ++tagIndex) {
      if (tagIndex != 0u) {
        output << ", ";
      }
      output << "{\"name\": ";
      appendJsonString(output, channel.tags[tagIndex]);
      output << ", \"owner\": ";
      appendJsonString(output, channel.owner);
      output << "}";
    }
    output << "],\n    \"properties\": [";
    bool firstProperty = true;
    for (const auto& property : channel.properties) {
      if (!firstProperty) {
        output << ", ";
      }
      firstProperty = false;
      output << "{\"name\": ";
      appendJsonString(output, property.first);
      output << ", \"owner\": ";
      appendJsonString(output, channel.owner);
      output << ", \"value\": ";
      appendJsonString(output, property.second);
      output << "}";
    }
    output << "]\n  }";
    if (channelIndex + 1u != channels.size()) {
      output << ",";
    }
    output << "\n";
  }
  output << "]\n";
  return output.str();
}

std::string normalizeChannelFinderChannelsUrl(const std::string& url) {
  if (url.empty()) {
    throw std::runtime_error("channelfinder.url must be set when publishing");
  }

  const auto trimmed = trimTrailingSlash(url);
  if (endsWith(trimmed, "/channels")) {
    return trimmed;
  }
  if (endsWith(trimmed, "/resources")) {
    return trimmed + "/channels";
  }
  return trimmed + "/resources/channels";
}

std::string currentChannelFinderTime() {
  const auto now = std::chrono::system_clock::now();
  const auto time = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#if defined(_WIN32)
  gmtime_s(&utc, &time);
#else
  gmtime_r(&time, &utc);
#endif

  std::ostringstream output;
  output << std::put_time(&utc, "%Y-%m-%dT%H:%M:%SZ");
  return output.str();
}

}  // namespace redis_pvxs_ioc
