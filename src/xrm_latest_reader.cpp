#include "redis_pvxs_ioc/xrm_latest_reader.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

#include <sw/redis++/redis++.h>

namespace redis_pvxs_ioc {
namespace {

// All large strings remain on the Redis side unless this entry satisfies the
// limits. The 32-pair/4096-byte limits apply to all fields, including unknown
// ones, and duplicate fields are rejected. Redis stream IDs have bounded size.
// The response contains a single entry, never an accumulated client queue.
constexpr const char* kReadScript = R"lua(
local kind = redis.call('TYPE', KEYS[1]).ok
if kind == 'none' then return {0} end
if kind ~= 'stream' then return {2} end
local tail = redis.call('XREVRANGE', KEYS[1], '+', '-', 'COUNT', 1)
if #tail == 0 then return {1, '0-0'} end
local latest = tail[1][1]
-- These bounded fields describe the observed tail, not an acquisition time.
-- If any is absent, duplicated or oversized, no tail metadata is returned.
local tf = tail[1][2]
local wanted = {session=128, connection=20, batch=20, sample_count=20, sample_rate_hz=20}
local found = {}
local bad = #tf > 64
if not bad then
  for i = 1, #tf, 2 do
    local name, value = tf[i], tf[i + 1]
    if wanted[name] then
      if found[name] or #value == 0 or #value > wanted[name] then bad = true; break end
      found[name] = value
    end
  end
end
local tm = {'', '', '', '', ''}
if not bad and found.session and found.connection and found.batch and
   found.sample_count and found.sample_rate_hz then
  tm = {found.session, found.connection, found.batch, found.sample_count, found.sample_rate_hz}
end
local function response(code, id)
  return {code, latest, tm[1], tm[2], tm[3], tm[4], tm[5], id}
end
local entries
if ARGV[3] == 'latest' then
  entries = tail
else
  entries = redis.call('XRANGE', KEYS[1], '(' .. ARGV[1], '+', 'COUNT', 1)
end
if #entries == 0 then return response(1, '0-0') end
local entry = entries[1]
local f = entry[2]
if #f > 64 then return response(3, entry[1]) end
local metadata = 0
local seen = {}
for i = 1, #f, 2 do
  local name, value = f[i], f[i + 1]
  if seen[name] then return response(3, entry[1]) end
  seen[name] = true
  if name == '_' then
    if #value > tonumber(ARGV[2]) then return response(3, entry[1]) end
  else
    metadata = metadata + #name + #value
  end
  if metadata > 4096 then return response(3, entry[1]) end
end
local result = response(4, entry[1])
for i = 1, #f do result[#result + 1] = f[i] end
return result
)lua";

std::string replyString(const redisReply* reply) {
  if (!reply || reply->type != REDIS_REPLY_STRING) throw std::runtime_error("invalid bounded XRM response");
  return std::string(reply->str, reply->len);
}

struct Result {
  int code = -1;
  std::string tail = "0-0", id = "0-0";
  RedisAdapter::Attrs fields;
  XrmTailMetadata tailMetadata;
};

Result parseReply(const redisReply* reply) {
  if (!reply || reply->type != REDIS_REPLY_ARRAY || !reply->elements ||
      reply->element[0]->type != REDIS_REPLY_INTEGER)
    throw std::runtime_error("invalid bounded XRM response");
  Result result;
  result.code = static_cast<int>(reply->element[0]->integer);
  if (result.code < 0 || result.code > 4) throw std::runtime_error("invalid bounded XRM response");
  if (result.code == 0 || result.code == 2) {
    if (reply->elements != 1) throw std::runtime_error("invalid bounded XRM response");
    return result;
  }
  if (reply->elements < 2) throw std::runtime_error("invalid bounded XRM response");
  result.tail = replyString(reply->element[1]);
  if (result.code == 1 && reply->elements == 2) {
    return result;
  }
  if (reply->elements < 8 || (reply->elements - 8) % 2 || reply->elements > 72)
    throw std::runtime_error("invalid bounded XRM response");
  result.tailMetadata.session = replyString(reply->element[2]);
  result.tailMetadata.connection = replyString(reply->element[3]);
  result.tailMetadata.batch = replyString(reply->element[4]);
  result.tailMetadata.sampleCount = replyString(reply->element[5]);
  result.tailMetadata.sampleRate = replyString(reply->element[6]);
  result.tailMetadata.present = !result.tailMetadata.session.empty();
  result.id = replyString(reply->element[7]);
  if ((result.code == 1 || result.code == 3) && reply->elements != 8)
    throw std::runtime_error("invalid bounded XRM response");
  for (size_t i = 8; i < reply->elements; i += 2)
    result.fields.emplace(replyString(reply->element[i]), replyString(reply->element[i + 1]));
  return result;
}

}  // namespace

struct XrmLatestReader::State {
  State(RedisConfig backend, std::string subkey, uint64_t payloadLimit, uint32_t poll,
        RedisAdapter::MetadataStreamCallback onEntry)
      : base(std::move(backend.baseKey)), key(std::move(subkey)), maxBytes(payloadLimit),
        pollMs(std::max<uint32_t>(1, poll)), callback(std::move(onEntry)) {
    if (base.empty() || key.empty() || !maxBytes || !callback)
      throw std::invalid_argument("invalid bounded XRM reader configuration");
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
    bool deliver = false, immediate = false;
  };

  Reading read(bool initial) {
    std::string cursor;
    {
      std::lock_guard<std::mutex> guard(mutex);
      cursor = status.cursor;
    }
    Result result;
    bool rejected = false, unavailable = false;
    try {
      const auto redisKey = "{" + base + "}:" + key;
      const auto reply = client->command("EVAL", kReadScript, "1", redisKey, cursor,
                                         std::to_string(maxBytes), initial ? "latest" : "next");
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
      return reading;
    }
    if (!status.connected && everConnected) ++status.reconnects;
    status.connected = true;
    everConnected = true;
    reading.snapshot.connected = true;
    if (rejected) {
      reading.snapshot.rejected = true;
      ++status.readRejections;
      return reading;
    }
    status.inspected = true;
    if (result.code == 0 || result.code == 2) {
      if (status.hasData) {
        ++status.epoch;
        if (result.code == 0) ++status.disappearances;
        else ++status.streamResets;
        status.cursor = "0-0";
      }
      status.hasData = false;
      status.observedCursor.clear();
      tail = {};
      status.streamKind = result.code == 0 ? RedisAdapter::StreamKind::Missing : RedisAdapter::StreamKind::Invalid;
      reading.snapshot.id = "0-0";
      reading.snapshot.rejected = result.code == 2;
      return reading;
    }
    status.streamKind = RedisAdapter::StreamKind::Stream;
    status.observedCursor = result.tail;
    tail = std::move(result.tailMetadata);
    if (!initial && status.cursor != "0-0" && RedisAdapter::compareStreamIds(result.tail, status.cursor) < 0) {
      ++status.epoch;
      ++status.streamResets;
      status.cursor = "0-0";
      status.hasData = result.tail != "0-0";
      reading.immediate = true;
      return reading;
    }
    status.hasData = result.tail != "0-0";
    if (result.code == 1) {
      reading.snapshot.id = "0-0";
      return reading;
    }
    status.cursor = result.id;
    reading.snapshot.id = result.id;
    reading.immediate = true;
    if (result.code == 3) {
      reading.snapshot.rejected = true;
      ++status.readRejections;
      // Immediately invalidate the retained waveform. An empty field map is
      // unambiguously invalid and carries no rejected bytes across the wire.
      reading.metadata = {status.epoch, status.readRejections};
      reading.deliver = !initial;
      return reading;
    }
    reading.snapshot.fields = std::move(result.fields);
    status.lastReceived = std::chrono::steady_clock::now();
    ++status.entries;
    reading.metadata = {status.epoch, status.readRejections};
    reading.deliver = !initial;
    return reading;
  }

  void run() {
    for (;;) {
      {
        std::lock_guard<std::mutex> guard(mutex);
        if (!status.active) break;
      }
      auto reading = read(false);
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
      if (!reading.immediate) {
        std::unique_lock<std::mutex> lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(pollMs), [&] { return !status.active; });
      }
    }
  }

  std::string base, key;
  uint64_t maxBytes;
  uint32_t pollMs;
  RedisAdapter::MetadataStreamCallback callback;
  std::unique_ptr<sw::redis::Redis> client;
  mutable std::mutex mutex;
  std::condition_variable wake;
  RedisAdapter::ReaderStatus status;
  XrmTailMetadata tail;
  bool everConnected = false, snapped = false, started = false;
  std::thread worker;
};

XrmLatestReader::XrmLatestReader(RedisConfig backend, std::string key, uint64_t maxPayloadBytes,
                                 uint32_t idlePollMs, RedisAdapter::MetadataStreamCallback callback)
    : state_(std::make_shared<State>(std::move(backend), std::move(key), maxPayloadBytes, idlePollMs,
                                     std::move(callback))) {}

XrmLatestReader::~XrmLatestReader() { reset(); }

RedisAdapter::StreamSnapshot XrmLatestReader::snapshot() {
  {
    std::lock_guard<std::mutex> guard(state_->mutex);
    if (state_->snapped || state_->started || !state_->status.active)
      throw std::logic_error("XRM snapshot must be read once before start");
    state_->snapped = true;
  }
  return state_->read(true).snapshot;
}

void XrmLatestReader::start() {
  std::lock_guard<std::mutex> guard(state_->mutex);
  if (!state_->snapped || state_->started || !state_->status.active)
    throw std::logic_error("XRM reader cannot be started");
  state_->started = true;
  auto state = state_;
  state_->worker = std::thread([state] { state->run(); });
}

void XrmLatestReader::reset() noexcept {
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

RedisAdapter::ReaderStatus XrmLatestReader::status() const {
  std::lock_guard<std::mutex> guard(state_->mutex);
  return state_->status;
}

XrmTailMetadata XrmLatestReader::tailMetadata() const {
  std::lock_guard<std::mutex> guard(state_->mutex);
  return state_->tail;
}

}  // namespace redis_pvxs_ioc
