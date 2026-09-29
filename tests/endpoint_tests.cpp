#include <cassert>
#include <functional>
#include <iostream>

#include "redis_pvxs_ioc/endpoints.h"

using namespace redis_pvxs_ioc;

void rejects(const std::function<void()>& operation, const std::string& name) {
  try {
    operation();
    assert(false && "invalid endpoint was accepted");
  } catch (const std::runtime_error& error) {
    assert(std::string(error.what()).find(name) != std::string::npos);
  }
}

int main() {
  AppConfig config;
  config.server.instance = "test";
  config.server.nameSpace = "TEST";
  config.discovery.enabled = false;
  PVConfig pv;
  pv.name = "value";
  pv.aliases = {"EXTERNAL:value"};
  config.pvs.push_back(pv);
  auto registry = configuredEndpoints(config);
  const auto initialSize = registry.entries().size();
  assert(registry.entries().at("TEST:value").kind == EndpointKind::Value);
  assert(registry.entries().at("EXTERNAL:value").kind == EndpointKind::Alias);
  assert(registry.entries().at("EXTERNAL:value").owner == "TEST:value");

  // Reflection must not hide/replace another endpoint, even with discovery off.
  for (const auto& name : {"TEST:value", "EXTERNAL:value"}) {
    rejects([&] { registry.reserve(name, EndpointKind::RPC, "service/Method"); }, name);
    assert(registry.entries().size() == initialSize);
  }
  for (const auto& name : adminPVNames(config.server)) {
    rejects([&] { registry.reserve(name, EndpointKind::RPC, "service/Method"); }, name);
    auto collision = config;
    collision.pvs[0].aliases = {name};
    rejects([&] { configuredEndpoints(collision); }, name);
  }
  registry.reserve("TEST:METHOD_RPC", EndpointKind::RPC, "one/Method");
  rejects([&] { registry.reserve("TEST:METHOD_RPC", EndpointKind::RPC, "two/Method"); }, "TEST:METHOD_RPC");
  registry.reserve("TEST:METHOD_OTHER", EndpointKind::RPC, "two/Method");

  auto duplicate = config;
  duplicate.pvs.push_back(pv);
  rejects([&] { configuredEndpoints(duplicate); }, "TEST:value");
  duplicate = config;
  duplicate.pvs[0].aliases.push_back("TEST:value");
  rejects([&] { configuredEndpoints(duplicate); }, "TEST:value");
  rejects([&] { registry.reserve("", EndpointKind::RPC, "service/Method"); }, "nonempty");
  rejects([&] { registry.reserve(std::string("TEST:bad\0suffix", 15), EndpointKind::RPC, "service/Method"); }, "NUL");
  config.server.instance = std::string("bad\0instance", 12);
  rejects([&] { configuredEndpoints(config); }, "NUL");
  std::cout << "endpoint collision and diagnostic reservation tests passed\n";
}
