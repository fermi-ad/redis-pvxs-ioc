#include <cstdlib>
#include <charconv>
#include <exception>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

#include <curl/curl.h>

#include "redis_pvxs_ioc/channelfinder.h"
#include "redis_pvxs_ioc/config.h"
#include "redis_pvxs_ioc/secrets.h"

namespace {

struct Options {
  std::string configPath;
  bool dryRun = false;
  bool allowRedirects = false;
  long connectTimeoutMs = 3000;
  long timeoutMs = 10000;
  size_t maxResponseBytes = 1024u * 1024u;
};

void usage(std::ostream& output) {
  output << "Usage: redis-pvxs-channelfinder-sync --config <path> [--dry-run]\n"
         << "  [--connect-timeout-ms <ms>] [--timeout-ms <ms>]\n"
         << "  [--max-response-bytes <bytes>] [--allow-redirects]\n";
}

long boundedNumber(const std::string& value, long maximum, const std::string& option) {
  long parsed = 0;
  const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
  if (result.ec != std::errc{} || result.ptr != value.data() + value.size() || parsed < 1 || parsed > maximum)
    throw std::runtime_error(option + " requires an integer between 1 and " + std::to_string(maximum));
  return parsed;
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
    } else if (arg == "--allow-redirects") {
      options.allowRedirects = true;
    } else if (arg == "--connect-timeout-ms" || arg == "--timeout-ms" || arg == "--max-response-bytes") {
      if (index + 1 >= argc) throw std::runtime_error(arg + " requires a value");
      const auto value = boundedNumber(argv[++index], arg == "--max-response-bytes" ? 64L * 1024L * 1024L : 300000L, arg);
      if (arg == "--connect-timeout-ms") options.connectTimeoutMs = value;
      else if (arg == "--timeout-ms") options.timeoutMs = value;
      else options.maxResponseBytes = static_cast<size_t>(value);
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

struct ResponseBudget {
  size_t limit;
  size_t received = 0;
  bool exceeded = false;
};

size_t captureResponse(char*, size_t size, size_t nmemb, void* userdata) noexcept {
  auto& response = *static_cast<ResponseBudget*>(userdata);
  if (size != 0 && nmemb > (response.limit - response.received) / size) {
    response.exceeded = true;
    return 0;
  }
  const auto bytes = size * nmemb;
  response.received += bytes;
  return bytes; // Do not retain or print arbitrary server response bodies.
}

void checked(CURLcode result) {
  if (result != CURLE_OK)
    throw std::runtime_error(std::string("ChannelFinder HTTP error: ") + curl_easy_strerror(result));
}

struct CurlGlobal {
  CurlGlobal() { checked(curl_global_init(CURL_GLOBAL_DEFAULT)); }
  ~CurlGlobal() { curl_global_cleanup(); }
};

struct Headers {
  curl_slist* value = nullptr;
  ~Headers() { curl_slist_free_all(value); }
  void append(const char* text) {
    auto* next = curl_slist_append(value, text);
    if (!next) throw std::runtime_error("failed to allocate HTTP headers");
    value = next;
  }
};

void publishChannels(const redis_pvxs_ioc::ChannelFinderConfig& config, const std::string& json,
                     const Options& options) {
  const auto url = redis_pvxs_ioc::normalizeChannelFinderChannelsUrl(config.url);
  const auto username = redis_pvxs_ioc::credentialFromEnvironment("CHANNELFINDER_USERNAME");
  const auto password = redis_pvxs_ioc::credentialFromEnvironment("CHANNELFINDER_PASSWORD");
  if (username.empty() != password.empty()) {
    throw std::runtime_error("CHANNELFINDER_USERNAME and CHANNELFINDER_PASSWORD must be set together");
  }
  if (options.allowRedirects && !username.empty())
    throw std::runtime_error("redirects with credentials are not supported; use the final ChannelFinder URL");
  const auto schemeEnd = url.find("://");
  const auto authorityEnd = schemeEnd == std::string::npos ? 0 : url.find('/', schemeEnd + 3);
  if (schemeEnd != std::string::npos &&
      url.substr(schemeEnd + 3, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - schemeEnd - 3).find('@') != std::string::npos)
    throw std::runtime_error("URL credentials are not supported; use the ChannelFinder credential inputs");

  CurlGlobal global;
  std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), &curl_easy_cleanup);
  CURL* curl = handle.get();
  if (curl == nullptr) {
    throw std::runtime_error("failed to initialize libcurl");
  }

  ResponseBudget response{options.maxResponseBytes};
  Headers headers;
  headers.append("Content-Type: application/json");
  headers.append("Accept: application/json");

  checked(curl_easy_setopt(curl, CURLOPT_URL, url.c_str()));
  checked(curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers.value));
  checked(curl_easy_setopt(curl, CURLOPT_POST, 1L));
  checked(curl_easy_setopt(curl, CURLOPT_POSTFIELDS, json.c_str()));
  checked(curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, static_cast<curl_off_t>(json.size())));
  checked(curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, captureResponse));
  checked(curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response));
  checked(curl_easy_setopt(curl, CURLOPT_USERAGENT, "redis-pvxs-channelfinder-sync/1"));
  checked(curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT_MS, options.connectTimeoutMs));
  checked(curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, options.timeoutMs));
  checked(curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L));
  checked(curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, options.allowRedirects ? 1L : 0L));
  checked(curl_easy_setopt(curl, CURLOPT_MAXREDIRS, 3L));
  checked(curl_easy_setopt(curl, CURLOPT_POSTREDIR, static_cast<long>(CURL_REDIR_POST_ALL)));
  checked(curl_easy_setopt(curl, CURLOPT_UNRESTRICTED_AUTH, 0L));
#if LIBCURL_VERSION_NUM >= 0x075500
  checked(curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "http,https"));
  checked(curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS_STR, "https"));
#else
  checked(curl_easy_setopt(curl, CURLOPT_PROTOCOLS, static_cast<long>(CURLPROTO_HTTP | CURLPROTO_HTTPS)));
  checked(curl_easy_setopt(curl, CURLOPT_REDIR_PROTOCOLS, static_cast<long>(CURLPROTO_HTTPS)));
#endif

  if (!username.empty()) {
    checked(curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str()));
    checked(curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str()));
    checked(curl_easy_setopt(curl, CURLOPT_HTTPAUTH, static_cast<long>(CURLAUTH_BASIC)));
  }

  const auto result = curl_easy_perform(curl);
  long status = 0;
  checked(curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status));

  if (response.exceeded)
    throw std::runtime_error("ChannelFinder response exceeded " + std::to_string(options.maxResponseBytes) + " bytes");
  if (result != CURLE_OK) {
    throw std::runtime_error(std::string("ChannelFinder publish failed: ") + curl_easy_strerror(result));
  }
  if (status < 200 || status >= 300) {
    throw std::runtime_error("ChannelFinder publish returned HTTP " + std::to_string(status));
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

    publishChannels(config.channelFinder, json, options);
    std::cout << "published " << channels.size() << " channels to ChannelFinder\n";
    return 0;
  } catch (const std::exception& ex) {
    std::cerr << "redis-pvxs-channelfinder-sync: " << ex.what() << "\n";
    usage(std::cerr);
    return 1;
  }
}
