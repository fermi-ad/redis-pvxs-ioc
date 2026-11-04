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
  std::atomic<unsigned> rejected{0}, subscriptions{0};
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
      : ConnectOp(target->name(), target->credentials(), target->op(), target->pvRequest(), target->subField()),
        target_(std::move(target)) {
    target_->pvRequest()["record._options.atomic"].as(atomic_);
  }
  void connect(const pvxs::Value& value) override { target_->connect(subField().empty() ? value : value[subField()]); }
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
      ++gate->subscriptions;
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


class NativeSource final : public pvxs::server::Source {
public:
  pvxs::Value put = pvxs::TypeDef(pvxs::TypeCode::Struct, {pvxs::members::Int32("input")}).create();
  pvxs::Value get = pvxs::TypeDef(pvxs::TypeCode::Struct, {pvxs::members::Int64("output")}).create();
  pvxs::Value values = pvxs::TypeDef(pvxs::TypeCode::Int32A).create();
  std::atomic<unsigned> processes{0}, mutations{0};
  pvxs::ProxyRequest last;
  uint64_t length = 5;
  void onSearch(Search& search) override { for (auto& name : search) if (std::strcmp(name.name(), "upstream:native") == 0) name.claim(); }
  void onCreate(std::unique_ptr<pvxs::server::ChannelControl>&& channel) override {
    if (channel->name() != "upstream:native") return;
    channel->onClose([](const std::string&) {});
    channel->onProxy([this](std::unique_ptr<pvxs::server::ProxySetupOp>&& setup) {
      pvxs::ProxyPrototype prototype;
      if (setup->kind() == pvxs::ProxyKind::PutGet) { prototype.put = put; prototype.get = get; }
      else if (setup->kind() == pvxs::ProxyKind::Array) prototype.put = prototype.get = values;
      setup->onExec([this](std::unique_ptr<pvxs::server::ProxyExecOp>&& op, pvxs::ProxyRequest&& request) {
        last = request;
        if (request.mutating()) ++mutations;
        switch (request.action) {
        case pvxs::ProxyAction::Process: ++processes; op->reply(); break;
        case pvxs::ProxyAction::GetPut: { auto value = put.cloneEmpty(); value["input"] = 3; op->reply(value); break; }
        case pvxs::ProxyAction::GetGet:
        case pvxs::ProxyAction::PutGet: {
          auto value = get.cloneEmpty();
          value["output"] = request.action == pvxs::ProxyAction::GetGet ? 77 : request.value["input"].as<int32_t>() * 2;
          op->reply(value); break;
        }
        case pvxs::ProxyAction::ArrayGet: { auto value = values.cloneEmpty(); value = array<int32_t>({11, 33}); op->reply(value); break; }
        case pvxs::ProxyAction::ArrayPut: op->reply(); break;
        case pvxs::ProxyAction::ArraySetLength: length = request.length; op->reply(); break;
        case pvxs::ProxyAction::ArrayGetLength: op->replyLength(length); break;
        }
      });
      setup->connect(prototype);
    });
  }
};

pvxs::Value execute(const std::shared_ptr<pvxs::client::ProxyOperation>& operation, const pvxs::ProxyRequest& request) {
  std::mutex mutex;
  std::condition_variable changed;
  bool done = false;
  pvxs::client::Result result;
  operation->execute(request, [&](pvxs::client::Result&& value) {
    std::lock_guard<std::mutex> guard(mutex); result = std::move(value); done = true; changed.notify_all();
  });
  std::unique_lock<std::mutex> guard(mutex);
  assert(changed.wait_for(guard, 5s, [&] { return done; }));
  return result();
}

PVBinding binding(const std::shared_ptr<GatewayPV>& gateway) {
  return {pvxs::server::SharedPV{}, gateway, AccessAssignment{"TEST", 0},
      [gateway](PVRegistry::Channel&& channel) { gateway->attach(std::move(channel)); }};
}

pvxs::Value waitFor(const std::shared_ptr<pvxs::client::Operation>& operation, const char* description) {
  try { return operation->wait(5.0); }
  catch (const pvxs::client::Timeout&) { std::fprintf(stderr, "gateway test timeout: %s\n", description); throw; }
}

int main() {
  auto upstreamServer = pvxs::server::Config::isolated(AF_INET).build();
  auto localServer = pvxs::server::Config::isolated(AF_INET).build();
  auto upstream = pvxs::server::SharedPV::buildMailbox();
  std::atomic<unsigned> puts{0}, rpcs{0};
  std::atomic<bool> rejectPut{false}, holdPut{false};
  std::mutex heldMutex;
  std::unique_ptr<pvxs::server::ExecOp> held;
  upstream.onPut([&](pvxs::server::SharedPV& pv, std::unique_ptr<pvxs::server::ExecOp>&& op, pvxs::Value&& value) {
    ++puts;
    if (rejectPut) { op->error("upstream rejected the selected fields"); return; }
    auto delta = pv.fetch().cloneEmpty(); delta.assign(value); pv.post(delta);
    if (holdPut) { std::lock_guard<std::mutex> guard(heldMutex); held = std::move(op); }
    else op->reply();
  });
  upstream.onRPC([&](pvxs::server::SharedPV&, std::unique_ptr<pvxs::server::ExecOp>&& op, pvxs::Value&& value) {
    ++rpcs; op->reply(value);
  });
  const auto initialTable = table();
  upstream.open(initialTable);
  auto gate = std::make_shared<MonitorGate>();
  auto source = std::make_shared<GatedSource>(upstream, gate);
  auto native = std::make_shared<NativeSource>();
  upstreamServer.addSource("table", source).addSource("native", native).start();
  const auto selectedServer = "127.0.0.1:" + std::to_string(upstreamServer.config().tcp_port);
  auto context = upstreamServer.clientConfig().build();
  auto gateway = GatewayPV::create(context, "upstream:table", selectedServer);
  auto nativeGateway = GatewayPV::create(context, "upstream:native", selectedServer);
  GatewayPV::Limits boundedLimits;
  boundedLimits.maxInFlight = 1; boundedLimits.timeoutMs = 300;
  auto bounded = GatewayPV::create(context, "upstream:table", selectedServer, boundedLimits);
  assert(gateway->status().channels == 0 && gateway->status().monitors == 0 && gate->subscriptions == 0);

  const auto path = std::filesystem::temp_directory_path() / ("redis-pvxs-gateway-" + std::to_string(getpid()) + ".acf");
  const auto policy = [&](const char* rights) { std::ofstream file(path); file << "ASG(TEST) { RULE(0, " << rights << ") }\n"; assert(file.good()); };
  policy("WRITE");
  AccessConfig cfg; cfg.enabled = true; cfg.file = path.string();
  auto access = std::make_shared<AccessController>(cfg);
  std::string error;
  assert(access->start({"TEST"}, error));
  PVBindings secured{{"export:table", binding(gateway)}, {"export:alias", binding(gateway)}, {"export:native", binding(nativeGateway)}};
  auto prepared = access->prepareBindings(secured);
  assert(access->publishBindings(prepared, {}, error)); access->finishBindings(prepared);
  auto plain = std::make_shared<PVRegistry>();
  auto plainPrepared = plain->prepare({{"export:plain", binding(gateway)}, {"export:bounded", binding(bounded)}});
  assert(plain->publish(plainPrepared, {}, error)); plain->finish(plainPrepared);
  localServer.addSource("secured", access->source()).addSource("plain", plain).start();
  auto client = localServer.clientConfig().build();

  auto initial = client.get("export:table").exec()->wait(5.0);
  assert(initial.id() == "epics:nt/NTTable:1.0");
  assert(initial["value.reading"].as<pvxs::shared_array<const double>>()[1] == 2.5);
  assert(initial["record._options.queueSize"].as<int32_t>() == 0);
  assert(client.get("export:alias").record("atomic", false).exec()->wait(5.0)["record._options.atomic"].as<bool>() == false);
  assert(initial["extra"].as<pvxs::Value>().as<std::string>() == "arbitrary value");
  assert(initial["records"].as<pvxs::shared_array<const pvxs::Value>>()[0]["samples"].as<pvxs::shared_array<const int32_t>>()[1] == 8);
  assert(client.info("export:table").subField("value.reading").exec()->wait(5.0).type() == pvxs::TypeCode::Float64A);
  assert(gate->subscriptions == 0); // GET and discovery never start an upstream monitor.

  client.put("export:table").field("value.reading").set("value.reading", array<double>({8, 9})).exec()->wait(5.0);
  assert(puts == 1 && upstream.fetch()["value.reading"].as<pvxs::shared_array<const double>>()[1] == 9);
  assert(upstream.fetch()["labels"].as<pvxs::shared_array<const std::string>>()[0] == "Reading");
  auto variant = pvxs::TypeDef(pvxs::TypeCode::String).create(); variant.from(std::string("changed"));
  client.put("export:plain").field("extra").set("extra", variant).exec()->wait(5.0);
  assert(puts == 2 && upstream.fetch()["extra"].as<pvxs::Value>().as<std::string>() == "changed");
  rejectPut = true;
  bool remoteRejected = false;
  try { client.put("export:table").field("value.reading").set("value.reading", array<double>({10, 11})).exec()->wait(5.0); }
  catch (const pvxs::client::RemoteError& e) { remoteRejected = std::string(e.what()).find("upstream rejected the selected fields") != std::string::npos; }
  assert(remoteRejected && puts == 3); rejectPut = false;
  auto rpcRequest = pvxs::TypeDef(pvxs::TypeCode::Struct, {pvxs::members::String("method")}).create(); rpcRequest["method"] = "echo";
  assert(client.rpc("export:table", rpcRequest).exec()->wait(5.0)["method"].as<std::string>() == "echo" && rpcs == 1);

  std::atomic<unsigned> events{0};
  auto monitor = client.monitor("export:table").field("value.reading").record("queueSize", 3)
      .event([&](pvxs::client::Subscription&) { ++events; }).exec();
  eventually([&] { return events != 0; });
  eventually([&] { try { return bool(monitor->pop()); } catch (...) { return false; } });
  assert(gate->subscriptions == 1 && gateway->status().monitors == 1);
  auto update = initialTable.cloneEmpty(); update["value.reading"] = array<double>({3, 4}); upstream.post(update);
  eventually([&] { try { auto value = monitor->pop(); return value && value["value.reading"].as<pvxs::shared_array<const double>>()[1] == 4; } catch (...) { return false; } });
  monitor->cancel(); eventually([&] { return gateway->status().monitors == 0; });
  gate->reject = true;
  bool rejectedMonitor = false;
  auto rejected = client.monitor("export:alias").event([](pvxs::client::Subscription&) {}).exec();
  eventually([&] { try { rejected->pop(); } catch (const pvxs::client::RemoteError& e) { rejectedMonitor = std::string(e.what()).find("upstream monitor rejected") != std::string::npos; } return rejectedMonitor; });
  rejected->cancel(); gate->reject = false;

  pvxs::ProxyRequest request;
  auto process = client.proxy("export:native", pvxs::ProxyKind::Process).exec();
  assert(!execute(process, request) && native->processes == 1);
  auto putget = client.proxy("export:native", pvxs::ProxyKind::PutGet).exec();
  request.action = pvxs::ProxyAction::GetPut;
  assert(execute(putget, request)["input"].as<int32_t>() == 3);
  request.action = pvxs::ProxyAction::GetGet;
  assert(execute(putget, request)["output"].as<int64_t>() == 77);
  request.action = pvxs::ProxyAction::PutGet; request.value = native->put.cloneEmpty(); request.value["input"] = 8;
  assert(execute(putget, request)["output"].as<int64_t>() == 16);
  auto arrayOp = client.proxy("export:native", pvxs::ProxyKind::Array).exec();
  request = {}; request.action = pvxs::ProxyAction::ArrayGet; request.offset = 260; request.count = 2; request.stride = 3;
  assert(execute(arrayOp, request).as<pvxs::shared_array<const int32_t>>()[1] == 33);
  assert(native->last.offset == 260 && native->last.count == 2 && native->last.stride == 3);
  request.action = pvxs::ProxyAction::ArrayPut; request.value = native->values.cloneEmpty(); request.value = array<int32_t>({7, 9});
  assert(!execute(arrayOp, request));
  assert(native->last.value.as<pvxs::shared_array<const int32_t>>()[1] == 9);
  request = {}; request.action = pvxs::ProxyAction::ArraySetLength; request.length = 265;
  assert(!execute(arrayOp, request)); request.action = pvxs::ProxyAction::ArrayGetLength;
  assert(execute(arrayOp, request).as<uint64_t>() == 265);

  policy("READ"); assert(access->reload("test", error));
  assert(client.get("export:table").exec()->wait(5.0));
  auto readonlyProcess = client.proxy("export:native", pvxs::ProxyKind::Process).exec();
  bool deniedProcess = false, deniedWrite = false, deniedGetProcess = false;
  try { execute(readonlyProcess, {}); } catch (const pvxs::client::RemoteError& e) { deniedProcess = std::string(e.what()).find("access denied") != std::string::npos; }
  try { client.put("export:table").field("extra").set("extra", variant).exec()->wait(5.0); } catch (const pvxs::client::RemoteError&) { deniedWrite = true; }
  try { client.get("export:table").record("process", "true").exec()->wait(5.0); } catch (const pvxs::client::RemoteError&) { deniedGetProcess = true; }
  assert(deniedProcess && deniedWrite && deniedGetProcess && native->processes == 1 && puts == 3);
  auto readonlyArray = client.proxy("export:native", pvxs::ProxyKind::Array).exec();
  request = {}; request.action = pvxs::ProxyAction::ArrayGetLength;
  assert(execute(readonlyArray, request).as<uint64_t>() == 265);
  policy("WRITE"); assert(access->reload("test", error));

  // Capacity and deadline are tied to a real outstanding upstream mutation.
  holdPut = true;
  const auto before = puts.load();
  auto outstanding = client.put("export:bounded").field("value.reading").set("value.reading", array<double>({20, 21})).exec();
  eventually([&] { return puts == before + 1; });
  bool capacity = false;
  try { waitFor(client.put("export:bounded").field("extra").set("extra", variant).exec(), "capacity rejection"); }
  catch (const pvxs::client::RemoteError& e) { capacity = std::string(e.what()).find("capacity") != std::string::npos; }
  bool uncertain = false;
  try { waitFor(outstanding, "uncertain mutation completion"); } catch (const pvxs::client::RemoteError& e) { uncertain = std::string(e.what()).find("uncertain after dispatch") != std::string::npos; }
  assert(capacity && uncertain && puts == before + 1 && bounded->status().uncertain == 1);
  { std::lock_guard<std::mutex> guard(heldMutex); held.reset(); }
  holdPut = false;
  // The timeout closes that downstream channel. A fresh request negotiates a
  // new native setup and succeeds, rather than reusing a cancelled upstream IOID.
  assert(waitFor(client.get("export:bounded").exec(), "fresh GET after mutation timeout")["value.reading"].as<pvxs::shared_array<const double>>()[1] == 21);

  // Retiring one alias leaves its canonical source and sibling aliases alive.
  secured.erase("export:alias"); prepared = access->prepareBindings(secured);
  assert(access->publishBindings(prepared, {}, error)); access->finishBindings(prepared);
  assert(client.get("export:table").exec()->wait(5.0));
  policy("NONE"); assert(access->reload("test", error));
  bool deniedGet = false;
  try { client.get("export:table").exec()->wait(5.0); } catch (const pvxs::client::RemoteError&) { deniedGet = true; }
  assert(deniedGet);
  // No wrapper means generic forwarding remains writable.
  client.put("export:plain").field("extra").set("extra", variant).exec()->wait(5.0);

  gateway->deactivate(); nativeGateway->deactivate(); bounded->deactivate();
  assert(!gateway->connected() && gateway->status().inFlight == 0);
  client.close(); localServer.stop(); context.close(); upstreamServer.stop();
  access->clearBindings(); plain->clear(); std::filesystem::remove(path);
  AccessConfig builtin; builtin.enabled = true;
  std::string fingerprint;
  assert(validateAccessPolicy(builtin, {"DEFAULT"}, fingerprint, error));
  assert(!validateAccessPolicy(builtin, {"TEST"}, fingerprint, error));
}
