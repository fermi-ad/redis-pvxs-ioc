// Native PVA fixture and client for the isolated sidecar/application test.
#ifndef PVXS_ENABLE_EXPERT_API
#define PVXS_ENABLE_EXPERT_API
#endif
#include <pvxs/client.h>
#include <pvxs/nt.h>
#include <pvxs/proxy.h>
#include <pvxs/server.h>
#include <pvxs/source.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <future>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#include <sys/socket.h>

using namespace pvxs;
using namespace std::chrono_literals;
namespace {
std::string clientStage;
void require(bool condition, const std::string& text) {
    if (!condition) throw std::runtime_error(text);
}
std::string jsonString(const std::string& value) {
    std::ostringstream output;
    output << '"';
    for (const unsigned char c : value) {
        switch (c) {
        case '"': output << "\\\""; break;
        case '\\': output << "\\\\"; break;
        case '\n': output << "\\n"; break;
        case '\r': output << "\\r"; break;
        case '\t': output << "\\t"; break;
        default:
            if (c < 0x20) output << "\\u" << std::hex << std::setw(4) << std::setfill('0') << unsigned(c) << std::dec;
            else output << c;
        }
    }
    output << '"';
    return output.str();
}
template<class T> shared_array<const T> array(std::initializer_list<T> values) {
    shared_array<T> out(values.size());
    std::copy(values.begin(), values.end(), out.begin());
    return out.freeze();
}
Value scalar(TypeCode type, int marker = 0) {
    auto out = nt::NTScalar{type}.create();
    if (type == TypeCode::String) out["value"] = std::string("fixture-") + std::to_string(marker);
    else if (type == TypeCode::Int64) out["value"] = int64_t(9007199254740993LL + marker);
    else out["value"] = int32_t(marker);
    out["timeStamp.secondsPastEpoch"] = int64_t(1760000000);
    out["timeStamp.nanoseconds"] = int32_t(987654321);
    return out;
}
Value table(int marker) {
    auto out = TypeDef(TypeCode::Struct, "epics:nt/NTTable:1.0", {
        members::StringA("labels"),
        members::Struct("value", {members::Float64A("reading"), members::StringA("name")}),
        members::Struct("timeStamp", {members::Int64("secondsPastEpoch"), members::Int32("nanoseconds")}),
        members::Struct("record", {members::Struct("_options", {
            members::Int32("queueSize"), members::Bool("atomic")})}),
        members::Any("variant"),
        members::StructA("records", {members::String("name"), members::Int32A("samples")})
    }).create();
    out["labels"] = array<std::string>({"Reading", "Name"});
    out["value.reading"] = array<double>({marker + 0.25, marker + 0.5});
    out["value.name"] = array<std::string>({"first", "second"});
    out["timeStamp.secondsPastEpoch"] = int64_t(1760000000);
    out["timeStamp.nanoseconds"] = int32_t(987654321);
    out["record._options.queueSize"] = int32_t(4);
    out["record._options.atomic"] = true;
    auto variant = TypeDef(TypeCode::UInt64).create();
    variant = uint64_t(18446744073709551610ULL);
    out["variant"].from(variant);
    shared_array<Value> records(1);
    records[0] = out["records"].allocMember();
    records[0]["name"] = std::string("nested");
    records[0]["samples"] = array<int32_t>({7, 8, 9});
    out["records"] = records.freeze();
    return out;
}
Value enumeration() {
    auto out = TypeDef(TypeCode::Struct, "epics:nt/NTEnum:1.0", {
        members::Struct("value", {members::Int32("index"), members::StringA("choices")}),
        members::Struct("timeStamp", {members::Int64("secondsPastEpoch"), members::Int32("nanoseconds")})
    }).create();
    out["value.index"] = int32_t(1);
    out["value.choices"] = array<std::string>({"idle", "armed", "busy"});
    out["timeStamp.secondsPastEpoch"] = int64_t(1760000000);
    out["timeStamp.nanoseconds"] = int32_t(987654321);
    return out;
}
Value waveform(size_t length) {
    auto out = nt::NTScalar{TypeCode::Int32A}.create();
    shared_array<int32_t> values(length);
    for (size_t i = 0; i < length; ++i) values[i] = int32_t(i ^ 0x55aa);
    out["value"] = values.freeze();
    out["timeStamp.secondsPastEpoch"] = int64_t(1760000000);
    out["timeStamp.nanoseconds"] = int32_t(987654321);
    return out;
}
struct Entry {
    explicit Entry(Value value, bool capable = false) : value(std::move(value)), capable(capable) {}
    std::mutex mutex;
    Value value;
    bool capable;
    bool dropNext = false;
    RemoteStatus remoteStatus;
    std::vector<int32_t> nativeArray{10, 11, 12, 13, 14, 15, 16, 17};
    std::vector<std::weak_ptr<server::MonitorControlOp>> monitors;
    std::vector<std::weak_ptr<server::ChannelControl>> channels;
    std::atomic<uint64_t> infos{0}, gets{0}, puts{0}, rpcs{0}, processes{0}, putgets{0};
    std::atomic<uint64_t> arrayReads{0}, arrayWrites{0}, subscriptions{0}, active{0}, mutations{0};
    Value snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return value.clone();
    }
    RemoteStatus status() {
        std::lock_guard<std::mutex> lock(mutex);
        return remoteStatus;
    }
    void publish(const Value& update) {
        std::vector<std::shared_ptr<server::MonitorControlOp>> targets;
        Value copy;
        {
            std::lock_guard<std::mutex> lock(mutex);
            value.assign(update);
            copy = value.clone();
            for (auto& weak : monitors) if (auto op = weak.lock()) targets.push_back(std::move(op));
        }
        for (auto& op : targets) op->post(copy);
    }
    void disconnect() {
        std::vector<std::shared_ptr<server::ChannelControl>> targets;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (auto& weak : channels) if (auto channel = weak.lock()) targets.push_back(std::move(channel));
        }
        for (auto& channel : targets) channel->close();
    }
    bool drop() {
        std::lock_guard<std::mutex> lock(mutex);
        const bool result = dropNext;
        dropNext = false;
        return result;
    }
};
class FixtureSource final : public server::Source {
public:
    std::map<std::string, std::shared_ptr<Entry>> pvs;
    std::mutex mutex;
    std::map<server::ChannelControl*, std::shared_ptr<server::ChannelControl>> channels;
    void onSearch(Search& search) override {
        for (auto& name : search) if (pvs.count(name.name())) name.claim();
    }
    List onList() override {
        auto names = std::make_shared<std::set<std::string>>();
        for (const auto& pair : pvs) names->insert(pair.first);
        return {names, true};
    }
    void onCreate(std::unique_ptr<server::ChannelControl>&& raw) override {
        const auto found = pvs.find(raw->name());
        if (found == pvs.end()) return;
        auto entry = found->second;
        auto channel = std::shared_ptr<server::ChannelControl>(std::move(raw));
        auto identity = channel.get();
        {
            std::lock_guard<std::mutex> lock(mutex);
            channels.emplace(identity, channel);
        }
        {
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->channels.push_back(channel);
        }
        channel->onClose([this, identity](const std::string&) {
            std::lock_guard<std::mutex> lock(mutex);
            channels.erase(identity);
        });
        channel->onOp([entry](std::unique_ptr<server::ConnectOp>&& op) {
            if (op->op() == server::OpBase::Info) ++entry->infos;
            bool atomic = true;
            if (op->pvRequest()["record._options.atomic"])
                op->pvRequest()["record._options.atomic"].as(atomic);
            op->onGet([entry, atomic](std::unique_ptr<server::ExecOp>&& execution) {
                ++entry->gets;
                auto value = entry->snapshot();
                if (value["record._options.queueSize"]) value["record._options.queueSize"] = int32_t(0);
                if (value["record._options.atomic"]) value["record._options.atomic"] = atomic;
                execution->reply(value, entry->status());
            });
            op->onPut([entry](std::unique_ptr<server::ExecOp>&& execution, Value&& value) {
                ++entry->puts; ++entry->mutations;
                entry->publish(value);
                if (entry->drop()) entry->disconnect();
                else execution->reply(entry->status());
            });
            op->connect(entry->snapshot(), entry->status());
        });
        channel->onRPC([entry](std::unique_ptr<server::ExecOp>&& execution, Value&& value) {
            if (!entry->capable) { execution->error("fixture RPC unsupported"); return; }
            ++entry->rpcs; ++entry->mutations;
            if (entry->drop()) entry->disconnect();
            else execution->reply(value, entry->status());
        });
        channel->onSubscribe([entry](std::unique_ptr<server::MonitorSetupOp>&& setup) {
            ++entry->subscriptions; ++entry->active;
            auto snapshot = entry->snapshot();
            auto control = std::shared_ptr<server::MonitorControlOp>(setup->connect(snapshot, entry->status()));
            std::weak_ptr<server::MonitorControlOp> weak(control);
            control->onStart([entry, weak](bool start) {
                if (start) if (auto target = weak.lock()) target->post(entry->snapshot());
            });
            setup->onClose([entry, control](const std::string&) { --entry->active; });
            std::lock_guard<std::mutex> lock(entry->mutex);
            entry->monitors.push_back(control);
        });
        channel->onProxy([entry](std::unique_ptr<server::ProxySetupOp>&& setup) {
            if (!entry->capable) { setup->error("fixture native operation unsupported"); return; }
            ProxyPrototype prototype;
            if (setup->kind() == ProxyKind::PutGet) {
                prototype.put = TypeDef(TypeCode::Struct, {members::Int32("argument")}).create();
                prototype.get = TypeDef(TypeCode::Struct, {
                    members::Int64("result"), members::String("action")}).create();
            } else if (setup->kind() == ProxyKind::Array) {
                prototype.put = prototype.get = TypeDef(TypeCode::Int32A).create();
            }
            setup->onExec([entry, prototype](std::unique_ptr<server::ProxyExecOp>&& execution,
                                             ProxyRequest&& request) {
                if (request.mutating()) ++entry->mutations;
                Value reply;
                switch (request.action) {
                case ProxyAction::Process:
                    ++entry->processes;
                    break;
                case ProxyAction::PutGet:
                    ++entry->putgets;
                    reply = prototype.get.cloneEmpty();
                    reply["result"] = int64_t(request.value["argument"].as<int32_t>()) * 10;
                    reply["action"] = std::string("putGet");
                    break;
                case ProxyAction::GetPut:
                    reply = prototype.put.cloneEmpty(); reply["argument"] = int32_t(42);
                    break;
                case ProxyAction::GetGet:
                    reply = prototype.get.cloneEmpty(); reply["result"] = int64_t(420);
                    reply["action"] = std::string("getGet");
                    break;
                case ProxyAction::ArrayGet: {
                    ++entry->arrayReads;
                    std::lock_guard<std::mutex> lock(entry->mutex);
                    const auto count = request.count ? request.count : entry->nativeArray.size();
                    std::vector<int32_t> selected;
                    for (uint64_t i = 0; i < count && request.offset + i * request.stride < entry->nativeArray.size(); ++i)
                        selected.push_back(entry->nativeArray[request.offset + i * request.stride]);
                    shared_array<int32_t> result(selected.size());
                    std::copy(selected.begin(), selected.end(), result.begin());
                    reply = prototype.get.cloneEmpty(); reply = result.freeze();
                    break;
                }
                case ProxyAction::ArrayPut: {
                    ++entry->arrayWrites;
                    const auto values = request.value.as<shared_array<const int32_t>>();
                    std::lock_guard<std::mutex> lock(entry->mutex);
                    for (size_t i = 0; i < values.size(); ++i) {
                        const auto index = request.offset + i * request.stride;
                        if (index >= entry->nativeArray.size()) entry->nativeArray.resize(index + 1);
                        entry->nativeArray[index] = values[i];
                    }
                    break;
                }
                case ProxyAction::ArrayGetLength: {
                    ++entry->arrayReads;
                    std::lock_guard<std::mutex> lock(entry->mutex);
                    execution->replyLength(entry->nativeArray.size(), entry->remoteStatus);
                    return;
                }
                case ProxyAction::ArraySetLength: {
                    ++entry->arrayWrites;
                    std::lock_guard<std::mutex> lock(entry->mutex);
                    entry->nativeArray.resize(request.length);
                    break;
                }
                }
                if (request.mutating() && entry->drop()) entry->disconnect();
                else if (reply) execution->reply(reply, entry->status());
                else execution->reply(entry->status());
            });
            setup->connect(prototype, entry->status());
        });
    }
    void stats() {
        std::cout << "{\"pvs\":{";
        bool first = true;
        for (const auto& pair : pvs) {
            if (!first) std::cout << ',';
            first = false;
            const auto& entry = *pair.second;
            std::cout << jsonString(pair.first) << ":{\"infos\":" << entry.infos
                << ",\"gets\":" << entry.gets << ",\"puts\":" << entry.puts
                << ",\"rpcs\":" << entry.rpcs << ",\"processes\":" << entry.processes
                << ",\"putgets\":" << entry.putgets << ",\"arrayReads\":" << entry.arrayReads
                << ",\"arrayWrites\":" << entry.arrayWrites << ",\"subscriptions\":" << entry.subscriptions
                << ",\"active\":" << entry.active << ",\"mutations\":" << entry.mutations << '}';
        }
        std::cout << "}}" << std::endl;
    }
};
int serve(const std::map<std::string, std::string>& options) {
    const auto role = options.at("--role");
    require(role == "BASE" || role == "APP", "role must be BASE or APP");
    auto source = std::make_shared<FixtureSource>();
    auto add = [&](const std::string& name, Value value, bool capable = false) {
        source->pvs.emplace(name, std::make_shared<Entry>(std::move(value), capable));
    };
    if (role == "BASE") {
        add("ACQ2206-097:0:MODEL", scalar(TypeCode::String, 2206));
        add("ACQ2206-097:0:SERIAL", scalar(TypeCode::String, 97));
        add("ACQ2206-097:1:NCHAN", scalar(TypeCode::Int32, 32));
        add("ACQ2206-097:2:NCHAN", scalar(TypeCode::Int32, 32));
        add("ACQ2206-097:0:WR:TIME_VALID", scalar(TypeCode::Int32, 1));
        add("ENUM", enumeration()); add("S64", scalar(TypeCode::Int64, 0));
        add("WAVEFORM", waveform(262144), true);
    } else {
        add("acq2206_597:FMT:TAB:Tbl", table(200));
        add("acq2206_597:SOE_HLD:TAB:Tbl", table(300));
        add("NESTED", table(400));
    }
    auto ip = scalar(TypeCode::String); ip["value"] = role;
    add("IP", ip);
    add("CONTROL", scalar(TypeCode::Int32, role == "BASE" ? 10 : 20), true);
    add("UNSUPPORTED", scalar(TypeCode::Int32, 30));
    add("LOST", scalar(TypeCode::Int32, 0), true);
    const auto extra = options.count("--extra") ? std::stoul(options.at("--extra")) : 0;
    for (size_t i = 0; i < extra; ++i) add("FILL:" + std::to_string(i), scalar(TypeCode::Int32, i));
    auto config = server::Config::isolated(AF_INET);
    config.udp_port = std::stoul(options.at("--udp-port"));
    auto native = config.build();
    native.addSource("native", source).start();
    const auto announce = [&] {
        std::cout << "{\"role\":" << jsonString(role) << ",\"tcp\":" << native.config().tcp_port
            << ",\"udp\":" << native.config().udp_port << ",\"count\":" << source->pvs.size() << '}' << std::endl;
    };
    announce();
    for (std::string line; std::getline(std::cin, line);) {
        std::istringstream input(line);
        std::string command, name;
        input >> command >> name;
        if (command == "stats") { source->stats(); continue; }
        if (command == "quit") break;
        if (command == "rotate") {
            for (const auto& pair : source->pvs) pair.second->disconnect();
            native.stop(); native = server::Server();
            native = config.build(); native.addSource("native", source).start();
            announce(); continue;
        }
        if (command == "stop") { native.stop(); std::cout << "{}" << std::endl; continue; }
        if (command == "start") { native.start(); announce(); continue; }
        const auto entry = source->pvs.at(name);
        if (command == "drop") {
            std::lock_guard<std::mutex> lock(entry->mutex); entry->dropNext = true;
        } else if (command == "warning" || command == "fatal" || command == "normal") {
            {
                std::lock_guard<std::mutex> lock(entry->mutex);
                entry->remoteStatus = command == "normal" ? RemoteStatus() : RemoteStatus(
                    command == "warning" ? RemoteStatus::Warning : RemoteStatus::Fatal,
                    "fixture " + command, "native fixture stack\nsecond frame");
            }
            entry->disconnect();
        } else if (command == "string") {
            { std::lock_guard<std::mutex> lock(entry->mutex); entry->value = scalar(TypeCode::String, 99); }
            entry->disconnect();
        } else if (command == "post") {
            auto value = entry->snapshot(); value["value"] = int32_t(123); entry->publish(value);
        } else throw std::runtime_error("unknown control command " + command);
        std::cout << "{}" << std::endl;
    }
    for (const auto& pair : source->pvs) pair.second->disconnect();
    native.stop();
    return 0;
}

client::Context context(const std::string& endpoint) {
    client::Config config; config.autoAddrList = false; config.addressList = {endpoint};
    config.nameServers.clear(); return config.build();
}
struct Native {
    std::shared_ptr<client::ProxyOperation> operation;
    ProxyPrototype prototype;
    RemoteStatus initStatus;
    Native(client::Context& client, const std::string& name, ProxyKind kind, const Value& request = Value()) {
        auto initialized = std::make_shared<std::promise<std::pair<ProxyPrototype, RemoteStatus>>>();
        auto future = initialized->get_future();
        auto builder = client.proxy(name, kind);
        if (request) builder.rawRequest(request);
        operation = builder.onInit([initialized](const ProxyPrototype& type, const RemoteStatus& status) {
            try { initialized->set_value({type, status}); } catch (const std::future_error&) {}
        }).result([initialized](client::Result&& result) {
            try { result(); } catch (...) {
                try { initialized->set_exception(std::current_exception()); } catch (const std::future_error&) {}
            }
        }).exec();
        require(future.wait_for(5s) == std::future_status::ready, "native INIT timeout " + name);
        const auto result = future.get();
        prototype = result.first; initStatus = result.second;
    }
    client::Result executeResult(const ProxyRequest& request) {
        auto completion = std::make_shared<std::promise<client::Result>>();
        auto future = completion->get_future();
        operation->execute(request, [completion](client::Result&& result) {
            completion->set_value(std::move(result));
        });
        require(future.wait_for(5s) == std::future_status::ready, "native execute timeout");
        return future.get();
    }
    Value execute(const ProxyRequest& request) { return executeResult(request)(); }
};
template<class F> void denied(F operation, const std::string& description, const std::string& expected = "") {
    bool failed = false;
    try { operation(); } catch (const client::RemoteError& error) {
        require(expected.empty() || std::string(error.what()).find(expected) != std::string::npos,
                description + " returned another remote error: " + error.what());
        failed = true;
    }
    require(failed, description + " unexpectedly succeeded");
}
int clientRun(const std::map<std::string, std::string>& options) {
    auto client = context(options.at("--endpoint"));
    const auto mode = options.at("--client");
    const auto prefix = options.count("--prefix") ? options.at("--prefix") : "";
    const auto get = [&](const std::string& name) {
        clientStage = "GET " + name;
        return client.get(name).exec()->wait(4.0);
    };
    const auto name = [&](const std::string& role, const std::string& native) {
        return options.count("--direct") ? native : prefix + role + ':' + native;
    };
    if (mode == "native-reference") {
        const auto target = options.count("--name") ? options.at("--name") : "CONTROL";
        const auto initial = get(target);
        require(initial["argument"] && initial["result"] && initial["value"].type() == TypeCode::Int32A,
                "native reference type changed");
        client.put(target).field("argument").fetchPresent(false).set("argument", int32_t(17)).exec()->wait(5.0);
        Native process(client, target, ProxyKind::Process);
        ProxyRequest request; request.action = ProxyAction::Process; process.execute(request);
        require(get(target)["result"].as<int64_t>() == 170, "native PROCESS did not execute");
        auto pgRequest = TypeDef(TypeCode::Struct, {
            members::Struct("record", {members::Struct("_options", {members::String("process")})}),
            members::Struct("putField", {members::Struct("argument", {})}),
            members::Struct("getField", {members::Struct("result", {}), members::Struct("action", {})})}).create();
        pgRequest["record._options.process"] = std::string("true");
        Native pg(client, target, ProxyKind::PutGet, pgRequest);
        request.action = ProxyAction::GetPut;
        require(pg.execute(request)["argument"].as<int32_t>() == 17, "native GET_PUT mismatch");
        request.action = ProxyAction::GetGet;
        require(pg.execute(request)["result"].as<int64_t>() == 170, "native GET_GET mismatch");
        request.action = ProxyAction::PutGet; request.value = pg.prototype.put.cloneEmpty();
        request.value["argument"] = int32_t(9);
        const auto result = pg.execute(request);
        require(result["result"].as<int64_t>() == 90 && result["action"].as<std::string>() == "putGet",
                "native PUT_GET distinct prototypes mismatch");
        const auto arrayRequest = client.request().pvRequest("field(value)").build();
        Native ar(client, target, ProxyKind::Array, arrayRequest);
        request = ProxyRequest(); request.action = ProxyAction::ArrayGet; request.offset = 1; request.count = 3; request.stride = 2;
        const auto selected = ar.execute(request).as<shared_array<const int32_t>>();
        require(selected.size() == 3 && selected[0] == 11 && selected[2] == 15,
                "native ARRAY offset/count/stride mismatch");
        request.action = ProxyAction::ArrayPut; request.offset = 0; request.count = 2; request.stride = 2;
        request.value = ar.prototype.put.cloneEmpty(); request.value = array<int32_t>({51, 52}); ar.execute(request);
        request = ProxyRequest(); request.action = ProxyAction::ArrayGet; request.count = 3;
        const auto changed = ar.execute(request).as<shared_array<const int32_t>>();
        require(changed.size() == 3 && changed[0] == 51 && changed[1] == 11 && changed[2] == 52,
                "native ARRAY PUT mismatch");
        request = ProxyRequest(); request.action = ProxyAction::ArrayGetLength;
        require(ar.execute(request).as<uint64_t>() == 8, "native ARRAY GET_LENGTH mismatch");
        request.action = ProxyAction::ArraySetLength; request.length = 5; ar.execute(request);
        request.action = ProxyAction::ArrayGetLength;
        require(ar.execute(request).as<uint64_t>() == 5, "native ARRAY SET_LENGTH mismatch");
        // The stock pvDatabase provider reuses a capacity-sized buffer on an
        // existing array operation after shrink; a fresh setup avoids that bug.
        Native freshArray(client, target, ProxyKind::Array, arrayRequest);
        request = ProxyRequest(); request.action = ProxyAction::ArrayGet; request.count = 5;
        require(freshArray.execute(request).as<shared_array<const int32_t>>().size() == 5,
                "native fresh ARRAY after shrink mismatch");
        auto argument = TypeDef(TypeCode::Struct, {members::Int64("number")}).create();
        argument["number"] = int64_t(9007199254740993LL);
        require(client.rpc(target, argument).exec()->wait(5.0)["number"].as<int64_t>() == 9007199254740993LL,
                "native RPC echo mismatch");
        auto subscription = client.monitor(target).event([](client::Subscription&) {}).exec();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        Value event;
        while (!event && std::chrono::steady_clock::now() < deadline) {
            try { event = subscription->pop(); } catch (const client::Connected&) {}
            if (!event) std::this_thread::sleep_for(5ms);
        }
        require(event && event["argument"].as<int32_t>() == 9, "native MONITOR mismatch");
        subscription->cancel();
        // Stock ChannelPutGetLocal leaves group-put depth set after process()
        // throws, so exercise its error path after monitor validation.
        request = ProxyRequest(); request.action = ProxyAction::PutGet;
        request.value = pg.prototype.put.cloneEmpty(); request.value["argument"] = int32_t(-1);
        bool executionRejected = false;
        try { pg.execute(request); } catch (const client::RemoteError& error) {
            executionRejected = std::string(error.what()).find("native reference rejected argument") != std::string::npos;
        }
        require(executionRejected, "native PUT_GET execution error was not propagated");
        request.value["argument"] = int32_t(9);
        require(pg.execute(request)["result"].as<int64_t>() == 90, "native PUT_GET did not recover after execution error");
    } else if (mode == "observe") {
        const auto model = get(name("BASE", "ACQ2206-097:0:MODEL"));
        require(model["value"].as<std::string>() == "fixture-2206", "model changed");
        require(model["timeStamp.secondsPastEpoch"].as<int64_t>() == 1760000000, "seconds changed");
        require(model["timeStamp.nanoseconds"].as<int32_t>() == 987654321, "nanoseconds changed");
        require(get(name("BASE", "IP"))["value"].as<std::string>() == "BASE", "BASE duplicate selected wrong server");
        require(get(name("APP", "IP"))["value"].as<std::string>() == "APP", "APP duplicate selected wrong server");
        require(get(name("BASE", "S64"))["value"].as<int64_t>() == 9007199254740993LL, "int64 lost precision");
        const auto en = get(name("BASE", "ENUM"));
        require(en.id() == "epics:nt/NTEnum:1.0" && en["value.index"].as<int32_t>() == 1 &&
            en["value.choices"].as<shared_array<const std::string>>()[2] == "busy", "enum changed");
        const auto tab = get(name("APP", "acq2206_597:FMT:TAB:Tbl"));
        require(tab.id() == "epics:nt/NTTable:1.0" && tab["value.reading"].as<shared_array<const double>>()[1] == 200.5,
                "table changed");
        require(tab["variant"].as<Value>().as<uint64_t>() == 18446744073709551610ULL, "variant changed");
        const auto records = tab["records"].as<shared_array<const Value>>();
        require(records.size() == 1 && records[0]["samples"].as<shared_array<const int32_t>>()[2] == 9, "nested array changed");
        const auto nonatomic = client.get(name("APP", "acq2206_597:FMT:TAB:Tbl"))
            .record("atomic", false).exec()->wait(4.0);
        require(!nonatomic["record._options.atomic"].as<bool>() &&
            nonatomic["record._options.queueSize"].as<int32_t>() == 0, "GET request options were not forwarded");
        const auto wave = get(name("BASE", "WAVEFORM"))["value"].as<shared_array<const int32_t>>();
        require(wave.size() == 262144, "large waveform truncated");
        for (size_t i = 0; i < wave.size(); ++i) require(wave[i] == int32_t(i ^ 0x55aa), "large waveform corrupted");
        clientStage = "GET_FIELD " + name("APP", "NESTED");
        const auto type = client.info(name("APP", "NESTED")).exec()->wait(4.0);
        require(type.id() == "epics:nt/NTTable:1.0" && type["records"], "GET_FIELD lost native type");
        if (options.count("--aliases")) {
            const std::vector<std::pair<std::string, std::string>> aliases{
                {"carrier:MODEL", name("BASE", "ACQ2206-097:0:MODEL")},
                {"carrier:SERIAL", name("BASE", "ACQ2206-097:0:SERIAL")},
                {"module1:NCHAN", name("BASE", "ACQ2206-097:1:NCHAN")},
                {"module2:NCHAN", name("BASE", "ACQ2206-097:2:NCHAN")},
                {"timing:WR:TIME_VALID", name("BASE", "ACQ2206-097:0:WR:TIME_VALID")},
                {"application:SOE_HLD:TAB:Tbl", name("APP", "acq2206_597:SOE_HLD:TAB:Tbl")},
                {"application:FMT:TAB:Tbl", name("APP", "acq2206_597:FMT:TAB:Tbl")}};
            for (const auto& alias : aliases) {
                auto friendly = get("SL:INST:ICM:XRM:" + alias.first), canonical = get(alias.second);
                require(friendly.equalType(canonical), "legacy alias type differs");
                std::ostringstream a, b; a << friendly; b << canonical;
                require(a.str() == b.str(), "legacy alias value differs");
            }
        }
    } else if (mode == "operations") {
        const bool writable = options.at("--writable") == "yes";
        const auto target = name("BASE", "CONTROL");
        auto put = [&] { client.put(target).fetchPresent(false).set("value", int32_t(77)).exec()->wait(4.0); };
        auto rpc = [&] {
            auto argument = TypeDef(TypeCode::Struct, "fixture:rpc:1.0", {
                members::Int64("number"), members::String("text")}).create();
            argument["number"] = int64_t(9007199254740993LL); argument["text"] = std::string("echo");
            auto response = client.rpc(target, argument).exec()->wait(4.0);
            require(response["number"].as<int64_t>() == 9007199254740993LL && response["text"].as<std::string>() == "echo",
                    "RPC payload changed");
        };
        auto process = [&] { Native op(client, target, ProxyKind::Process); ProxyRequest r; r.action = ProxyAction::Process; op.execute(r); };
        if (writable) { put(); rpc(); process(); } else { denied(put, "PUT"); denied(rpc, "RPC"); denied(process, "PROCESS"); }
        Native pg(client, target, ProxyKind::PutGet);
        ProxyRequest request; request.action = ProxyAction::GetPut;
        require(pg.execute(request)["argument"].as<int32_t>() == 42, "GET_PUT changed");
        request.action = ProxyAction::GetGet;
        require(pg.execute(request)["result"].as<int64_t>() == 420, "GET_GET changed");
        request.action = ProxyAction::PutGet; request.value = pg.prototype.put.cloneEmpty();
        request.value["argument"] = int32_t(9);
        auto putget = [&] { require(pg.execute(request)["result"].as<int64_t>() == 90, "PUT_GET changed"); };
        if (writable) putget(); else denied(putget, "PUT_GET");
        Native ar(client, target, ProxyKind::Array);
        request = ProxyRequest(); request.action = ProxyAction::ArrayGet; request.offset = 1; request.count = 3; request.stride = 2;
        auto values = ar.execute(request).as<shared_array<const int32_t>>();
        require(values.size() == 3 && values[0] == 11 && values[2] == 15, "ARRAY offset/count/stride changed");
        request.action = ProxyAction::ArrayGetLength;
        require(ar.execute(request).as<uint64_t>() >= 8, "ARRAY length changed");
        request.action = ProxyAction::ArrayPut; request.offset = 0; request.count = 2; request.stride = 2;
        request.value = ar.prototype.put.cloneEmpty(); request.value = array<int32_t>({51, 52});
        auto arrayPut = [&] { ar.execute(request); };
        if (writable) arrayPut(); else denied(arrayPut, "ARRAY PUT");
        request = ProxyRequest(); request.action = ProxyAction::ArraySetLength; request.length = 10;
        auto arrayResize = [&] { ar.execute(request); };
        if (writable) arrayResize(); else denied(arrayResize, "ARRAY SET_LENGTH");
        for (const auto kind : {ProxyKind::Process, ProxyKind::PutGet, ProxyKind::Array})
            denied([&] { Native unsupported(client, name("BASE", "UNSUPPORTED"), kind); },
                   "unsupported native INIT", writable && !options.count("--exception") ? "fixture native operation unsupported" : "");
        denied([&] { client.rpc(name("BASE", "UNSUPPORTED")).arg("number", int32_t(1)).exec()->wait(4.0); },
               "unsupported RPC", writable && !options.count("--exception") ? "fixture RPC unsupported" : "");
        if (options.count("--exception")) {
            denied([&] { client.put(name("APP", "CONTROL")).fetchPresent(false).set("value", int32_t(99)).exec()->wait(4.0); },
                   "unassigned APP PUT exception");
            denied([&] { client.rpc(name("APP", "CONTROL")).arg("number", int32_t(99)).exec()->wait(4.0); },
                   "unassigned APP RPC exception");
            denied([&] { Native other(client, name("APP", "CONTROL"), ProxyKind::Process);
                ProxyRequest request; request.action = ProxyAction::Process; other.execute(request); },
                   "unassigned APP PROCESS exception");
            denied([&] { client.put("SL:INST:ICM:XRM:module1:NCHAN").fetchPresent(false)
                .set("value", int32_t(99)).exec()->wait(4.0); }, "unassigned legacy PUT exception");
        }
    } else if (mode == "native-status") {
        const auto target = name("BASE", "CONTROL");
        const bool fatal = options.at("--severity") == "fatal";
        const auto check = [&](const RemoteStatus& status) {
            require(status.severity == (fatal ? RemoteStatus::Fatal : RemoteStatus::Warning) &&
                status.message == (fatal ? "fixture fatal" : "fixture warning") &&
                status.stack == "native fixture stack\nsecond frame", "native status severity/message/stack changed");
        };
        const auto expect = [&](const std::function<void()>& fn) {
            if (!fatal) { fn(); return; }
            bool failed = false;
            try { fn(); } catch (const client::RemoteError& error) { check(error.status()); failed = true; }
            require(failed, "fatal native status was not an error");
        };
        expect([&] {
            RemoteStatus init;
            auto complete = std::make_shared<std::promise<client::Result>>();
            auto future = complete->get_future();
            auto operation = client.get(target).onInit([&](const Value&, const RemoteStatus& status) { init = status; })
                .result([complete](client::Result&& result) { complete->set_value(std::move(result)); }).exec();
            require(future.wait_for(5s) == std::future_status::ready, "native GET status timed out");
            auto result = future.get(); result(); check(result.status()); check(init);
        });
        expect([&] {
            auto complete = std::make_shared<std::promise<client::Result>>();
            auto future = complete->get_future();
            auto operation = client.rpc(target).arg("number", int32_t(1)).result(
                [complete](client::Result&& result) { complete->set_value(std::move(result)); }).exec();
            require(future.wait_for(5s) == std::future_status::ready, "native RPC status timed out");
            auto result = future.get(); result(); check(result.status());
        });
        for (const auto kind : {ProxyKind::Process, ProxyKind::PutGet, ProxyKind::Array}) expect([&] {
            Native native(client, target, kind); check(native.initStatus);
            ProxyRequest request;
            request.action = kind == ProxyKind::Process ? ProxyAction::Process :
                kind == ProxyKind::PutGet ? ProxyAction::GetGet : ProxyAction::ArrayGetLength;
            auto result = native.executeResult(request); result(); check(result.status());
        });
    } else if (mode == "monitor") {
        auto subscription = client.monitor(name("BASE", "CONTROL"))
            .event([](client::Subscription&) {}).exec();
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        Value event;
        while (!event && std::chrono::steady_clock::now() < deadline) {
            try { event = subscription->pop(); } catch (const client::Connected&) {}
            if (!event) std::this_thread::sleep_for(5ms);
        }
        require(bool(event), "monitor did not deliver native value");
        std::this_thread::sleep_for(150ms);
        subscription->cancel();
    } else if (mode == "load") {
        const size_t count = std::stoul(options.at("--monitors"));
        std::vector<std::shared_ptr<client::Subscription>> subscriptions;
        std::vector<std::string> targets;
        for (size_t i = 0; i < count; ++i) {
            const auto target = i == 0 ? name("BASE", "WAVEFORM") :
                i == 1 ? name("APP", "NESTED") : name(i % 2 ? "APP" : "BASE", "FILL:" + std::to_string(i / 2));
            targets.push_back(target);
            subscriptions.push_back(client.monitor(target).event([](client::Subscription&) {}).exec());
        }
        std::vector<bool> observed(count, false);
        size_t accepted = 0, rejected = 0, waveElements = 0;
        const auto deadline = std::chrono::steady_clock::now() + 12s;
        while (accepted + rejected < count && std::chrono::steady_clock::now() < deadline) {
            for (size_t i = 0; i < subscriptions.size(); ++i) if (!observed[i]) {
                try {
                    if (auto event = subscriptions[i]->pop()) {
                        observed[i] = true; ++accepted;
                        if (i == 0) waveElements = event["value"].as<shared_array<const int32_t>>().size();
                    }
                } catch (const client::Connected&) {
                } catch (const client::RemoteError& error) {
                    require(std::string(error.what()).find("capacity") != std::string::npos,
                            "monitor load failed for another reason: " + std::string(error.what()));
                    observed[i] = true; ++rejected;
                }
            }
            std::this_thread::sleep_for(5ms);
        }
        std::string pendingNames;
        for (size_t i = 0; i < targets.size(); ++i) if (!observed[i]) pendingNames += " " + targets[i];
        require(accepted + rejected == count, "monitor admission did not settle: accepted=" + std::to_string(accepted) +
            " rejected=" + std::to_string(rejected) + " pending=" + std::to_string(count - accepted - rejected) + pendingNames);
        require(waveElements == 262144, "load monitor did not preserve large native array");
        std::cout << "{\"accepted\":" << accepted << ",\"rejected\":" << rejected
            << ",\"events\":" << accepted << ",\"waveElements\":" << waveElements << '}' << std::endl;
        std::string release;
        std::getline(std::cin, release);
        for (auto& subscription : subscriptions) subscription->cancel();
        return 0;
    } else if (mode == "lost") {
        const auto target = name("BASE", "LOST");
        const auto family = options.count("--family") ? options.at("--family") : "put";
        denied([&] {
            if (family == "put") client.put(target).fetchPresent(false).set("value", int32_t(991)).exec()->wait(2.0);
            else if (family == "rpc") client.rpc(target).arg("number", int32_t(991)).exec()->wait(2.0);
            else {
                const auto kind = family == "process" ? ProxyKind::Process :
                    family == "putget" ? ProxyKind::PutGet : ProxyKind::Array;
                Native op(client, target, kind);
                ProxyRequest request;
                if (family == "process") request.action = ProxyAction::Process;
                else if (family == "putget") {
                    request.action = ProxyAction::PutGet;
                    request.value = op.prototype.put.cloneEmpty(); request.value["argument"] = int32_t(991);
                } else if (family == "arrayput") {
                    request.action = ProxyAction::ArrayPut; request.count = 1;
                    request.value = op.prototype.put.cloneEmpty(); request.value = array<int32_t>({991});
                } else { request.action = ProxyAction::ArraySetLength; request.length = 12; }
                op.execute(request);
            }
        }, "lost-reply " + family);
        std::this_thread::sleep_for(300ms);
    } else if (mode == "status") {
        const auto value = get("SYS:xrm-integration:gateway:status");
        std::cout << "{\"generation\":" << value["generation"].as<uint64_t>()
            << ",\"pvCount\":" << value["pvCount"].as<uint64_t>()
            << ",\"aliasCount\":" << value["aliasCount"].as<uint64_t>()
            << ",\"state\":" << jsonString(value["state"].as<std::string>())
            << ",\"activeRevision\":" << jsonString(value["activeRevision"].as<std::string>())
            << ",\"lastError\":" << jsonString(value["lastError"].as<std::string>()) << ",\"sources\":[";
        const auto names = value["sources.name"].as<shared_array<const std::string>>();
        const auto upstreams = value["sources.upstream"].as<shared_array<const std::string>>();
        const auto servers = value["sources.server"].as<shared_array<const std::string>>();
        const auto monitors = value["sources.monitors"].as<shared_array<const uint64_t>>();
        const auto channels = value["sources.channels"].as<shared_array<const uint64_t>>();
        const auto inFlight = value["sources.inFlight"].as<shared_array<const uint64_t>>();
        const auto rejections = value["sources.rejected"].as<shared_array<const uint64_t>>();
        for (size_t i = 0; i < names.size(); ++i) {
            if (i) std::cout << ',';
            std::cout << "{\"name\":" << jsonString(names[i]) << ",\"upstream\":" << jsonString(upstreams[i])
                << ",\"server\":" << jsonString(servers[i]) << ",\"monitors\":" << monitors[i]
                << ",\"channels\":" << channels[i] << ",\"inFlight\":" << inFlight[i]
                << ",\"rejected\":" << rejections[i] << '}';
        }
        std::cout << "]}" << std::endl;
        return 0;
    } else if (mode == "get") {
        const auto value = get(options.at("--name"));
        const auto field = value["value"];
        if (field.type() == TypeCode::String) std::cout << jsonString(field.as<std::string>()) << std::endl;
        else std::cout << field.as<int64_t>() << std::endl;
        return 0;
    } else throw std::runtime_error("unknown client mode " + mode);
    client.close();
    std::cout << "{\"passed\":" << jsonString(mode) << '}' << std::endl;
    return 0;
}
}
int main(int argc, char** argv) {
    try {
        std::map<std::string, std::string> options;
        for (int i = 1; i < argc; ++i) {
            const std::string key = argv[i];
            if (key == "--serve" || key == "--aliases" || key == "--direct" || key == "--exception") options[key] = "yes";
            else { require(i + 1 < argc, "missing option value"); options[key] = argv[++i]; }
        }
        return options.count("--serve") ? serve(options) : clientRun(options);
    } catch (const std::exception& error) {
        std::cerr << "xrm integration fixture: " << error.what();
        if (!clientStage.empty()) std::cerr << " while " << clientStage;
        std::cerr << std::endl;
        return 1;
    }
}
