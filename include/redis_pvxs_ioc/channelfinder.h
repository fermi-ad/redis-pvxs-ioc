#pragma once

#include <map>
#include <string>
#include <vector>

#include "redis_pvxs_ioc/config.h"

namespace redis_pvxs_ioc {

struct ChannelFinderChannel {
  std::string name;
  std::string owner;
  std::vector<std::string> tags;
  std::map<std::string, std::string> properties;
};

std::vector<ChannelFinderChannel> buildChannelFinderChannels(const AppConfig& config,
                                                             const std::string& syncTime);

std::string channelFinderChannelsJson(const std::vector<ChannelFinderChannel>& channels);

std::string normalizeChannelFinderChannelsUrl(const std::string& url);

std::string currentChannelFinderTime();

}  // namespace redis_pvxs_ioc
