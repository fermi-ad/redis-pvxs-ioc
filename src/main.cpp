#include <csignal>
#include <chrono>
#include <iostream>
#include <string>
#include <thread>

#include <pvxs/log.h>

#include "redis_pvxs_ioc/app.h"

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
  std::cerr << "Usage: " << executable << " [--config <path>] [--check-config <path>]\n";
}

}  // namespace

int main(int argc, char* argv[]) {
  pvxs::logger_config_env();

  std::string configPath = "/etc/redis-pvxs-ioc/config.yaml";
  bool checkOnly = false;

  for (int index = 1; index < argc; ++index) {
    const std::string argument = argv[index];
    if ((argument == "--config" || argument == "--check-config") && index + 1 < argc) {
      configPath = argv[++index];
      checkOnly = (argument == "--check-config");
    } else if (argument == "--help" || argument == "-h") {
      printUsage(argv[0]);
      return 0;
    } else {
      printUsage(argv[0]);
      return 1;
    }
  }

  redis_pvxs_ioc::Application app(configPath);

  if (checkOnly) {
    std::string summary;
    std::string error;
    if (!app.validateOnly(summary, error)) {
      std::cerr << error << '\n';
      return 1;
    }
    std::cout << summary << '\n';
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
