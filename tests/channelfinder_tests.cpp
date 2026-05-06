#include <cassert>
#include <iostream>
#include <stdexcept>
#include <string>

#include "redis_pvxs_ioc/channelfinder.h"
#include "redis_pvxs_ioc/config.h"

using namespace redis_pvxs_ioc;

namespace {

const char* const kConfig = R"YAML(
server:
  instance: demo
  namespace: DEMO
  tcp_port: 15075
redis:
  base_key: demo
  host: redis
  port: 6379
channelfinder:
  url: https://cf.example/ChannelFinder
  owner: cf-owner
  tags:
    - demo
    - pva
  properties:
    area: testbed
pvs:
  - name: source:temperature
    type: float64
    shape: scalar
    read:
      key: temperature:rb
    metadata:
      description: Source temperature
      units: C
      precision: 2
  - name: magnet:current
    type: float64
    shape: scalar
    read:
      key: magnet:current
    write:
      key: magnet:current
    confirm:
      key: magnet:current
)YAML";

bool throwsNormalize(const std::string& url) {
  try {
    static_cast<void>(normalizeChannelFinderChannelsUrl(url));
    return false;
  } catch (const std::exception&) {
    return true;
  }
}

}  // namespace

int main() {
  const auto config = loadConfigString(kConfig);
  assert(config.channelFinder.url == "https://cf.example/ChannelFinder");
  assert(config.channelFinder.owner == "cf-owner");
  assert(config.channelFinder.tags.size() == 2u);
  assert(config.channelFinder.properties.at("area") == "testbed");

  const auto channels = buildChannelFinderChannels(config, "2026-05-06T00:00:00Z");
  assert(channels.size() == 2u);

  const auto& readback = channels[0];
  assert(readback.name == "DEMO:source:temperature");
  assert(readback.owner == "cf-owner");
  assert(readback.properties.at("source") == "redis-pvxs-ioc");
  assert(readback.properties.at("protocol") == "pva");
  assert(readback.properties.at("iocName") == "demo");
  assert(readback.properties.at("namespace") == "DEMO");
  assert(readback.properties.at("pvStatus") == "Active");
  assert(readback.properties.at("time") == "2026-05-06T00:00:00Z");
  assert(readback.properties.at("pvaPort") == "15075");
  assert(readback.properties.at("type") == "float64");
  assert(readback.properties.at("shape") == "scalar");
  assert(readback.properties.at("description") == "Source temperature");
  assert(readback.properties.at("units") == "C");
  assert(readback.properties.at("precision") == "2");
  assert(readback.properties.at("redisBackend") == kDefaultRedisBackendAlias);
  assert(readback.properties.at("redisReadKey") == "temperature:rb");
  assert(readback.properties.at("area") == "testbed");

  const auto& writable = channels[1];
  assert(writable.name == "DEMO:magnet:current");
  assert(writable.properties.at("redisWriteKey") == "magnet:current");
  assert(writable.properties.at("redisConfirmKey") == "magnet:current");

  const auto json = channelFinderChannelsJson(channels);
  assert(json.find("\"name\": \"DEMO:source:temperature\"") != std::string::npos);
  assert(json.find("\"name\": \"redisReadKey\"") != std::string::npos);
  assert(json.find("\"name\": \"redisWriteKey\"") != std::string::npos);
  assert(json.find("\"name\": \"recordType\"") == std::string::npos);

  assert(normalizeChannelFinderChannelsUrl("https://cf.example/ChannelFinder") ==
         "https://cf.example/ChannelFinder/resources/channels");
  assert(normalizeChannelFinderChannelsUrl("https://cf.example/ChannelFinder/resources") ==
         "https://cf.example/ChannelFinder/resources/channels");
  assert(normalizeChannelFinderChannelsUrl("https://cf.example/ChannelFinder/resources/channels") ==
         "https://cf.example/ChannelFinder/resources/channels");
  assert(throwsNormalize(""));

  std::cout << "channelfinder tests passed\n";
  return 0;
}
