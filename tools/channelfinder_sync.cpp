#include <cstdlib>
#include <exception>
#include <iostream>
#include <stdexcept>
#include <string>

#include <curl/curl.h>

#include "redis_pvxs_ioc/channelfinder.h"
#include "redis_pvxs_ioc/config.h"

namespace {

struct Options {
  std::string configPath;
  bool dryRun = false;
};

void usage(std::ostream& output) {
  output << "Usage: redis-pvxs-channelfinder-sync --config <path> [--dry-run]\n";
}

Options parseOptions(const int argc, char* argv[]) {
  Options options;
  for (int index = 1; index < argc; ++index) {
    const std::string arg = argv[index];
    if (arg == "--config") {
      if (index + 1 >= argc) {
        throw std::runtime_error("--config requires a path");
      }
      options.configPath = argv[++index];
    } else if (arg == "--dry-run") {
      options.dryRun = true;
    } else if (arg == "--help" || arg == "-h") {
      usage(std::cout);
      std::exit(0);
    } else {
      throw std::runtime_error("unknown argument: " + arg);
    }
  }

  if (options.configPath.empty()) {
    throw std::runtime_error("--config is required");
  }
  return options;
}

size_t captureResponse(char* ptr, size_t size, size_t nmemb, void* userdata) {
  auto* response = static_cast<std::string*>(userdata);
  response->append(ptr, size * nmemb);
  return size * nmemb;
}

std::string envValue(const char* name) {
  const auto* value = std::getenv(name);
  return value == nullptr ? std::string{} : std::string{value};
}

void publishChannels(const redis_pvxs_ioc::ChannelFinderConfig& config, const std::string& json) {
  const auto url = redis_pvxs_ioc::normalizeChannelFinderChannelsUrl(config.url);
  const auto username = envValue("CHANNELFINDER_USERNAME");
  const auto password = envValue("CHANNELFINDER_PASSWORD");
  if (username.empty() != password.empty()) {
    throw std::runtime_error("CHANNELFINDER_USERNAME and CHANNELFINDER_PASSWORD must be set together");
  }

  curl_global_init(CURL_GLOBAL_DEFAULT);
  CURL* curl = curl_easy_init();
  if (curl == nullptr) {
    curl_global_cleanup();
    throw std::runtime_error("failed to initialize libcurl");
  }

  std::string response;
  struct curl_slist* headers = nullptr;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "Accept: application/json");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POST, 1L);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str());
  curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(json.size()));
  curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, captureResponse);
  curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
  curl_easy_setopt(curl, CURLOPT_USERAGENT, "redis-pvxs-channelfinder-sync/1");
  curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);

  if (!username.empty()) {
    curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str());
    curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_BASIC);
  }

  const auto result = curl_easy_perform(curl);
  long status = 0;
  curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);

  curl_slist_free_all(headers);
  curl_easy_cleanup(curl);
  curl_global_cleanup();

  if (result != CURLE_OK) {
    throw std::runtime_error(std::string("ChannelFinder publish failed: ") + curl_easy_strerror(result));
  }
  if (status < 200 || status >= 300) {
    throw std::runtime_error("ChannelFinder publish returned HTTP " + std::to_string(status) + ": " + response);
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  try {
    const auto options = parseOptions(argc, argv);
    const auto config = redis_pvxs_ioc::loadConfigFile(options.configPath);
    const auto channels = redis_pvxs_ioc::buildChannelFinderChannels(config, redis_pvxs_ioc::currentChannelFinderTime());
    const auto json = redis_pvxs_ioc::channelFinderChannelsJson(channels);

    if (options.dryRun) {
      std::cout << json;
      return 0;
    }

    publishChannels(config.channelFinder, json);
    std::cout << "published " << channels.size() << " channels to ChannelFinder\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "redis-pvxs-channelfinder-sync: " << ex.what() << "\n";
    usage(std::cerr);
    return 1;
  }
}
