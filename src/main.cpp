#include <csignal>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <pvxs/log.h>

#include "redis_pvxs_ioc/app.h"
#include "redis_pvxs_ioc/config_diff.h"
#include "redis_pvxs_ioc/version.h"

namespace {

volatile std::sig_atomic_t g_stopRequested = 0;
volatile std::sig_atomic_t g_reloadRequested = 0;

void signalHandler(const int signalNumber) {
  if (signalNumber == SIGHUP) {
    g_reloadRequested = 1;
  } else {
    g_stopRequested = 1;
  }
}

void printUsage(const char* executable) {
  std::cerr << "Usage: " << executable << " [--config <path>] [--check-config <path> [--json]] "
            << "[--diff-config <old> <new> [--json]] [--version]\n";
}

void printVersion() {
  std::cout << "redis-pvxs-ioc " << REDIS_PVXS_IOC_VERSION;
  if (std::string(REDIS_PVXS_IOC_GIT_REVISION) != "unknown") {
    std::cout << " (" << REDIS_PVXS_IOC_GIT_REVISION << ")";
  }
  std::cout << '\n';
}

}  // namespace

int main(int argc, char* argv[]) {
  pvxs::logger_config_env();

  std::string configPath = "/etc/redis-pvxs-ioc/config.yaml";
  bool checkOnly = false;
  bool json = false;
  std::string oldConfig, newConfig;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if ((argument == "--config" || argument == "--check-config") && index + 1 < argc) {
      configPath = argv[++index];
      checkOnly = (argument == "--check-config");
    } else if (argument == "--diff-config" && index + 2 < argc) {
      oldConfig = argv[++index];
      newConfig = argv[++index];
    } else if (argument == "--json") {
      json = true;
    } else if (argument == "--help" || argument == "-h") {
      printUsage(argv[0]);
      return 0;
    } else if (argument == "--version") {
      printVersion();
      return 0;
    } else {
      printUsage(argv[0]);
      return 1;
    }
  }

  if ((json && !checkOnly && oldConfig.empty()) || (checkOnly && !oldConfig.empty())) {
    printUsage(argv[0]);
    return 1;
  }
  if (!oldConfig.empty()) {
    try {
      const auto before = redis_pvxs_ioc::loadConfigFile(oldConfig);
      const auto after = redis_pvxs_ioc::loadConfigFile(newConfig);
      std::cout << redis_pvxs_ioc::formatConfigDiff(redis_pvxs_ioc::diffConfigs(before, after), json) << '\n';
      return 0;
    } catch (const std::exception& error) {
      if (json) std::cout << "{\"valid\":false,\"error\":" << redis_pvxs_ioc::quoteJson(error.what()) << "}\n";
      else std::cerr << error.what() << '\n';
      return 1;
    }
  }

  redis_pvxs_ioc::Application app(configPath);

  if (checkOnly) {
    std::string summary;
    std::string error;
    redis_pvxs_ioc::AppConfig config;
    if (!app.validateOnly(summary, error, &config)) {
      if (json) std::cout << "{\"valid\":false,\"error\":" << redis_pvxs_ioc::quoteJson(error) << "}\n";
      else std::cerr << error << '\n';
      return 1;
    }
    if (json) {
      std::cout << "{\"valid\":true,\"schema_version\":" << config.schemaVersion
                << ",\"legacy_input\":" << (config.legacyInput ? "true" : "false")
                << ",\"pv_count\":" << config.pvs.size() << ",\"rpc_service_count\":" << config.rpcServices.size()
                << ",\"summary\":" << redis_pvxs_ioc::quoteJson(summary) << "}\n";
    } else std::cout << summary << '\n';
    return 0;
  }

  std::signal(SIGINT, signalHandler);
  std::signal(SIGTERM, signalHandler);
  std::signal(SIGHUP, signalHandler);

  std::string error;
  if (!app.start(error)) {
    std::cerr << error << '\n';
    return 1;
  }

  while (!g_stopRequested) {
    if (g_reloadRequested) {
      g_reloadRequested = 0;
      app.requestReload();
    }
    app.pump();
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
  }

  app.stop();
  return 0;
}
