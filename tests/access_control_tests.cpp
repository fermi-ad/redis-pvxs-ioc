#include "redis_pvxs_ioc/access_control.h"

#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <string>
#include <thread>

#include <asLib.h>

using namespace redis_pvxs_ioc;

namespace {

class PolicyFile {
public:
  PolicyFile() {
    path = std::filesystem::temp_directory_path() /
           ("redis-pvxs-ioc-access-" + std::to_string(reinterpret_cast<uintptr_t>(this)) + ".acf");
  }
  ~PolicyFile() {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
  }
  void write(const std::string& text) const {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text;
    assert(output.good());
  }
  std::filesystem::path path;
};

AccessConfig enabledConfig(const PolicyFile& file) {
  AccessConfig config;
  config.enabled = true;
  config.file = file.path.string();
  return config;
}

bool validates(const AccessConfig& config,
               const std::set<std::string>& groups,
               std::string* errorResult = nullptr) {
  std::string result;
  std::string error;
  const bool okay = validateAccessPolicy(config, groups, result, error);
  if (errorResult) *errorResult = error;
  return okay;
}

}  // namespace

int main(int argc, char** argv) {
  {
    AccessConfig disabled;
    std::string fingerprint;
    std::string error;
    assert(validateAccessPolicy(disabled, {}, fingerprint, error));
    assert(fingerprint.empty());
  }

  PolicyFile file;
  file.write(R"acf(
UAG(operators) { alice, "role/ops" }
HAG(local) { 127.0.0.1 }
ASG(DEFAULT) { RULE(0, READ) }
ASG(OPERATORS) {
  RULE(0, READ)
  RULE(1, WRITE, TRAPWRITE) { UAG(operators) HAG(local) }
}
)acf");
  auto config = enabledConfig(file);
  assert(validates(config, {"DEFAULT", "OPERATORS"}));

  std::string error;
  assert(!validates(config, {"MISSING"}, &error));
  assert(error.find("MISSING") != std::string::npos);

  file.write("ASG(DEFAULT) { RULE(0, READ) { CALC(\"A=1\") } }\n");
  assert(!validates(config, {"DEFAULT"}, &error));
  assert(error.find("1:") != std::string::npos);
  assert(error.find("CALC") != std::string::npos);

  file.write("ASG(DEFAULT) { RULE(0, READ) { INPA(\"x\") } }\n");
  assert(!validates(config, {"DEFAULT"}, &error));
  assert(error.find("INPA") != std::string::npos);

  file.write("# CALC(ignored)\nASG(DEFAULT) { RULE(0, READ) }\n");
  assert(validates(config, {"DEFAULT"}));

  file.write("ASG(DEFAULT) { RULE(0, READ) } # INPU(ignored)\n");
  assert(validates(config, {"DEFAULT"}));

  file.write("UAG(words) { \"CALC(ignored)\", \"INPA(ignored)\" }\n"
             "ASG(DEFAULT) { RULE(0, READ) }\n");
  assert(validates(config, {"DEFAULT"}));

  file.write("ASG($(GROUP)) { RULE(0, READ) }\n");
  config.macros["GROUP"] = "MACRO_GROUP";
  assert(validates(config, {"MACRO_GROUP"}));

  auto missingMacro = config;
  missingMacro.macros.clear();
  assert(!validates(missingMacro, {"MACRO_GROUP"}, &error));

  const auto firstConfig = config;
  std::string firstFingerprint;
  assert(validateAccessPolicy(firstConfig, {"MACRO_GROUP"}, firstFingerprint, error));
  file.write("ASG($(GROUP)) {\n  RULE(0, READ)\n}\n");
  std::string secondFingerprint;
  assert(validateAccessPolicy(firstConfig, {"MACRO_GROUP"}, secondFingerprint, error));
  assert(firstFingerprint != secondFingerprint);

  file.write(R"acf(
UAG(ops) { alice, "role/operators" }
HAG(local) { 127.0.0.1 }
ASG(DECISION) {
  RULE(0, READ)
  RULE(1, WRITE, TRAPWRITE) { UAG(ops) HAG(local) }
}
)acf");
  config.macros.clear();
  assert(validates(config, {"DECISION"}));
  ASMEMBERPVT member = nullptr;
  assert(asAddMember(&member, "DECISION") == 0);
  auto addClient = [&](const int asl, std::string user, std::string host) {
    ASCLIENTPVT client = nullptr;
    assert(asAddClient(&client, member, asl, user.data(), host.data()) == 0);
    return client;
  };
  auto alice = addClient(1, "alice", "127.0.0.1");
  auto bob = addClient(1, "bob", "127.0.0.1");
  auto role = addClient(1, "role/operators", "127.0.0.1");
  auto remoteAlice = addClient(1, "alice", "192.0.2.10");
  auto asl0Alice = addClient(0, "alice", "127.0.0.1");
  auto asl0Bob = addClient(0, "bob", "127.0.0.1");
  assert(asCheckGet(alice));
  assert(asCheckPut(alice));
  assert(static_cast<ASGCLIENT*>(alice)->trapMask != 0u);
  assert(!asCheckGet(bob));
  assert(!asCheckPut(bob));
  assert(asCheckPut(role));
  assert(!asCheckPut(remoteAlice));
  assert(asCheckGet(asl0Alice));
  assert(asCheckPut(asl0Alice));
  assert(asCheckGet(asl0Bob));
  assert(!asCheckPut(asl0Bob));
  bool aggregateWrite = false;
  bool aggregateTrapWrite = false;
  for (const auto client : {bob, role}) {
    if (asCheckPut(client)) {
      aggregateWrite = true;
      aggregateTrapWrite = aggregateTrapWrite || static_cast<ASGCLIENT*>(client)->trapMask != 0u;
    }
  }
  assert(aggregateWrite);
  assert(aggregateTrapWrite);
  asRemoveClient(&alice);
  asRemoveClient(&bob);
  asRemoveClient(&role);
  asRemoveClient(&remoteAlice);
  asRemoveClient(&asl0Alice);
  asRemoveClient(&asl0Bob);
  assert(asRemoveMember(&member) == 0);

  file.write("ASG(WATCHED) { RULE(0, READ) }\n");
  config.watch.enabled = true;
  config.watch.intervalMs = 100;
  config.watch.settleMs = 0;
  AccessController watched(config);
  assert(watched.start({"WATCHED"}, error));
  assert(watched.status().generation == 1u);
  file.write("ASG(WATCHED) { RULE(0, WRITE) }\n");
  watched.pump();
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  watched.pump();
  assert(watched.status().generation == 2u);
  file.write("ASG(WATCHED) { RULE(0, READ) { CALC(\"A=1\") } }\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  watched.pump();
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  watched.pump();
  assert(watched.status().generation == 2u);
  assert(watched.status().lastError.find("CALC") != std::string::npos);
  // The same invalid content is suppressed; a later distinct valid replacement
  // is still observed and activated.
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  watched.pump();
  assert(watched.status().generation == 2u);
  file.write("ASG(WATCHED) {\n RULE(0, READ)\n}\n");
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  watched.pump();
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  watched.pump();
  assert(watched.status().generation == 3u);
  const auto beforeReconfigure = watched.status().policyFingerprint;
  file.write("ASG(WATCHED) { RULE(0, WRITE) }\n");
  assert(watched.reconfigure(config, {"WATCHED"}, error));
  assert(watched.status().policyFingerprint != beforeReconfigure);
  file.write("this is no longer a policy\n");
  assert(watched.restorePrevious(error));
  assert(watched.status().policyFingerprint == beforeReconfigure);
  const auto replacement = file.path.string() + ".replacement";
  {
    std::ofstream output(replacement, std::ios::binary | std::ios::trunc);
    output << "ASG(WATCHED) { RULE(0, WRITE) }\n";
    assert(output.good());
  }
  std::filesystem::rename(replacement, file.path);
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  watched.pump();
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  watched.pump();
  assert(watched.status().generation == 6u);
  std::filesystem::remove(file.path);
  std::this_thread::sleep_for(std::chrono::milliseconds(110));
  watched.pump();
  assert(watched.status().generation == 6u);
  assert(watched.status().lastError.find("cannot open") != std::string::npos);
  auto disabledReload = config;
  disabledReload.enabled = false;
  assert(!watched.reconfigure(disabledReload, {"WATCHED"}, error));
  assert(error.find("immutable") != std::string::npos);

  if (argc > 1) {
    const auto demo = loadConfigFile(argv[1]);
    std::set<std::string> groups{
      demo.access.defaults.adminRead.asg,
      demo.access.defaults.adminWrite.asg,
    };
    for (const auto& pv : demo.pvs) groups.insert(pv.access->asg);
    for (const auto& rpc : demo.rpcServices) groups.insert(rpc.access->asg);
    std::string demoFingerprint;
    assert(validateAccessPolicy(demo.access, groups, demoFingerprint, error));
    assert(!demoFingerprint.empty());
  }

  return 0;
}
