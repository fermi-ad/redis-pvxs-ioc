// Independent wire reference: this executable links stock pvAccessCPP and
// pvDatabaseCPP only.  Keep PVXS headers and libraries out of this target.
#include <pv/pvAccess.h>
#include <pv/clientFactory.h>
#include <pv/channelProviderLocal.h>
#include <pv/serverContext.h>
#include <pv/configuration.h>
#include <pv/createRequest.h>
#include <pv/pvDatabase.h>
#include <pv/rpcService.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace pd = epics::pvData;
namespace pa = epics::pvAccess;
namespace db = epics::pvDatabase;
using std::tr1::shared_ptr;
using std::tr1::dynamic_pointer_cast;

namespace {
void check(bool ok, const std::string& text) {
    if (!ok) throw std::runtime_error(text);
}
pd::PVStructurePtr request(const std::string& text) {
    auto parser = pd::CreateRequest::create();
    auto value = parser->createRequest(text);
    check(!!value, "bad request: " + text + ": " + parser->getMessage());
    return value;
}
pd::PVStructurePtr prototype() {
    auto type = pd::getFieldCreate()->createFieldBuilder()
        ->add("argument", pd::pvInt)->add("result", pd::pvLong)
        ->add("action", pd::pvString)->add("executions", pd::pvLong)
        ->addArray("value", pd::pvInt)->createStructure();
    auto value = pd::getPVDataCreate()->createPVStructure(type);
    value->getSubField<pd::PVInt>("argument")->put(42);
    value->getSubField<pd::PVLong>("result")->put(420);
    value->getSubField<pd::PVString>("action")->put("initial");
    pd::shared_vector<pd::int32> values(8);
    for (size_t i = 0; i < values.size(); ++i) values[i] = pd::int32(10 + i);
    value->getSubField<pd::PVIntArray>("value")->replace(pd::freeze(values));
    return value;
}
struct Echo final : pa::RPCService {
    pd::PVStructurePtr request(const pd::PVStructurePtr& args) override {
        auto value = pd::getPVDataCreate()->createPVStructure(args->getStructure());
        value->copy(*args);
        return value;
    }
};
struct RejectedArgument final : std::exception {
    const char* what() const noexcept override { return "native reference rejected argument"; }
};
class Record final : public db::PVRecord {
    pa::RPCServiceAsync::shared_pointer echo{new Echo};
public:
    Record(const std::string& name) : PVRecord(name, prototype()) {}
    void process() override {
        auto value = getPVStructure();
        const auto argument = value->getSubField<pd::PVInt>("argument")->get();
        if (argument < 0) throw RejectedArgument();
        value->getSubField<pd::PVLong>("result")->put(pd::int64(argument) * 10);
        value->getSubField<pd::PVString>("action")->put("putGet");
        auto executions = value->getSubField<pd::PVLong>("executions");
        executions->put(executions->get() + 1);
    }
    pa::RPCServiceAsync::shared_pointer getService(const pd::PVStructurePtr&) override {
        return echo;
    }
};
int serve(unsigned port, const std::string& prefix) {
    auto database = db::PVDatabase::getMaster();
    shared_ptr<Record> record(new Record(prefix + "CONTROL"));
    check(record->init(), "record init failed");
    check(database->addRecord(record), "record registration failed");
    auto scalarType = pd::getFieldCreate()->createFieldBuilder()->add("value", pd::pvInt)->createStructure();
    auto unsupported = db::PVRecord::create(prefix + "UNSUPPORTED",
        pd::getPVDataCreate()->createPVStructure(scalarType));
    check(database->addRecord(unsupported), "unsupported registration failed");
    auto config = pa::ConfigurationBuilder()
        .add("EPICS_PVAS_INTF_ADDR_LIST", "127.0.0.1")
        .add("EPICS_PVAS_BEACON_ADDR_LIST", "127.0.0.1:" + std::to_string(port))
        .add("EPICS_PVAS_AUTO_BEACON_ADDR_LIST", "NO")
        .add("EPICS_PVAS_SERVER_PORT", port)
        .add("EPICS_PVAS_BROADCAST_PORT", port).push_map().build();
    auto server = pa::ServerContext::create(pa::ServerContext::Config()
        .config(config).provider(db::getChannelProviderLocal()));
    std::cout << "{\"implementation\":\"pvAccessCPP/pvDatabaseCPP\",\"tcp\":"
        << server->getServerPort() << ",\"channel\":\"" << prefix << "CONTROL\"}" << std::endl;
    for (std::string command; std::getline(std::cin, command);) {
        if (command == "quit") break;
        if (command == "stats") {
            record->lock();
            auto value = record->getPVStructure();
            std::cout << "{\"executions\":" << value->getSubField<pd::PVLong>("executions")->get()
                << ",\"argument\":" << value->getSubField<pd::PVInt>("argument")->get()
                << ",\"result\":" << value->getSubField<pd::PVLong>("result")->get() << "}" << std::endl;
            record->unlock();
        }
    }
    server->shutdown();
    return 0;
}

// A condition variable supplies a release/acquire boundary around callback data.
// Each operation has one outstanding request and every wait has a hard deadline.
struct Waiter {
    std::mutex mutex;
    std::condition_variable changed;
    bool done = false;
    pd::Status status;
    void complete(const pd::Status& value) {
        std::lock_guard<std::mutex> guard(mutex);
        status = value; done = true; changed.notify_one();
    }
    pd::Status wait(const std::string& context, bool success = true) {
        std::unique_lock<std::mutex> guard(mutex);
        check(changed.wait_for(guard, std::chrono::seconds(6), [&] { return done; }), context + " timed out");
        done = false;
        if (success) check(status.isSuccess(), context + ": " + status.getMessage());
        return status;
    }
};
#define REQUESTER_METHODS \
    std::string getRequesterName() override { return "native-reference"; } \
    void message(const std::string& text, pd::MessageType) override { std::cerr << text << std::endl; }
struct ChannelWaiter final : pa::ChannelRequester, Waiter {
    REQUESTER_METHODS
    void channelCreated(const pd::Status& status, const pa::Channel::shared_pointer&) override {
        if (!status.isSuccess()) complete(status);
    }
    void channelStateChange(const pa::Channel::shared_pointer&, pa::Channel::ConnectionState state) override {
        if (state == pa::Channel::CONNECTED) complete(pd::Status::Ok);
    }
};
struct ProcessWaiter final : pa::ChannelProcessRequester, Waiter {
    REQUESTER_METHODS
    void channelProcessConnect(const pd::Status& s, const pa::ChannelProcess::shared_pointer&) override { complete(s); }
    void processDone(const pd::Status& s, const pa::ChannelProcess::shared_pointer&) override { complete(s); }
};
struct PutGetWaiter final : pa::ChannelPutGetRequester, Waiter {
    pd::StructureConstPtr putType, getType;
    pd::PVStructurePtr value;
    pd::BitSetPtr bits;
    REQUESTER_METHODS
    void channelPutGetConnect(const pd::Status& s, const pa::ChannelPutGet::shared_pointer&,
        const pd::StructureConstPtr& put, const pd::StructureConstPtr& get) override {
        putType = put; getType = get; complete(s);
    }
    void putGetDone(const pd::Status& s, const pa::ChannelPutGet::shared_pointer&,
        const pd::PVStructurePtr& v, const pd::BitSetPtr& b) override { value = v; bits = b; complete(s); }
    void getPutDone(const pd::Status& s, const pa::ChannelPutGet::shared_pointer&,
        const pd::PVStructurePtr& v, const pd::BitSetPtr& b) override { value = v; bits = b; complete(s); }
    void getGetDone(const pd::Status& s, const pa::ChannelPutGet::shared_pointer&,
        const pd::PVStructurePtr& v, const pd::BitSetPtr& b) override { value = v; bits = b; complete(s); }
};
struct ArrayWaiter final : pa::ChannelArrayRequester, Waiter {
    pd::ArrayConstPtr type;
    pd::PVArray::shared_pointer value;
    size_t length = 0;
    REQUESTER_METHODS
    void channelArrayConnect(const pd::Status& s, const pa::ChannelArray::shared_pointer&,
        const pd::ArrayConstPtr& t) override { type = t; complete(s); }
    void getArrayDone(const pd::Status& s, const pa::ChannelArray::shared_pointer&,
        const pd::PVArray::shared_pointer& v) override { value = v; complete(s); }
    void putArrayDone(const pd::Status& s, const pa::ChannelArray::shared_pointer&) override { complete(s); }
    void getLengthDone(const pd::Status& s, const pa::ChannelArray::shared_pointer&, size_t n) override { length = n; complete(s); }
    void setLengthDone(const pd::Status& s, const pa::ChannelArray::shared_pointer&) override { complete(s); }
};
struct GetWaiter final : pa::ChannelGetRequester, Waiter {
    pd::PVStructurePtr value;
    REQUESTER_METHODS
    void channelGetConnect(const pd::Status& s, const pa::ChannelGet::shared_pointer&,
        const pd::StructureConstPtr&) override { complete(s); }
    void getDone(const pd::Status& s, const pa::ChannelGet::shared_pointer&,
        const pd::PVStructurePtr& v, const pd::BitSetPtr&) override { value = v; complete(s); }
};
struct PutWaiter final : pa::ChannelPutRequester, Waiter {
    pd::StructureConstPtr type;
    pd::PVStructurePtr value;
    REQUESTER_METHODS
    void channelPutConnect(const pd::Status& s, const pa::ChannelPut::shared_pointer&,
        const pd::StructureConstPtr& t) override { type = t; complete(s); }
    void putDone(const pd::Status& s, const pa::ChannelPut::shared_pointer&) override { complete(s); }
    void getDone(const pd::Status& s, const pa::ChannelPut::shared_pointer&,
        const pd::PVStructurePtr& v, const pd::BitSetPtr&) override { value = v; complete(s); }
};
struct RPCWaiter final : pa::ChannelRPCRequester, Waiter {
    pd::PVStructurePtr value;
    REQUESTER_METHODS
    void channelRPCConnect(const pd::Status& s, const pa::ChannelRPC::shared_pointer&) override { complete(s); }
    void requestDone(const pd::Status& s, const pa::ChannelRPC::shared_pointer&,
        const pd::PVStructurePtr& v) override { value = v; complete(s); }
};
struct MonitorWaiter final : pa::MonitorRequester, Waiter {
    std::mutex valuesMutex;
    std::vector<pd::PVStructurePtr> values;
    REQUESTER_METHODS
    void monitorConnect(const pd::Status& s, const pa::MonitorPtr&, const pd::StructureConstPtr&) override { complete(s); }
    void monitorEvent(const pa::MonitorPtr& mon) override {
        while (auto element = mon->poll()) {
            auto copy = pd::getPVDataCreate()->createPVStructure(element->pvStructurePtr->getStructure());
            copy->copy(*element->pvStructurePtr);
            { std::lock_guard<std::mutex> guard(valuesMutex); values.push_back(copy); }
            mon->release(element);
            complete(pd::Status::Ok);
        }
    }
    void unlisten(const pa::MonitorPtr&) override {}
};
#undef REQUESTER_METHODS
pd::BitSetPtr allBits() { pd::BitSetPtr bits(new pd::BitSet); bits->set(0); return bits; }
void expectArray(const pd::PVArray::shared_pointer& value, const std::vector<pd::int32>& expected) {
    auto array = dynamic_pointer_cast<pd::PVIntArray>(value);
    check(!!array, "ARRAY type is not int32[]");
    auto actual = array->view();
    if (std::vector<pd::int32>(actual.begin(), actual.end()) != expected) {
        std::string detail = "ARRAY contents changed: actual";
        for (auto v : actual) detail += " " + std::to_string(v);
        detail += "; expected";
        for (auto v : expected) detail += " " + std::to_string(v);
        throw std::runtime_error(detail);
    }
}
int client(const std::string& endpoint, const std::string& name, const std::string& unsupportedName, bool patched) {
    pa::ClientFactory::start();
    auto clientConfig = pa::ConfigurationBuilder()
        .add("EPICS_PVA_AUTO_ADDR_LIST", "NO").add("EPICS_PVA_ADDR_LIST", endpoint)
        .add("EPICS_PVA_NAME_SERVERS", "").push_map().build();
    auto provider = pa::ChannelProviderRegistry::clients()->createProvider("pva", clientConfig);
    shared_ptr<ChannelWaiter> connected(new ChannelWaiter);
    auto channel = provider->createChannel(name, connected, pa::ChannelProvider::PRIORITY_DEFAULT, endpoint);
    connected->wait("channel connect");

    shared_ptr<GetWaiter> get(new GetWaiter);
    auto getop = channel->createChannelGet(get, request("record[process=false]field()"));
    get->wait("GET init"); getop->get(); get->wait("GET");
    shared_ptr<MonitorWaiter> mon(new MonitorWaiter);
    auto monitor = channel->createMonitor(mon, request("field()"));
    mon->wait("MONITOR init"); check(monitor->start().isSuccess(), "MONITOR start"); mon->wait("MONITOR initial");

    shared_ptr<ProcessWaiter> process(new ProcessWaiter);
    auto processop = channel->createChannelProcess(process, request(""));
    process->wait("PROCESS init"); processop->process(); process->wait("PROCESS");

    shared_ptr<PutGetWaiter> pg(new PutGetWaiter);
    auto pgop = channel->createChannelPutGet(pg, request("record[process=true]putField(argument)getField(result,action)"));
    pg->wait("PUT_GET init");
    check(pg->putType->getFieldNames() == pd::StringArray(1, "argument"), "PUT_GET put prototype changed");
    check(pg->getType->getFieldNames() == pd::StringArray({"result", "action"}), "PUT_GET get prototype changed");
    check(pg->putType->getField("argument")->getType() == pd::scalar, "PUT_GET argument is not scalar");
    pgop->getPut(); pg->wait("GET_PUT");
    check(pg->value->getSubField<pd::PVInt>("argument")->get() == 42, "GET_PUT data changed");
    pgop->getGet(); pg->wait("GET_GET");
    check(pg->value->getSubField<pd::PVLong>("result")->get() == 420, "GET_GET data changed");
    auto input = pd::getPVDataCreate()->createPVStructure(pg->putType);
    input->getSubField<pd::PVInt>("argument")->put(9);
    pgop->putGet(input, allBits()); pg->wait("PUT_GET");
    check(pg->value->getSubField<pd::PVLong>("result")->get() == 90, "PUT_GET was not atomic");
    check(pg->value->getSubField<pd::PVString>("action")->get() == "putGet", "PUT_GET action changed");
    std::exception_ptr concurrentError;
    std::mutex errorMutex;
    auto atomicWorker = [&](pd::int32 first) {
        try {
            shared_ptr<PutGetWaiter> worker(new PutGetWaiter);
            auto op = channel->createChannelPutGet(worker, request("record[process=true]putField(argument)getField(result,action)"));
            worker->wait("concurrent PUT_GET init");
            auto data = pd::getPVDataCreate()->createPVStructure(worker->putType);
            for (pd::int32 index = 0; index < 32; ++index) {
                const auto argument = first + index;
                data->getSubField<pd::PVInt>("argument")->put(argument);
                op->putGet(data, allBits()); worker->wait("concurrent PUT_GET");
                check(worker->value->getSubField<pd::PVLong>("result")->get() == pd::int64(argument) * 10,
                    "concurrent PUT_GET returned another request's result");
            }
            op->destroy();
        } catch (...) {
            std::lock_guard<std::mutex> guard(errorMutex);
            if (!concurrentError) concurrentError = std::current_exception();
        }
    };
    std::thread first(atomicWorker, 1000), second(atomicWorker, 2000);
    first.join(); second.join();
    if (concurrentError) std::rethrow_exception(concurrentError);

    shared_ptr<ArrayWaiter> array(new ArrayWaiter);
    auto arrayop = channel->createChannelArray(array, request("field(value)"));
    array->wait("ARRAY init"); arrayop->getLength(); array->wait("ARRAY_GET_LENGTH");
    check(array->length == 8, "ARRAY initial length changed");
    arrayop->getArray(1, 3, 2); array->wait("ARRAY_GET"); expectArray(array->value, {11, 13, 15});
    auto update = pd::getPVDataCreate()->createPVScalarArray(pd::pvInt);
    pd::shared_vector<pd::int32> changes(2); changes[0] = 71; changes[1] = 73;
    dynamic_pointer_cast<pd::PVIntArray>(update)->replace(pd::freeze(changes));
    arrayop->putArray(update, 1, 2, 2); array->wait("ARRAY_PUT");
    arrayop->getArray(0, 0, 1); array->wait("ARRAY_GET after PUT");
    expectArray(array->value, {10, 71, 12, 73, 14, 15, 16, 17});
    arrayop->setLength(5); array->wait("ARRAY_SET_LENGTH");
    arrayop->getLength(); array->wait("ARRAY_GET_LENGTH after SET"); check(array->length == 5, "ARRAY_SET_LENGTH changed");
    if (!patched) {
        // The pinned pvData subarray copy retains a previous destination's
        // capacity as length. Use a fresh native copy after shrinking so the
        // reference test measures the wire length rather than that stock bug.
        arrayop->destroy();
        array.reset(new ArrayWaiter);
        arrayop = channel->createChannelArray(array, request("field(value)"));
        array->wait("ARRAY reinit after SET");
    }
    arrayop->getArray(0, 0, 1); array->wait("ARRAY_GET after SET"); expectArray(array->value, {10, 71, 12, 73, 14});

    shared_ptr<PutWaiter> put(new PutWaiter);
    auto putop = channel->createChannelPut(put, request(patched ? "record[process=false]field(value)" : "record[process=false]field(argument)"));
    put->wait("PUT init"); putop->get(); put->wait("PUT_GET existing");
    auto replacement = pd::getPVDataCreate()->createPVStructure(put->type);
    replacement->getSubField<pd::PVInt>(patched ? "value" : "argument")->put(77);
    putop->put(replacement, allBits()); put->wait("PUT");
    getop->get(); get->wait("GET after PUT");
    check(get->value->getSubField<pd::PVInt>(patched ? "value" : "argument")->get() == 77, "PUT not visible to GET");
    const auto monitorDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
    bool monitorUpdated = false;
    do {
        {
            std::lock_guard<std::mutex> guard(mon->valuesMutex);
            for (const auto& value : mon->values) {
                auto field = value->getSubField<pd::PVInt>(patched ? "value" : "argument");
                if (field && field->get() == 77) monitorUpdated = true;
            }
        }
        if (!monitorUpdated) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    } while (!monitorUpdated && std::chrono::steady_clock::now() < monitorDeadline);
    check(monitorUpdated, "MONITOR did not receive PUT update");
    shared_ptr<RPCWaiter> rpc(new RPCWaiter);
    auto rpcop = channel->createChannelRPC(rpc, request("")); rpc->wait("RPC init");
    auto argsType = pd::getFieldCreate()->createFieldBuilder()->add("echo", pd::pvInt)->createStructure();
    auto args = pd::getPVDataCreate()->createPVStructure(argsType); args->getSubField<pd::PVInt>("echo")->put(123);
    rpcop->request(args); rpc->wait("RPC");
    check(rpc->value->getSubField<pd::PVInt>("echo")->get() == 123, "RPC response changed");
    if (!patched) {
        // Stock ChannelPutGetLocal leaves group-put depth incremented when
        // process() throws. Exercise its error reply after monitor assertions.
        input->getSubField<pd::PVInt>("argument")->put(-1);
        pgop->putGet(input, allBits());
        auto status = pg->wait("rejected PUT_GET", false);
        check(!status.isSuccess() && status.getMessage() == "native reference rejected argument",
              "PUT_GET execution error status changed");
        input->getSubField<pd::PVInt>("argument")->put(9);
        pgop->putGet(input, allBits()); pg->wait("PUT_GET recovery");
    }

    shared_ptr<ChannelWaiter> unsupportedConnect(new ChannelWaiter);
    auto unsupported = provider->createChannel(unsupportedName, unsupportedConnect, pa::ChannelProvider::PRIORITY_DEFAULT, endpoint);
    unsupportedConnect->wait("unsupported channel connect");
    shared_ptr<ArrayWaiter> rejected(new ArrayWaiter);
    auto rejectedop = unsupported->createChannelArray(rejected, request("field(value)"));
    auto rejectedStatus = rejected->wait("unsupported ARRAY init", false);
    check(!rejectedStatus.isSuccess() && !rejectedStatus.getMessage().empty(), "unsupported ARRAY has no error status");
    if (patched) {
        shared_ptr<ProcessWaiter> badProcess(new ProcessWaiter);
        auto badProcessop = unsupported->createChannelProcess(badProcess, request(""));
        check(!badProcess->wait("unsupported PROCESS init", false).isSuccess(), "unsupported PROCESS succeeded");
        shared_ptr<PutGetWaiter> badPG(new PutGetWaiter);
        auto badPGop = unsupported->createChannelPutGet(badPG, request("putField(argument)getField(result,action)"));
        check(!badPG->wait("unsupported PUT_GET init", false).isSuccess(), "unsupported PUT_GET succeeded");
    }
    monitor->stop(); monitor->destroy();
    rejectedop->destroy(); unsupported->destroy();
    rpcop->destroy(); putop->destroy(); arrayop->destroy(); pgop->destroy(); processop->destroy(); getop->destroy();
    channel->destroy(); provider.reset(); pa::ClientFactory::stop();
    std::cout << "{\"implementation\":\"pvAccessCPP\",\"process\":true,\"put_get\":true,\"distinct_prototypes\":true,"
        "\"atomic_result\":true,\"concurrent_put_get\":64,\"get_put\":true,\"get_get\":true,\"array_slice\":true,\"array_put\":true,"
        "\"array_length\":true,\"get\":true,\"put\":true,\"monitor\":true,\"rpc\":true,\"unsupported_status\":true}" << std::endl;
    return 0;
}
}
int main(int argc, char** argv) {
    try {
        if (argc >= 3 && std::string(argv[1]) == "--server")
            return serve(unsigned(std::stoul(argv[2])), argc > 3 ? argv[3] : "");
        if (argc >= 4 && std::string(argv[1]) == "--client")
            return client(argv[2], argv[3], argc > 4 ? argv[4] : "UNSUPPORTED", argc > 5 && std::string(argv[5]) == "--patched");
        std::cerr << "usage: pva_native_reference --server PORT [PREFIX]\n"
            "       pva_native_reference --client HOST:PORT NAME [UNSUPPORTED_NAME] [--patched]\n";
        return 2;
    } catch (const std::exception& error) {
        std::cerr << "native reference: " << error.what() << std::endl;
        return 1;
    }
}
