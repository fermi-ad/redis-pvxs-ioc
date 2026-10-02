// Independent short PVA fixture. This is not an IOC or release qualification.
#include <pvxs/server.h>
#include <pvxs/sharedpv.h>
#include <pvxs/nt.h>
#include <chrono>
#include <iostream>
#include <string>
#include <sys/socket.h>
#include <thread>

int main(int argc, char** argv) {
  using namespace pvxs;
  const bool corrupt = argc > 1 && std::string(argv[1]) == "--corrupt";
  auto server = server::Config::isolated(AF_INET).build();
  const auto add = [&](const std::string& name, Value value) {
    auto pv = server::SharedPV::buildReadonly();
    pv.open(value); server.addPV(name, pv); return pv;
  };
  auto scalar = nt::NTScalar{TypeCode::UInt32}.create();
  scalar["value"] = uint32_t(1000);
  scalar["timeStamp.secondsPastEpoch"] = int64_t(1760000000);
  scalar["timeStamp.nanoseconds"] = int32_t(123);
  auto shared = add("Q:scalar", scalar); server.addPV("Q:alias", shared);
  auto array = nt::NTScalar{TypeCode::UInt32A}.create();
  shared_array<uint32_t> data(4096);
  for (size_t i = 0; i < data.size(); ++i) data[i] = 1000 + i;
  if (corrupt) data[4095] = 0;
  array["value"] = data.freeze();
  array["timeStamp.secondsPastEpoch"] = int64_t(1760000000);
  array["timeStamp.nanoseconds"] = int32_t(123);
  add("Q:array", array);
  const auto string = [&](const std::string& suffix, const std::string& text) {
    auto value = nt::NTScalar{TypeCode::String}.create(); value["value"] = text;
    add("SYS:qualification:" + suffix, value);
  };
  string("backend:health", "1/1 connected"); string("config:lastStatus", "generation 1 active"); string("config:lastError", "");
  auto generation = nt::NTScalar{TypeCode::Int64}.create(); generation["value"] = int64_t(1);
  add("SYS:qualification:config:generation", generation);
  auto ready = nt::NTScalar{TypeCode::Bool}.create(); ready["value"] = true;
  add("SYS:qualification:ready", ready);
  auto sources = TypeDef(TypeCode::Struct, {
    Member(TypeCode::Struct, "sources", {
      Member(TypeCode::StringA, "pv"), Member(TypeCode::UInt64A, "readFailures"),
      Member(TypeCode::UInt64A, "readRejections"), Member(TypeCode::UInt64A, "streamResets"),
      Member(TypeCode::UInt64A, "retentionGaps")})}).create();
  shared_array<std::string> names(2); names[0] = "Q:scalar"; names[1] = "Q:array";
  sources["sources.pv"] = names.freeze();
  for (const auto& field : {"readFailures", "readRejections", "streamResets", "retentionGaps"}) {
    shared_array<uint64_t> values(2); values[0] = values[1] = 0;
    sources[std::string("sources.") + field] = values.freeze();
  }
  add("SYS:qualification:source:status", sources);
  server.start();
  std::cout << "PVA_ADDRESS=127.0.0.1:" << server.config().udp_port << std::endl;
  std::this_thread::sleep_for(std::chrono::seconds(20));
}
