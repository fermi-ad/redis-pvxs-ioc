#include "redis_pvxs_ioc/xrm_hash_reader.h"
#include "redis_pvxs_ioc/xrm_contract.h"

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <sw/redis++/redis++.h>

namespace redis_pvxs_ioc {
namespace {

// TYPE/OBJECT/MEMORY/HLEN and HSTRLEN never return stored field values. A
// compact representation is capped before HLEN or field lookup can scan it.
// Unknown encodings are rejected. Each known value is length-checked before
// *any* HGET; no stream entry or unrecognized hash field is materialized.
constexpr const char* kReadHash = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
if kind == 'none' then return {0} end
if kind ~= 'hash' then return {2} end
if redis.call('TYPE', KEYS[2]).ok ~= 'string' then return {3} end
local markerSize = redis.call('STRLEN', KEYS[2])
if markerSize == 0 or markerSize > 172 then return {3} end
local enc = redis.call('OBJECT', 'ENCODING', KEYS[1])
if enc == 'listpack' or enc == 'ziplist' then
  if redis.call('MEMORY', 'USAGE', KEYS[1], 'SAMPLES', 1) > 65536 then return {3} end
elseif enc ~= 'hashtable' then return {3} end
if redis.call('HLEN', KEYS[1]) > 32 then return {3} end
local fields = {
  {'_', tonumber(ARGV[2]), true}, {'schema', 64, true}, {'sample_bytes', 20, true},
  {'sample_rate_hz', 20, true}, {'sample_count', 20, true}, {'channel', 20, true},
  {'session', 128, true}, {'connection', 20, true}, {'batch', 20, true},
  {'source_validity', 16, true}, {'acq_time_valid', 1, true}, {'acq_time_ns', 20, false},
  {'contract', 64, true}, {'state', 16, true}, {'stream_id', 41, true}, {'commit', 172, true}
}
for _, f in ipairs(fields) do
  local size = redis.call('HSTRLEN', KEYS[1], f[1])
  if size > f[2] or (f[3] and size == 0) then return {3} end
end
local token = redis.call('HGET', KEYS[1], 'commit')
if redis.call('GET', KEYS[2]) ~= token or
   redis.call('HGET', KEYS[1], 'contract') ~= 'fermi-ad/xrm-latest:1' or
   redis.call('HGET', KEYS[1], 'state') ~= 'ready' then return {3} end
local expected = redis.call('HGET', KEYS[1], 'session') .. '/' ..
                 redis.call('HGET', KEYS[1], 'connection') .. '/' .. redis.call('HGET', KEYS[1], 'batch')
if token ~= expected then return {3} end
local id = redis.call('HGET', KEYS[1], 'stream_id')
if id == ARGV[1] and token == ARGV[4] and ARGV[3] ~= 'force' then return {1, id, token} end
local result = {4, id, token}
for i = 1, 12 do
  local name = fields[i][1]
  local value = redis.call('HGET', KEYS[1], name)
  if value then result[#result + 1] = name; result[#result + 1] = value end
end
return result
)lua";

std::string stringReply(const redisReply* reply) {
  if (!reply || reply->type != REDIS_REPLY_STRING) throw std::runtime_error("invalid XRM hash response");
  return {reply->str, reply->len};
}

struct Result {
  int code = -1;
  std::string id, token;
  RedisAdapter::Attrs fields;
};

Result parseReply(const redisReply* reply) {
  if (!reply || reply->type != REDIS_REPLY_ARRAY || !reply->elements ||
      reply->element[0]->type != REDIS_REPLY_INTEGER)
    throw std::runtime_error("invalid XRM hash response");
  Result result;
  result.code = static_cast<int>(reply->element[0]->integer);
  if (result.code == 0 || result.code == 2 || result.code == 3) {
    if (reply->elements != 1) throw std::runtime_error("invalid XRM hash response");
    return result;
  }
  if ((result.code != 1 && result.code != 4) || reply->elements < 3 ||
      (reply->elements - 3) % 2 || reply->elements > 27 || (result.code == 1 && reply->elements != 3))
    throw std::runtime_error("invalid XRM hash response");
  result.id = stringReply(reply->element[1]);
  result.token = stringReply(reply->element[2]);
  if (RedisAdapter::compareStreamIds(result.id, "0-0") <= 0) throw std::runtime_error("invalid XRM stream ID");
  for (size_t i = 3; i < reply->elements; i += 2)
    if (!result.fields.emplace(stringReply(reply->element[i]), stringReply(reply->element[i+1])).second)
      throw std::runtime_error("duplicate XRM hash field");
  return result;
}

}  // namespace

struct XrmHashReader::State {
  State(RedisConfig backend, std::string subkey, uint64_t limit, uint32_t poll,
        RedisAdapter::MetadataStreamCallback onEntry)
      : base(std::move(backend.baseKey)), key(std::move(subkey)), maxBytes(limit),
        pollMs(std::max<uint32_t>(1, poll)), callback(std::move(onEntry)) {
    if (base.empty() || key.empty() || !maxBytes || !callback)
      throw std::invalid_argument("invalid XRM hash reader configuration");
    sw::redis::ConnectionOptions options;
    options.host = backend.host;
    options.port = backend.port;
    if (!backend.user.empty()) options.user = backend.user;
    options.password = backend.password;
    options.connect_timeout = std::chrono::milliseconds(500);
    options.socket_timeout = std::chrono::milliseconds(500);
    sw::redis::ConnectionPoolOptions pool;
    pool.size = 1;
    client = std::make_unique<sw::redis::Redis>(options, pool);
    status.active = true;
    status.epoch = 1;
    status.cursor = "0-0";
  }

  struct Reading {
    RedisAdapter::StreamSnapshot snapshot;
    RedisAdapter::StreamBatchMetadata metadata;
    bool deliver = false;
  };

  Reading read(bool notify) {
    std::string cursor, priorToken;
    bool baseline;
    {
      std::lock_guard<std::mutex> guard(mutex);
      cursor = status.cursor;
      priorToken = token;
      baseline = needsBaseline;
    }
    Result result;
    bool rejected = false, unavailable = false;
    const auto startedAt = std::chrono::steady_clock::now();
    try {
      const auto stream = "{" + base + "}:" + key;
      const auto reply = client->command("EVAL", kReadHash, "2", xrmLatestHashKey(stream),
                                         xrmCommitMarkerKey(stream), cursor,
                                         std::to_string(maxBytes), baseline ? "force" : "normal", priorToken);
      result = parseReply(reply.get());
    } catch (const sw::redis::ReplyError&) {
      rejected = true;
    } catch (const sw::redis::Error&) {
      unavailable = true;
    } catch (const std::exception&) {
      rejected = true;
    }

    Reading reading;
    std::lock_guard<std::mutex> guard(mutex);
    if (!status.active) return reading;
    if (unavailable) {
      status.connected = false;
      ++status.readFailures;
      needsBaseline = true;
      proof = {};
      return reading;
    }
    if (!status.connected && everConnected) ++status.reconnects;
    status.connected = true;
    everConnected = true;
    reading.snapshot.connected = true;
    if (rejected || result.code == 2 || result.code == 3) {
      needsBaseline = true;
      proof = {};
      if (result.code == 2) status.streamKind = RedisAdapter::StreamKind::Invalid;
      if (!wasRejected) {
        ++status.readRejections;
        reading.snapshot.rejected = true;
        reading.metadata = {status.epoch, status.readRejections};
        reading.deliver = notify;
      }
      wasRejected = true;
      return reading;
    }
    wasRejected = false;
    status.inspected = true;
    if (result.code == 0) {
      if (status.hasData) { ++status.epoch; ++status.disappearances; }
      status.hasData = false;
      status.streamKind = RedisAdapter::StreamKind::Missing;
      status.cursor = "0-0";
      token.clear();
      status.observedCursor.clear();
      needsBaseline = false;
      proof = {};
      previousPoll = startedAt;
      return reading;
    }
    status.streamKind = RedisAdapter::StreamKind::Stream;
    status.hasData = true;
    status.observedCursor = result.id;
    if (result.code == 1) { previousPoll = startedAt; return reading; }
    if (!baseline && cursor != "0-0" && RedisAdapter::compareStreamIds(result.id, cursor) <= 0) {
      ++status.epoch;
      ++status.streamResets;
      baseline = true;
    }
    // A baseline, including after an outage or reset, is not proof that the
    // retained value was acquired recently. Only a later ID change is proof.
    proof = baseline ? std::chrono::steady_clock::time_point{} : previousPoll;
    needsBaseline = false;
    previousPoll = startedAt;
    status.cursor = result.id;
    token = std::move(result.token);
    status.lastReceived = std::chrono::steady_clock::now();
    ++status.entries;
    reading.snapshot.id = result.id;
    reading.snapshot.fields = std::move(result.fields);
    reading.metadata = {status.epoch, status.readRejections};
    reading.deliver = notify;
    return reading;
  }

  void run() {
    for (;;) {
      {
        std::lock_guard<std::mutex> guard(mutex);
        if (!status.active) break;
      }
      auto reading = read(true);
      if (reading.deliver) {
        RedisAdapter::StreamBatch batch;
        batch.emplace_back(std::move(reading.snapshot.id), std::move(reading.snapshot.fields));
        try {
          callback(base, key, batch, reading.metadata);
          std::lock_guard<std::mutex> guard(mutex);
          ++status.callbacks;
        } catch (...) {
          std::lock_guard<std::mutex> guard(mutex);
          ++status.callbackErrors;
        }
      }
      std::unique_lock<std::mutex> lock(mutex);
      wake.wait_for(lock, std::chrono::milliseconds(pollMs), [&] { return !status.active; });
    }
  }

  std::string base, key, token;
  uint64_t maxBytes;
  uint32_t pollMs;
  RedisAdapter::MetadataStreamCallback callback;
  std::unique_ptr<sw::redis::Redis> client;
  mutable std::mutex mutex;
  std::condition_variable wake;
  RedisAdapter::ReaderStatus status;
  std::chrono::steady_clock::time_point previousPoll{}, proof{};
  bool needsBaseline = true, wasRejected = false, everConnected = false, snapped = false, started = false;
  std::thread worker;
};

XrmHashReader::XrmHashReader(RedisConfig backend, std::string key, uint64_t maxPayloadBytes,
                             uint32_t idlePollMs, RedisAdapter::MetadataStreamCallback callback)
    : state_(std::make_shared<State>(std::move(backend), std::move(key), maxPayloadBytes, idlePollMs,
                                     std::move(callback))) {}
XrmHashReader::~XrmHashReader() { reset(); }

RedisAdapter::StreamSnapshot XrmHashReader::snapshot() {
  {
    std::lock_guard<std::mutex> guard(state_->mutex);
    if (state_->snapped || state_->started || !state_->status.active)
      throw std::logic_error("XRM snapshot must be read once before start");
    state_->snapped = true;
  }
  return state_->read(false).snapshot;
}

void XrmHashReader::start() {
  std::lock_guard<std::mutex> guard(state_->mutex);
  if (!state_->snapped || state_->started || !state_->status.active)
    throw std::logic_error("XRM reader cannot be started");
  state_->started = true;
  auto state = state_;
  state_->worker = std::thread([state] { state->run(); });
}

void XrmHashReader::reset() noexcept {
  if (!state_) return;
  {
    std::lock_guard<std::mutex> guard(state_->mutex);
    state_->status.active = false;
    state_->status.connected = false;
  }
  state_->wake.notify_all();
  if (state_->worker.joinable()) {
    if (state_->worker.get_id() == std::this_thread::get_id()) state_->worker.detach();
    else state_->worker.join();
  }
}

RedisAdapter::ReaderStatus XrmHashReader::status() const {
  std::lock_guard<std::mutex> guard(state_->mutex);
  return state_->status;
}

std::chrono::steady_clock::time_point XrmHashReader::freshnessProof() const {
  std::lock_guard<std::mutex> guard(state_->mutex);
  return state_->proof;
}

}  // namespace redis_pvxs_ioc
