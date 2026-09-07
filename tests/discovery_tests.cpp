#include "redis_pvxs_ioc/discovery.h"
#include <cassert>
#include <chrono>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

void rejected(const std::function<void()>& operation) {
  bool failed = false;
  try { operation(); } catch (const std::runtime_error&) { failed = true; }
  assert(failed);
}

int main() {
  DiscoveryConfig config;
  config.bindAddress = "127.0.0.1";
  config.udpPort = 0;
  config.maxRecords = 2;
  config.maxBytes = 1024;
  DiscoveryRecord value{"TEST:value", "epics:nt/NTScalar:1.0", {"TEST:alias"}, {{"DESC", "A value"}}};
  const std::map<std::string, std::string> identity{{"IOCNAME", "test"}, {"PVAS_SERVER_PORT", "5075"}};
  auto first = prepareDiscoveryCatalog(config, 1, identity, {value});
  assert(first->records == 1 && first->aliases == 1 && first->wire.size() < config.maxBytes);
  auto overloaded = value;
  overloaded.aliases.push_back("TEST:overflow");
  rejected([&] { prepareDiscoveryCatalog(config, 2, identity, {overloaded}); });
  overloaded = value;
  overloaded.properties["DESC"] = std::string(1024, 'x');
  rejected([&] { prepareDiscoveryCatalog(config, 2, identity, {overloaded}); });
  overloaded = value;
  overloaded.aliases = {value.name};
  rejected([&] { prepareDiscoveryCatalog(config, 2, identity, {overloaded}); });
  overloaded = value;
  overloaded.name = std::string("bad\0name", 8);
  rejected([&] { prepareDiscoveryCatalog(config, 2, identity, {overloaded}); });

  const auto start = std::chrono::steady_clock::now();
  {
    DiscoveryPublisher publisher(config);
    publisher.publish(first);
    // No receiver: repeated updates keep only the newest desired catalog.
    for (uint64_t generation = 2; generation <= 200; ++generation)
      publisher.publish(prepareDiscoveryCatalog(config, generation, identity, {value}));
    auto status = publisher.status();
    assert(status.desiredGeneration == 200 && status.bytes == first->wire.size());
    assert(status.coalesced == 199 && status.uploads == 0);
    assert(status.records == 1 && status.aliases == 1);
  }
  assert(std::chrono::steady_clock::now() - start < 2s);

  std::cout << "discovery count/byte bounds, duplicate validation, coalescing and cancellation passed\n";
}
