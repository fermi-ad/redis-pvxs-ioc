#ifndef PVXS_ENABLE_EXPERT_API
#define PVXS_ENABLE_EXPERT_API
#endif
#include "redis_pvxs_ioc/access_control.h"
#include "redis_pvxs_ioc/gateway_pv.h"

#include <atomic>
#include <cassert>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/socket.h>

#include <pvxs/data.h>
#include <pvxs/nt.h>
#include <pvxs/server.h>
#include <pvxs/sharedArray.h>

using namespace redis_pvxs_ioc;
using namespace std::chrono_literals;

template<typename F> void eventuallyAt(int line, F predicate) {
  const auto deadline = std::chrono::steady_clock::now() + 8s;
  while (!predicate()) {
    if (std::chrono::steady_clock::now() >= deadline)
      std::fprintf(stderr, "gateway test timed out at line %d\n", line);
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::sleep_for(5ms);
  }
}
#define eventually(...) eventuallyAt(__LINE__, __VA_ARGS__)

template<typename T> pvxs::shared_array<const T> array(std::initializer_list<T> values) {
  pvxs::shared_array<T> result(values.size());
  std::copy(values.begin(), values.end(), result.begin());
  return result.freeze();
}

pvxs::Value table() {
  using pvxs::Member;
  using pvxs::TypeCode;
  auto result = pvxs::TypeDef(TypeCode::Struct, "epics:nt/NTTable:1.0", {
      Member(TypeCode::StringA, "labels"),
      Member(TypeCode::Struct, "value", {
          Member(TypeCode::Float64A, "reading"), Member(TypeCode::StringA, "name")}),
      Member(TypeCode::Struct, "timeStamp", {
          Member(TypeCode::Int64, "secondsPastEpoch"), Member(TypeCode::Int32, "nanoseconds")}),
      Member(TypeCode::Struct, "record", {
          Member(TypeCode::Struct, "_options", {
              Member(TypeCode::Int32, "queueSize"), Member(TypeCode::Bool, "atomic")})}),
      Member(TypeCode::Any, "extra"),
      Member(TypeCode::StructA, "records", {
          Member(TypeCode::String, "name"), Member(TypeCode::Int32A, "samples")})
  }).create();
  result["labels"] = array<std::string>({"Reading", "Name"});
  result["value.reading"] = array<double>({1.25, 2.5});
  result["value.name"] = array<std::string>({"first", "second"});
  result["timeStamp.secondsPastEpoch"] = int64_t(1234567);
  result["timeStamp.nanoseconds"] = int32_t(123);
  // Model the initial update from an EPICS group monitor, which carries its
  // negotiated queue size and atomic behavior inside the returned structure.
  result["record._options.queueSize"] = int32_t(4);
  result["record._options.atomic"] = true;
  auto extra = pvxs::TypeDef(TypeCode::String).create();
  extra.from(std::string("arbitrary value"));
  result["extra"].from(extra);
  pvxs::shared_array<pvxs::Value> records(1);
  records[0] = result["records"].allocMember();
  records[0]["name"] = std::string("sample");
  records[0]["samples"] = array<int32_t>({7, 8});
  result["records"] = records.freeze();
  return result;
}

struct MonitorGate {
  std::atomic<bool> reject{false};
  std::atomic<unsigned> rejected{0};
};

// The source fixture models EPICS group GET semantics independently of the
// monitor snapshot: GET has no monitor queue and reflects record[atomic=...].
class RequestMetadataExec final : public pvxs::server::ExecOp {
public:
  RequestMetadataExec(std::unique_ptr<pvxs::server::ExecOp> target, bool atomic)
      : ExecOp(target->name(), target->credentials(), target->op(), target->pvRequest()),
        target_(std::move(target)), atomic_(atomic) {}
  void reply() override { target_->reply(); }
  void reply(const pvxs::Value& value) override {
    auto response = value.clone();
    if (response["record._options.queueSize"]) response["record._options.queueSize"] = int32_t(0);
    if (response["record._options.atomic"]) response["record._options.atomic"] = atomic_;
    target_->reply(response);
  }
  void error(const std::string& message) override { target_->error(message); }
  void logRemote(pvxs::Level level, const std::string& message) override { target_->logRemote(level, message); }
  void onCancel(std::function<void()>&& fn) override { target_->onCancel(std::move(fn)); }
private:
  pvxs::Timer _timerOneShot(double delay, std::function<void()>&& fn) override {
    return target_->timerOneShot(delay, std::move(fn));
  }
  std::unique_ptr<pvxs::server::ExecOp> target_;
  bool atomic_;
};

class RequestMetadataConnect final : public pvxs::server::ConnectOp {
public:
  explicit RequestMetadataConnect(std::unique_ptr<pvxs::server::ConnectOp> target)
      : ConnectOp(target->name(), target->credentials(), target->op(), target->pvRequest()),
        target_(std::move(target)) {
    target_->pvRequest()["record._options.atomic"].as(atomic_);
  }
  void connect(const pvxs::Value& value) override { target_->connect(value); }
  void error(const std::string& message) override { target_->error(message); }
  void logRemote(pvxs::Level level, const std::string& message) override { target_->logRemote(level, message); }
  void onClose(std::function<void(const std::string&)>&& fn) override { target_->onClose(std::move(fn)); }
  void onGet(std::function<void(std::unique_ptr<pvxs::server::ExecOp>&&)>&& fn) override {
    const bool atomic = atomic_;
    target_->onGet([atomic, fn = std::move(fn)](std::unique_ptr<pvxs::server::ExecOp>&& op) {
      fn(std::make_unique<RequestMetadataExec>(std::move(op), atomic));
    });
  }
  void onPut(std::function<void(std::unique_ptr<pvxs::server::ExecOp>&&, pvxs::Value&&)>&& fn) override {
    target_->onPut(std::move(fn));
  }
private:
  std::unique_ptr<pvxs::server::ConnectOp> target_;
  bool atomic_ = true;
};

class GatedChannel final : public pvxs::server::ChannelControl {
public:
  GatedChannel(std::unique_ptr<pvxs::server::ChannelControl> target, std::shared_ptr<MonitorGate> gate)
      : ChannelControl(target->name(), target->credentials(), target->op()),
        target_(std::move(target)), gate_(std::move(gate)) {}
  void onOp(std::function<void(std::unique_ptr<pvxs::server::ConnectOp>&&)>&& fn) override {
    target_->onOp([fn = std::move(fn)](std::unique_ptr<pvxs::server::ConnectOp>&& op) {
      fn(std::make_unique<RequestMetadataConnect>(std::move(op)));
    });
  }
  void onRPC(std::function<void(std::unique_ptr<pvxs::server::ExecOp>&&, pvxs::Value&&)>&& fn) override {
    target_->onRPC(std::move(fn));
  }
  void onSubscribe(std::function<void(std::unique_ptr<pvxs::server::MonitorSetupOp>&&)>&& fn) override {
    auto gate = gate_;
    target_->onSubscribe([gate, fn = std::move(fn)](std::unique_ptr<pvxs::server::MonitorSetupOp>&& op) {
      if (gate->reject) { ++gate->rejected; op->error("upstream monitor rejected"); }
      else fn(std::move(op));
    });
  }
  void onClose(std::function<void(const std::string&)>&& fn) override { target_->onClose(std::move(fn)); }
  void close() override { target_->close(); }
private:
  void _updateInfo(const std::shared_ptr<const pvxs::server::ReportInfo>& info) override { target_->updateInfo(info); }
  std::unique_ptr<pvxs::server::ChannelControl> target_;
  std::shared_ptr<MonitorGate> gate_;
};

class GatedSource final : public pvxs::server::Source {
public:
  GatedSource(pvxs::server::SharedPV pv, std::shared_ptr<MonitorGate> gate)
      : pv_(std::move(pv)), gate_(std::move(gate)) {}
  void onSearch(Search& search) override {
    for (auto& name : search) if (std::strcmp(name.name(), "upstream:table") == 0) name.claim();
  }
  void onCreate(std::unique_ptr<pvxs::server::ChannelControl>&& channel) override {
    if (channel->name() == "upstream:table") pv_.attach(std::make_unique<GatedChannel>(std::move(channel), gate_));
  }
  List onList() override { return {std::make_shared<const std::set<std::string>>(std::set<std::string>{"upstream:table"}), true}; }
private:
  pvxs::server::SharedPV pv_;
  std::shared_ptr<MonitorGate> gate_;
};

int main() {
  auto upstreamServer = pvxs::server::Config::isolated(AF_INET).build();
  auto localServer = pvxs::server::Config::isolated(AF_INET).build();
  auto upstream = pvxs::server::SharedPV::buildMailbox();
  std::atomic<unsigned> puts{0}, rpcs{0};
  upstream.onPut([&](pvxs::server::SharedPV& pv, std::unique_ptr<pvxs::server::ExecOp>&& op,
                     pvxs::Value&& value) {
    ++puts;
    auto delta = pv.fetch().cloneEmpty();
    delta.assign(value);
    pv.post(delta);
    op->reply();
  });
  upstream.onRPC([&](pvxs::server::SharedPV&, std::unique_ptr<pvxs::server::ExecOp>&& op,
                     pvxs::Value&&) { ++rpcs; op->reply(); });
  const auto initialTable = table();
  upstream.open(initialTable);
  auto monitorGate = std::make_shared<MonitorGate>();
  auto upstreamSource = std::make_shared<GatedSource>(upstream, monitorGate);
  upstreamServer.addSource("source", upstreamSource).start();
  const auto selectedServer = "127.0.0.1:" + std::to_string(upstreamServer.config().tcp_port);
  auto direct = upstreamServer.clientConfig().build();
  const auto directGet = direct.get("upstream:table").server(selectedServer).exec()->wait(5.0);
  assert(directGet["record._options.queueSize"].as<int32_t>() == 0);
  assert(directGet["record._options.atomic"].as<bool>());
  const auto directNonAtomic = direct.get("upstream:table").server(selectedServer)
      .record("atomic", false).exec()->wait(5.0);
  assert(!directNonAtomic["record._options.atomic"].as<bool>());

  // A different server advertises the same name and is the only discovery
  // target of the context. The explicit TCP target must override discovery.
  auto impostorServer = pvxs::server::Config::isolated(AF_INET).build();
  auto impostor = pvxs::server::SharedPV::buildMailbox();
  std::atomic<unsigned> impostorPuts{0};
  impostor.onPut([&](pvxs::server::SharedPV&, std::unique_ptr<pvxs::server::ExecOp>&& op,
                     pvxs::Value&&) { ++impostorPuts; op->reply(); });
  auto impostorValue = table();
  impostorValue["value.reading"] = array<double>({666.0});
  impostor.open(impostorValue);
  auto impostorSource = pvxs::server::StaticSource::build();
  impostorSource.add("upstream:table", impostor);
  impostorServer.addSource("impostor", impostorSource.source()).start();
  auto adversarialContext = impostorServer.clientConfig().build();
  assert(adversarialContext.get("upstream:table").exec()->wait(5.0)
             ["value.reading"].as<pvxs::shared_array<const double>>()[0] == 666.0);

  const auto path = std::filesystem::temp_directory_path() /
      ("redis-pvxs-gateway-" + std::to_string(getpid()) + ".acf");
  const auto policy = [&](const char* rights) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file << "ASG(TEST) { RULE(0, " << rights << ") }\n";
    assert(file.good());
  };
  policy("WRITE");
  AccessConfig accessConfig;
  accessConfig.enabled = true;
  accessConfig.file = path.string();
  auto access = std::make_shared<AccessController>(accessConfig);
  std::string error;
  assert(access->start({"TEST"}, error));

  bool rejectedUnspecified = false, rejectedHostname = false;
  try { GatewayPV::create(adversarialContext, "upstream:table", ""); }
  catch (const std::invalid_argument&) { rejectedUnspecified = true; }
  try { GatewayPV::create(adversarialContext, "upstream:table", "localhost:5075"); }
  catch (const std::invalid_argument&) { rejectedHostname = true; }
  assert(rejectedUnspecified && rejectedHostname);
  auto gateway = GatewayPV::create(adversarialContext, "upstream:table", selectedServer);
  auto plainGateway = GatewayPV::create(adversarialContext, "upstream:table", selectedServer);
  access->addPV("export:table", gateway->sharedPV(), AccessAssignment{"TEST", 0});
  auto plain = pvxs::server::StaticSource::build();
  plain.add("export:plain", plainGateway->sharedPV());
  localServer.addSource("secured", access->source()).addSource("plain", plain.source()).start();
  auto client = localServer.clientConfig().build();
  eventually([&] { return gateway->connected() && plainGateway->connected(); });

  const auto initial = client.get("export:table").exec()->wait(5.0);
  assert(initial.id() == "epics:nt/NTTable:1.0");
  assert(initial["labels"].as<pvxs::shared_array<const std::string>>()[1] == "Name");
  assert(initial["value.reading"].as<pvxs::shared_array<const double>>()[1] == 2.5);
  assert(initial["timeStamp.secondsPastEpoch"].as<int64_t>() == 1234567);
  assert(initial["record._options.queueSize"].as<int32_t>() == 4);
  const auto gatewayNonAtomic = client.get("export:table").record("atomic", false).exec()->wait(5.0);
  assert(gatewayNonAtomic["record._options.queueSize"].as<int32_t>() == 4);
  assert(gatewayNonAtomic["record._options.atomic"].as<bool>());
  assert(initial["extra"].as<pvxs::Value>().as<std::string>() == "arbitrary value");
  const auto records = initial["records"].as<pvxs::shared_array<const pvxs::Value>>();
  assert(records.size() == 1 && records[0]["samples"].as<pvxs::shared_array<const int32_t>>()[1] == 8);

  const auto rejectTablePuts = [&](const std::string& name) {
    bool selectedDenied = false, variantDenied = false;
    try {
      client.put(name).field("value.reading")
          .set("value.reading", array<double>({8.0, 9.0})).exec()->wait(5.0);
    } catch (const pvxs::client::RemoteError& ex) {
      selectedDenied = std::string(ex.what()).find("Read-only PV") != std::string::npos;
    }
    auto extra = pvxs::TypeDef(pvxs::TypeCode::String).create();
    extra.from(std::string("changed"));
    try { client.put(name).field("extra").set("extra", extra).exec()->wait(5.0); }
    catch (const pvxs::client::RemoteError& ex) {
      variantDenied = std::string(ex.what()).find("Read-only PV") != std::string::npos;
    }
    assert(selectedDenied && variantDenied);
    assert(puts == 0 && impostorPuts == 0);
    assert(upstream.fetch()["value.reading"].as<pvxs::shared_array<const double>>()[1] == 2.5);
    const auto cached = client.get(name).exec()->wait(5.0);
    assert(cached["value.reading"].as<pvxs::shared_array<const double>>()[1] == 2.5);
    assert(cached["extra"].as<pvxs::Value>().as<std::string>() == "arbitrary value");
  };
  // One gateway has WRITE-granting ACF; the other has no access wrapper.
  rejectTablePuts("export:table");
  rejectTablePuts("export:plain");
  const auto rejectRpc = [&](const std::string& name) {
    auto request = pvxs::TypeDef(pvxs::TypeCode::Struct, {}).create();
    bool denied = false;
    try { client.rpc(name, request).exec()->wait(5.0); }
    catch (const pvxs::client::RemoteError&) { denied = true; }
    assert(denied && rpcs == 0);
  };
  rejectRpc("export:table");
  rejectRpc("export:plain");

  std::atomic<unsigned> monitorEvents{0};
  auto monitor = client.monitor("export:table").maskDisconnected(false)
      .event([&](pvxs::client::Subscription&) { ++monitorEvents; }).exec();
  eventually([&] { return monitorEvents.load() != 0; });
  eventually([&] { try { return bool(monitor->pop()); } catch (...) { return false; } });
  auto update = initialTable.cloneEmpty();
  update["value.reading"] = array<double>({3.0, 4.0});
  upstream.post(update);
  eventually([&] {
    const auto result = client.get("export:table").exec()->wait(5.0);
    return result["value.reading"].as<pvxs::shared_array<const double>>()[1] == 4.0;
  });
  const auto merged = client.get("export:table").exec()->wait(5.0);
  assert(merged["labels"].as<pvxs::shared_array<const std::string>>()[0] == "Reading");
  assert(merged["timeStamp.nanoseconds"].as<int32_t>() == 123);
  eventually([&] {
    try { auto value = monitor->pop(); return value && value["value.reading"].as<pvxs::shared_array<const double>>()[1] == 4.0; }
    catch (...) { return false; }
  });

  upstream.close();
  eventually([&] { return !gateway->connected() && !plainGateway->connected(); });
  bool disconnected = false;
  eventually([&] {
    try { monitor->pop(); } catch (const pvxs::client::Disconnect&) { disconnected = true; }
    catch (...) {}
    return disconnected;
  });
  auto replacement = pvxs::nt::NTScalar{pvxs::TypeCode::Int32}.create();
  replacement["value"] = int32_t(42);
  upstream.open(replacement);
  eventually([&] { return gateway->connected() && plainGateway->connected(); });
  for (const auto* name : {"export:table", "export:plain"}) {
    const auto reopened = client.get(name).exec()->wait(5.0);
    assert(reopened.id().find("epics:nt/NTScalar:") == 0);
    assert(reopened["value"].as<int32_t>() == 42);
    bool denied = false;
    try { client.put(name).set("value", int32_t(77)).exec()->wait(5.0); }
    catch (const pvxs::client::RemoteError& ex) {
      denied = std::string(ex.what()).find("Read-only PV") != std::string::npos;
    }
    assert(denied && puts == 0 && upstream.fetch()["value"].as<int32_t>() == 42);
    assert(client.get(name).exec()->wait(5.0)["value"].as<int32_t>() == 42);
  }

  policy("NONE");
  assert(access->reload("test", error));
  bool deniedGet = false, deniedMonitor = false;
  try { client.get("export:table").exec()->wait(5.0); }
  catch (const pvxs::client::RemoteError&) { deniedGet = true; }
  auto deniedSub = client.monitor("export:table").event([](pvxs::client::Subscription&) {}).exec();
  eventually([&] {
    try { deniedSub->pop(); } catch (const pvxs::client::RemoteError&) { deniedMonitor = true; }
    catch (...) {}
    return deniedMonitor;
  });
  assert(deniedGet && deniedMonitor && puts == 0);
  policy("WRITE");
  assert(access->reload("test", error));

  // A terminal monitor setup rejection is retried by the application pump.
  monitorGate->reject = true;
  upstream.close();
  eventually([&] { return !gateway->connected() && !plainGateway->connected(); });
  upstream.open(replacement);
  eventually([&] { return monitorGate->rejected.load() >= 1; });
  eventually([&] {
    gateway->pump(); plainGateway->pump();
    return monitorGate->rejected.load() >= 2;
  });
  monitorGate->reject = false;
  eventually([&] {
    gateway->pump(); plainGateway->pump();
    return gateway->connected() && plainGateway->connected();
  });
  assert(client.get("export:table").exec()->wait(5.0)["value"].as<int32_t>() == 42);

  gateway->deactivate();
  plainGateway->deactivate();
  assert(!gateway->connected() && !plainGateway->connected());
  monitor->cancel();
  deniedSub->cancel();
  client.close();
  localServer.stop();
  direct.close();
  adversarialContext.close();
  upstreamServer.stop();
  impostorServer.stop();
  std::filesystem::remove(path);
}
