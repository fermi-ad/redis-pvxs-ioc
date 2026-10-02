#include <cassert>
#include <iostream>
#include <string>
#include <pvxs/client.h>
#include <pvxs/data.h>

int main(int argc, char** argv) {
  assert(argc == 2);
  using pvxs::Member;
  using pvxs::TypeCode;
  pvxs::client::Config config;
  config.autoAddrList = false;
  config.addressList = {"127.0.0.1:" + std::string(argv[1])};
  auto client = config.build();
  const auto call = [&](pvxs::Value value) { return client.rpc("RPC:ECHO_RPC", value).exec()->wait(2.); };
  const auto rejected = [&](pvxs::Value value, const std::string& message) {
    try { call(value); assert(false); }
    catch (const std::exception& error) { assert(std::string(error.what()).find(message) != std::string::npos); }
  };
  auto flat = pvxs::TypeDef(TypeCode::Struct, {
    Member(TypeCode::Struct, "source", {Member(TypeCode::String, "digitizer")}),
    Member(TypeCode::Struct, "left", {Member(TypeCode::Int32, "index")}),
    Member(TypeCode::Struct, "right", {Member(TypeCode::Int32, "index")}),
    Member(TypeCode::Bool, "flag")}).create();
  flat["source.digitizer"] = "structured-client";
  flat["left.index"] = int32_t(5); flat["right.index"] = int32_t(6); flat["flag"] = true;
  const auto result = call(flat);
  assert(result["request.source.digitizer"].as<std::string>() == "structured-client");
  assert(result["request.left.index"].as<int32_t>() == 5);
  assert(result["request.right.index"].as<int32_t>() == 6);
  assert(result["request.flag"].as<bool>());
  const auto before = result["calls"].as<uint32_t>();

  auto duplicate = pvxs::TypeDef(TypeCode::Struct, {
    Member(TypeCode::Struct, "source", {Member(TypeCode::String, "digitizer")}),
    Member(TypeCode::String, "digitizer")}).create();
  duplicate["source.digitizer"] = "one"; duplicate["digitizer"] = "two";
  rejected(duplicate, "RPC field supplied twice");
  auto array = pvxs::TypeDef(TypeCode::Struct, {Member(TypeCode::Int32A, "numbers")}).create();
  array["numbers"] = pvxs::shared_array<const int32_t>({1, 2});
  rejected(array, "RPC argument must be scalar");
  auto unknown = pvxs::TypeDef(TypeCode::Struct, {
    Member(TypeCode::Struct, "source", {Member(TypeCode::String, "unknown")})}).create();
  unknown["source.unknown"] = "bad";
  rejected(unknown, "unknown RPC field");

  auto uri = pvxs::TypeDef(TypeCode::Struct, "epics:nt/NTURI:1.0", {
    Member(TypeCode::String, "scheme"), Member(TypeCode::String, "path"),
    Member(TypeCode::Struct, "query", {
      Member(TypeCode::Struct, "source", {Member(TypeCode::String, "digitizer")})})}).create();
  uri["scheme"] = "pva"; uri["path"] = "RPC:ECHO_RPC";
  uri["query.source.digitizer"] = "nested-query";
  const auto last = call(uri);
  assert(last["request.source.digitizer"].as<std::string>() == "nested-query");
  assert(last["calls"].as<uint32_t>() == before + 1);
  std::cout << "flat/nested NTURI requests, aliases and non-scalar rejection passed\n";
}
