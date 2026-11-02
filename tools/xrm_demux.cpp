#include "xrm_demux.h"

#include "hiredis.h"
#include <yaml-cpp/yaml.h>

#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <fstream>
#include <fcntl.h>
#include <iomanip>
#include <limits>
#include <mutex>
#include <poll.h>
#include <set>
#include <sstream>
#include <stdexcept>
#include <thread>

namespace xrm_demux {
namespace {
constexpr size_t maxConfig = 1024u * 1024u, maxPayload = 1024u * 1024u;
constexpr size_t maxSources = 32, maxChannels = 64;
using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;
using Context = std::unique_ptr<redisContext, decltype(&redisFree)>;
struct InputError : std::runtime_error { using std::runtime_error::runtime_error; };

void require(bool yes, const std::string& message) {
  if (!yes) throw std::runtime_error(message);
}
void inputRequire(bool yes, const std::string& message) {
  if (!yes) throw InputError(message);
}
void keys(const YAML::Node& node, std::initializer_list<const char*> allowed, const std::string& path) {
  require(node.IsMap(), path + " must be a mapping");
  std::set<std::string> seen;
  for (const auto& pair : node) {
    require(pair.first.IsScalar(), path + " has a non-scalar field");
    const auto key = pair.first.as<std::string>();
    require(seen.insert(key).second, path + " has duplicate field " + key);
    require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(), path + " has unknown field " + key);
  }
}
std::string text(const YAML::Node& node, const std::string& field, size_t limit = 512) {
  require(node.IsScalar(), field + " is required and must be scalar");
  const auto result = node.as<std::string>();
  require(!result.empty() && result.size() <= limit, field + " length is invalid");
  for (unsigned char ch : result) require(ch > 0x20 && ch < 0x7f, field + " must contain printable ASCII without spaces");
  return result;
}
uint64_t integer(const std::string& input, uint64_t maximum, const std::string& field) {
  uint64_t value = 0;
  const auto parsed = std::from_chars(input.data(), input.data() + input.size(), value);
  require(!input.empty() && parsed.ec == std::errc{} && parsed.ptr == input.data() + input.size() && value <= maximum,
          field + " must be an unsigned decimal integer within its configured bound");
  return value;
}
unsigned number(const YAML::Node& node, unsigned fallback, unsigned minimum, unsigned maximum, const std::string& field) {
  if (!node) return fallback;
  auto value = integer(text(node, field), maximum, field);
  require(value >= minimum, field + " is below its minimum");
  return static_cast<unsigned>(value);
}
void streamId(const std::string& id) {
  const auto dash = id.find('-');
  require(dash != std::string::npos && id.size() <= 41, "invalid Redis stream ID");
  const auto first = id.substr(0, dash), second = id.substr(dash + 1);
  integer(first, UINT64_MAX, "stream ID milliseconds");
  integer(second, UINT64_MAX, "stream ID sequence");
  require((first == "0" || first[0] != '0') && (second == "0" || second[0] != '0'), "Redis stream ID must be canonical");
}
float calibration(const YAML::Node& node, const std::string& field) {
  require(node.IsScalar(), field + " must be supplied explicitly");
  const double value = node.as<double>();
  const auto result = static_cast<float>(value);
  require(std::isfinite(value) && std::isfinite(result), field + " must be finite float32");
  return result;
}
std::string fingerprint(const Source& source) {
  std::ostringstream data;
  const auto part = [&](const std::string& value) { data << value.size() << ':' << value; };
  part(source.stream); part(source.checkpoint); part(source.payloadField); part(source.startId);
  part(source.timestampMode); part(source.timestampField); part(source.timestampValidField);
  part(std::to_string(source.samplesPerChannel)); part(std::to_string(source.retention));
  for (const auto& channel : source.channels) {
    part(std::to_string(channel.channel));
    part(channel.raw); part(channel.volts);
    uint32_t slope, offset;
    std::memcpy(&slope, &channel.eslo, sizeof slope); std::memcpy(&offset, &channel.eoff, sizeof offset);
    part(std::to_string(slope)); part(std::to_string(offset));
  }
  uint64_t hash = UINT64_C(14695981039346656037);
  for (unsigned char ch : data.str()) { hash ^= ch; hash *= UINT64_C(1099511628211); }
  std::ostringstream value;
  value << "xrm-demux-1-" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return value.str();
}
std::string str(const redisReply* reply) {
  require(reply && (reply->type == REDIS_REPLY_STRING || reply->type == REDIS_REPLY_STATUS || reply->type == REDIS_REPLY_ERROR), "unexpected Redis reply");
  return {reply->str, reply->len};
}
Reply command(redisContext* context, const std::vector<std::string>& args,
              unsigned timeoutMs, const std::atomic<bool>& stopping) {
  struct PendingCommand {
    redisContext* context;
    bool replied = false;
    ~PendingCommand() {
      // Never reuse a connection whose unanswered command may still deliver a
      // late reply. A later source must not consume another source's response.
      if (!replied) context->err = REDIS_ERR_IO;
    }
  } pending{context};
  std::vector<const char*> argv;
  std::vector<size_t> sizes;
  for (const auto& arg : args) { argv.push_back(arg.data()); sizes.push_back(arg.size()); }
  require(redisAppendCommandArgv(context, static_cast<int>(args.size()), argv.data(), sizes.data()) == REDIS_OK, "cannot buffer Redis command");
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
  const auto ready = [&](short event) {
    for (;;) {
      require(!stopping, "demux worker stopping");
      const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
      require(remaining > 0, "Redis command deadline exceeded");
      pollfd fd{context->fd, event, 0};
      const auto result = ::poll(&fd, 1, static_cast<int>(std::min<int64_t>(remaining, 50)));
      if (result < 0 && errno == EINTR) continue;
      require(result >= 0, "Redis readiness polling failed");
      if (result > 0) {
        require(!(fd.revents & (POLLERR | POLLHUP | POLLNVAL)), "Redis connection closed");
        if (fd.revents & event) return;
      }
    }
  };
  int done = 0;
  while (!done) {
    ready(POLLOUT);
    require(redisBufferWrite(context, &done) == REDIS_OK, "Redis write failure");
  }
  void* object = nullptr;
  while (!object) {
    require(!stopping, "demux worker stopping");
    require(std::chrono::steady_clock::now() < deadline, "Redis command deadline exceeded");
    require(redisGetReplyFromReader(context, &object) == REDIS_OK, "invalid Redis response");
    if (object) break;
    ready(POLLIN);
    require(redisBufferRead(context) == REDIS_OK, "Redis read failure");
  }
  Reply reply(static_cast<redisReply*>(object), &freeReplyObject);
  pending.replied = true;
  if (reply->type == REDIS_REPLY_ERROR) throw std::runtime_error("Redis rejected demux operation: " + str(reply.get()));
  return reply;
}

// XRANGE is deliberately COUNT 1. This server-side gate returns at most one
// exact-sized payload plus bounded metadata, even if the producer wrote an
// oversized entry. No unbounded stream contents reach the client.
const char* readScript = R"LUA(
local function typ(k) return redis.call('TYPE', k).ok end
local st, ct = typ(KEYS[1]), typ(KEYS[2])
if ct ~= 'none' and ct ~= 'hash' then return redis.error_reply('checkpoint must be a hash') end
local cfg = redis.call('HGET', KEYS[2], 'config')
if cfg and cfg ~= ARGV[1] then return redis.error_reply('checkpoint configuration mismatch') end
if not cfg then
  if ct ~= 'none' then return redis.error_reply('checkpoint is not owned by demux') end
  redis.call('HSET', KEYS[2], 'config', ARGV[1], 'source_stream', KEYS[1], 'last_id', ARGV[2], 'processed', 0, 'rejected', 0, 'gaps', 0)
end
if redis.call('HGET', KEYS[2], 'source_stream') ~= KEYS[1] then return redis.error_reply('checkpoint source mismatch') end
local cursor = redis.call('HGET', KEYS[2], 'last_id')
if not cursor or string.len(cursor) > 41 or not string.match(cursor, '^%d+%-%d+$') then return redis.error_reply('invalid checkpoint cursor') end
local function response(kind,id,payload,stamp,valid,gap,reason)
  if kind == 'reject' then return {'reject',cursor,id,reason,gap} end
  return {'entry',cursor,id,payload,stamp,valid,gap}
end
-- Retain one bounded pending input before any fanout write. Producer trimming
-- can remove the source row while a write fails or the worker is stopped;
-- replay must use the original bytes AND continuity decision, not reread them.
local pending = redis.call('HGET',KEYS[2],'pending_id')
if pending and pending ~= '' then
  if string.len(pending) > 41 or not string.match(pending,'^%d+%-%d+$') then return redis.error_reply('invalid pending source ID') end
  local p = redis.call('HMGET',KEYS[2],'pending_kind','pending_payload','pending_timestamp','pending_valid','pending_gap','pending_reason')
  if not p[1] or not p[2] or not p[3] or not p[4] or not p[5] or not p[6] then return redis.error_reply('incomplete pending input') end
  if (p[1] ~= 'entry' and p[1] ~= 'reject') or (p[5] ~= '0' and p[5] ~= '1') or string.len(p[3]) > 32 or string.len(p[4]) > 1 or string.len(p[6]) > 256 then return redis.error_reply('invalid pending metadata') end
  if (p[1] == 'entry' and string.len(p[2]) ~= tonumber(ARGV[4])) or (p[1] == 'reject' and string.len(p[2]) ~= 0) then return redis.error_reply('invalid pending payload size') end
  return response(p[1],pending,p[2],p[3],p[4],p[5],p[6])
end
if st ~= 'none' and st ~= 'stream' then return redis.error_reply('source must be a stream') end
local rows = redis.call('XRANGE', KEYS[1], '(' .. cursor, '+', 'COUNT', 1)
if #rows == 0 then return {'empty', cursor} end
local row, gap = rows[1], '0'
if cursor ~= '0-0' then
  local previous = redis.call('XRANGE', KEYS[1], cursor, cursor, 'COUNT', 1)
  if #previous == 0 then gap = '1' end
end
local id, fields = row[1], row[2]
local function retain(kind,payload,stamp,valid,reason)
  redis.call('HSET',KEYS[2],'pending_id',id,'pending_kind',kind,'pending_payload',payload,
    'pending_timestamp',stamp,'pending_valid',valid,'pending_gap',gap,'pending_reason',reason)
  return response(kind,id,payload,stamp,valid,gap,reason)
end
local function reject(reason) return retain('reject','','','',reason) end
if #fields > 6 or #fields % 2 ~= 0 then return reject('unsupported field count') end
local payload, stamp, valid = nil, '', ''
local seen = {}
for i = 1, #fields, 2 do
  local k, v = fields[i], fields[i + 1]
  if seen[k] then return reject('duplicate producer field') end
  seen[k] = true
  if k == ARGV[3] then
    if string.len(v) ~= tonumber(ARGV[4]) then return reject('payload size does not match configured complete pulse') end
    payload = v
  elseif ARGV[5] ~= '' and k == ARGV[5] then
    if string.len(v) > 32 then return reject('acquisition timestamp exceeds bound') end
    stamp = v
  elseif ARGV[6] ~= '' and k == ARGV[6] then
    if string.len(v) > 1 then return reject('acquisition validity exceeds bound') end
    valid = v
  else return reject('unsupported producer field or mixed layout') end
end
if not payload then return reject('payload field missing') end
return retain('entry',payload,stamp,valid,'')
)LUA";

// Preflight ALL types, IDs, checkpoint, and existing same-ID records before
// the first write. Identical already-written records are skipped on retry:
// Lua errors/OOM do not roll back earlier writes, and a lost reply may hide a
// successful commit. Compare-and-set on the cursor also permits two workers
// to race safely without duplicating fanout.
const char* commitScript = R"LUA(
local function typ(k) return redis.call('TYPE', k).ok end
local function greater(a,b)
  local am,as = string.match(a, '^(%d+)%-(%d+)$')
  local bm,bs = string.match(b, '^(%d+)%-(%d+)$')
  if #am ~= #bm then return #am > #bm end
  if am ~= bm then return am > bm end
  if #as ~= #bs then return #as > #bs end
  return as > bs
end
if typ(KEYS[1]) ~= 'hash' then return redis.error_reply('checkpoint must be a hash') end
if redis.call('HGET',KEYS[1],'config') ~= ARGV[1] then return redis.error_reply('checkpoint configuration mismatch') end
if redis.call('HGET',KEYS[1],'last_id') ~= ARGV[2] then return 'stale' end
if redis.call('HGET',KEYS[1],'pending_id') ~= ARGV[3] then return redis.error_reply('pending source ID mismatch') end
if not greater(ARGV[3],ARGV[2]) then return redis.error_reply('non-increasing source ID') end
-- Counters must be safe before any output is appended.
local counters = {}
for _, name in ipairs({'processed','rejected','gaps'}) do
  local v = redis.call('HGET',KEYS[1],name)
  if not v or not string.match(v,'^%d+$') or #v > 15 then return redis.error_reply('invalid checkpoint counter') end
  counters[name] = tonumber(v)
end
local id, count, pos = ARGV[3], tonumber(ARGV[7]), 8
if count ~= #KEYS - 1 then return redis.error_reply('output count mismatch') end
local records = {}
for n = 1,count do
  local t = typ(KEYS[n+1])
  if t ~= 'none' and t ~= 'stream' then return redis.error_reply('output must be a stream') end
  local length = tonumber(ARGV[pos]); pos = pos + 1
  local fields, same = {}, false
  for j = 1,length do fields[j] = ARGV[pos]; pos = pos + 1 end
  local old = redis.call('XRANGE',KEYS[n+1],id,id,'COUNT',1)
  if #old ~= 0 then
    local existing = old[1][2]
    if #existing ~= #fields then return redis.error_reply('same-ID output collision') end
    local values = {}
    for j=1,#existing,2 do
      if values[existing[j]] then return redis.error_reply('duplicate existing output field') end
      values[existing[j]]=existing[j+1]
    end
    for j=1,#fields,2 do
      if values[fields[j]] ~= fields[j+1] then return redis.error_reply('same-ID output collision') end
    end
    same = true
  else
    local last = redis.call('XREVRANGE',KEYS[n+1],'+','-','COUNT',1)
    if #last ~= 0 and not greater(id,last[1][1]) then return redis.error_reply('output ID precedes existing history') end
  end
  records[n] = {fields=fields,same=same}
end
for n=1,count do
  if not records[n].same then
    redis.call('XADD',KEYS[n+1],'MAXLEN','=',ARGV[4],id,unpack(records[n].fields))
  end
end
local counter = count == 0 and 'rejected' or 'processed'
counters[counter] = counters[counter] + 1
if ARGV[6] == '1' then counters.gaps = counters.gaps + 1 end
-- A single final Redis command commits cursor and counters together. There
-- are no commands after this point that could leave a committed cursor with
-- an uncounted rejection or missing continuity record.
local checkpoint = {'last_id',id,'last_error',ARGV[5],
  'pending_id','','pending_kind','','pending_payload','','pending_timestamp','','pending_valid','','pending_gap','','pending_reason','',
  'processed',string.format('%.0f',counters.processed),
  'rejected',string.format('%.0f',counters.rejected),
  'gaps',string.format('%.0f',counters.gaps)}
if count == 0 then
  table.insert(checkpoint,'last_rejected_id'); table.insert(checkpoint,id)
  table.insert(checkpoint,'last_rejection'); table.insert(checkpoint,ARGV[5])
end
if ARGV[6] == '1' then table.insert(checkpoint,'last_gap_id'); table.insert(checkpoint,id) end
redis.call('HSET',KEYS[1],unpack(checkpoint))
return 'committed'
)LUA";

void floatBytes(std::string& bytes, size_t offset, float value) {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE754 float32 required");
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<char>((bits >> (8 * i)) & 0xffu);
}
} // namespace

Config parseConfig(const std::string& yaml) {
  require(yaml.size() <= maxConfig, "demux config exceeds 1 MiB");
  const auto docs = YAML::LoadAll(yaml);
  require(docs.size() == 1, "demux config requires exactly one YAML document");
  const auto root = docs.front();
  keys(root, {"schema_version", "redis", "sources"}, "demux");
  require(root["schema_version"] && root["schema_version"].as<int>() == 1, "unsupported demux schema_version");
  Config config;
  const auto redis = root["redis"];
  keys(redis, {"host", "port", "db", "connect_timeout_ms", "command_timeout_ms", "poll_interval_ms", "retry_ms"}, "redis");
  config.host = text(redis["host"], "redis.host");
  in_addr address{};
  require(inet_pton(AF_INET, config.host.c_str(), &address) == 1, "redis.host requires numeric IPv4 to bound connection time");
  config.port = number(redis["port"], 6379, 1, 65535, "redis.port");
  require(config.port != 4210, "Redis cannot use digitizer acquisition port 4210");
  config.db = number(redis["db"], 0, 0, 15, "redis.db");
  config.connectTimeoutMs = number(redis["connect_timeout_ms"], 200, 1, 2000, "connect_timeout_ms");
  config.commandTimeoutMs = number(redis["command_timeout_ms"], 500, 1, 2000, "command_timeout_ms");
  config.pollIntervalMs = number(redis["poll_interval_ms"], 100, 1, 10000, "poll_interval_ms");
  config.retryMs = number(redis["retry_ms"], 1000, 10, 30000, "retry_ms");
  const auto sources = root["sources"];
  require(sources.IsSequence() && sources.size() > 0 && sources.size() <= maxSources, "sources requires 1 to 32 entries");
  std::set<std::string> allKeys;
  for (const auto& node : sources) {
    keys(node, {"stream", "checkpoint", "payload_field", "format", "layout", "samples_per_channel", "start_id", "retention", "timestamp", "channels"}, "source");
    Source source;
    source.stream = text(node["stream"], "source.stream");
    source.checkpoint = text(node["checkpoint"], "source.checkpoint");
    require(allKeys.insert(source.stream).second && allKeys.insert(source.checkpoint).second, "source/checkpoint/output keys must be globally disjoint");
    if (node["payload_field"]) source.payloadField = text(node["payload_field"], "payload_field", 128);
    require(text(node["format"], "format") == "int16-le", "only signed int16-le ADC input is supported; mixed/SPAD layouts require a producer decoder");
    require(text(node["layout"], "layout") == "sample-major", "only aligned sample-major layout is supported");
    source.samplesPerChannel = number(node["samples_per_channel"], 0, 1, maxPayload / 2, "samples_per_channel");
    require(source.samplesPerChannel > 0, "samples_per_channel must be explicit");
    source.retention = number(node["retention"], 0, 1, 100000, "retention");
    require(source.retention > 0, "retention must be explicit");
    source.startId = text(node["start_id"], "start_id"); streamId(source.startId);
    const auto stamp = node["timestamp"];
    keys(stamp, {"mode", "field", "valid_field"}, "timestamp");
    source.timestampMode = text(stamp["mode"], "timestamp.mode");
    require(source.timestampMode == "absent" || source.timestampMode == "unix-nanoseconds", "unsupported timestamp layout");
    if (source.timestampMode == "absent") require(!stamp["field"] && !stamp["valid_field"], "absent timestamp mode cannot name metadata fields");
    else {
      source.timestampField = text(stamp["field"], "timestamp.field", 128);
      source.timestampValidField = text(stamp["valid_field"], "timestamp.valid_field", 128);
      require(source.timestampField != source.timestampValidField && source.timestampField != source.payloadField && source.timestampValidField != source.payloadField,
              "payload and timestamp fields must be distinct");
    }
    const auto channels = node["channels"];
    require(channels.IsSequence() && channels.size() > 0 && channels.size() <= maxChannels, "channels requires 1 to 64 ordered entries");
    std::set<unsigned> physical;
    for (const auto& item : channels) {
      keys(item, {"channel", "raw", "volts", "eslo", "eoff"}, "channel");
      Channel channel;
      channel.channel = number(item["channel"], 0, 1, 65535, "channel.channel");
      require(channel.channel && physical.insert(channel.channel).second, "channel numbers must be explicit and unique per source");
      channel.raw = text(item["raw"], "channel.raw"); channel.volts = text(item["volts"], "channel.volts");
      require(allKeys.insert(channel.raw).second && allKeys.insert(channel.volts).second, "source/checkpoint/output keys must be globally disjoint");
      channel.eslo = calibration(item["eslo"], "eslo"); channel.eoff = calibration(item["eoff"], "eoff");
      source.channels.push_back(std::move(channel));
    }
    require(source.samplesPerChannel <= maxPayload / 2 / source.channels.size(), "configured complete pulse exceeds 1 MiB");
    source.fingerprint = fingerprint(source);
    config.sources.push_back(std::move(source));
  }
  return config;
}

Config loadConfig(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "cannot read demux config");
  std::string content;
  char buffer[8192];
  while (input) {
    input.read(buffer, sizeof buffer);
    const auto size = static_cast<size_t>(input.gcount());
    require(size <= maxConfig - content.size(), "demux config exceeds 1 MiB");
    content.append(buffer, size);
  }
  require(input.eof(), "cannot finish reading demux config");
  return parseConfig(content);
}

std::vector<Record> decode(const Source& source, const std::string& sourceId,
                           const std::string& payload, const std::string& timestamp,
                           const std::string& timestampValid, bool continuityUnknown) {
  streamId(sourceId);
  require(sourceId != "0-0", "source ID cannot be zero");
  require(!source.channels.empty() && source.channels.size() <= maxChannels && source.samplesPerChannel > 0 &&
          source.samplesPerChannel <= maxPayload / 2 / source.channels.size(), "invalid decoder bounds");
  inputRequire(payload.size() == source.samplesPerChannel * 2 * source.channels.size(), "payload size does not match configured complete pulse");
  bool valid = false;
  if (source.timestampMode == "absent") inputRequire(timestamp.empty() && timestampValid.empty(), "unexpected acquisition timestamp");
  else {
    require(source.timestampMode == "unix-nanoseconds", "unsupported timestamp layout");
    inputRequire(timestampValid.empty() || timestampValid == "0" || timestampValid == "1", "invalid acquisition time validity");
    if (!timestamp.empty()) {
      int64_t nanoseconds = 0;
      const auto parsed = std::from_chars(timestamp.data(), timestamp.data() + timestamp.size(), nanoseconds);
      inputRequire(parsed.ec == std::errc{} && parsed.ptr == timestamp.data() + timestamp.size(), "acquisition_time_ns must be an exact signed int64 decimal integer");
    }
    valid = timestampValid == "1";
    inputRequire(!valid || !timestamp.empty(), "valid acquisition time is missing timestamp");
  }
  std::vector<Record> result;
  result.reserve(source.channels.size() * 2);
  for (size_t channelIndex = 0; channelIndex < source.channels.size(); ++channelIndex) {
    const auto& channel = source.channels[channelIndex];
    std::string raw(source.samplesPerChannel * 2, '\0'), volts(source.samplesPerChannel * 4, '\0');
    for (size_t sample = 0; sample < source.samplesPerChannel; ++sample) {
      const auto offset = 2 * (sample * source.channels.size() + channelIndex);
      raw[2 * sample] = payload[offset]; raw[2 * sample + 1] = payload[offset + 1];
      const auto bits = uint32_t(static_cast<unsigned char>(payload[offset])) | (uint32_t(static_cast<unsigned char>(payload[offset + 1])) << 8);
      const int32_t count = bits >= 0x8000u ? int32_t(bits) - 65536 : int32_t(bits);
      const float calibrated = static_cast<float>(double(count) * double(channel.eslo) + double(channel.eoff));
      inputRequire(std::isfinite(calibrated), "calibrated float32 overflow");
      floatBytes(volts, sample * 4, calibrated);
    }
    std::map<std::string, std::string> fields{
      {"schema", "fermi-ad/xrm-demux:1"}, {"source_stream", source.stream}, {"source_id", sourceId},
      {"channel", std::to_string(channel.channel)}, {"acquisition_time_valid", valid ? "1" : "0"},
      {"source_continuity", continuityUnknown ? "unknown" : "observed"}};
    if (!timestamp.empty()) fields["acquisition_time_ns"] = timestamp;
    fields["_"] = std::move(raw); fields["format"] = "int16-le";
    result.push_back({channel.raw, fields});
    fields["_"] = std::move(volts); fields["format"] = "float32-le";
    result.push_back({channel.volts, std::move(fields)});
  }
  return result;
}

struct Worker::Impl {
  Config config;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::thread thread;
  std::atomic<bool> stopping{false};
  Status status;
  explicit Impl(Config value) : config(std::move(value)) {}
  void wait(unsigned ms) {
    std::unique_lock<std::mutex> lock(mutex);
    wake.wait_for(lock, std::chrono::milliseconds(ms), [&] { return stopping.load(); });
  }
  void update(const std::string& state, const std::string& error = {}) {
    std::lock_guard<std::mutex> lock(mutex);
    status.state = state; status.error = error.substr(0, 512);
  }
  Context connect() {
    const timeval connection{static_cast<time_t>(config.connectTimeoutMs / 1000), static_cast<suseconds_t>((config.connectTimeoutMs % 1000) * 1000)};
    Context context(redisConnectWithTimeout(config.host.c_str(), config.port, connection), &redisFree);
    require(context && !context->err, "Redis connection failed");
    const int flags = ::fcntl(context->fd, F_GETFL, 0);
    require(flags >= 0 && ::fcntl(context->fd, F_SETFL, flags | O_NONBLOCK) == 0, "cannot set nonblocking Redis socket");
    context->flags &= ~REDIS_BLOCK;
    if (config.db) command(context.get(), {"SELECT", std::to_string(config.db)}, config.commandTimeoutMs, stopping);
    return context;
  }
  bool poll(redisContext* context, const Source& source) {
    auto reply = command(context, {"EVAL", readScript, "2", source.stream, source.checkpoint,
      source.fingerprint, source.startId, source.payloadField,
      std::to_string(source.samplesPerChannel * 2 * source.channels.size()), source.timestampField, source.timestampValidField}, config.commandTimeoutMs, stopping);
    require(reply->type == REDIS_REPLY_ARRAY && reply->elements >= 2, "invalid demux read reply");
    const auto kind = str(reply->element[0]);
    if (kind == "empty") return false;
    if (stopping) return false;
    require((kind == "entry" && reply->elements == 7) || (kind == "reject" && reply->elements == 5), "invalid demux read record");
    const auto previous = str(reply->element[1]), id = str(reply->element[2]);
    streamId(previous); streamId(id);
    const bool gap = str(reply->element[kind == "entry" ? 6 : 4]) == "1";
    std::string error;
    std::vector<Record> records;
    if (kind == "reject") error = str(reply->element[3]);
    else {
      try { records = decode(source, id, str(reply->element[3]), str(reply->element[4]), str(reply->element[5]), gap); }
      catch (const InputError& failure) { error = failure.what(); }
    }
    std::vector<std::string> args{"EVAL", commitScript, std::to_string(records.size() + 1), source.checkpoint};
    for (const auto& record : records) args.push_back(record.key);
    args.insert(args.end(), {source.fingerprint, previous, id, std::to_string(source.retention), error, gap ? "1" : "0", std::to_string(records.size())});
    for (const auto& record : records) {
      args.push_back(std::to_string(record.fields.size() * 2));
      for (const auto& field : record.fields) { args.push_back(field.first); args.push_back(field.second); }
    }
    reply = command(context, args, config.commandTimeoutMs, stopping);
    const auto outcome = str(reply.get());
    require(outcome == "committed" || outcome == "stale", "invalid demux commit reply");
    if (outcome == "committed") {
      std::lock_guard<std::mutex> lock(mutex);
      if (error.empty()) ++status.processed; else ++status.rejected;
      if (gap) ++status.gaps;
      status.state = !error.empty() ? "rejected-input" : gap ? "continuity-unknown" : "ready";
      status.error = !error.empty() ? source.stream + ": " + error : gap ? source.stream + ": checkpoint entry is absent; source continuity cannot be established" : "";
    }
    return true;
  }
  void run() noexcept {
    Context context(nullptr, &redisFree);
    while (!stopping) {
      bool work = false, failed = false;
      try {
        if (!context) { update("connecting"); context = connect(); update("ready"); }
        for (const auto& source : config.sources) {
          if (stopping) break;
          try { work |= poll(context.get(), source); }
          catch (const std::exception& error) {
            { std::lock_guard<std::mutex> lock(mutex); ++status.failures; }
            update("retrying", source.stream + ": " + error.what());
            failed = true;
            if (context->err) break;
          }
        }
      } catch (const std::exception& error) {
        { std::lock_guard<std::mutex> lock(mutex); ++status.failures; }
        update("retrying", error.what()); failed = true;
      } catch (...) { update("retrying", "unknown demux worker failure"); failed = true; }
      if (failed) { context.reset(); wait(config.retryMs); }
      else if (!work) wait(config.pollIntervalMs);
    }
    std::lock_guard<std::mutex> lock(mutex);
    status.running = false; status.state = "stopped";
  }
};

Worker::Worker(Config config) : impl_(new Impl(std::move(config))) {
  require(!impl_->config.sources.empty(), "demux worker requires validated sources");
}
Worker::~Worker() { stop(); }
void Worker::start() {
  std::lock_guard<std::mutex> lock(impl_->mutex);
  require(!impl_->thread.joinable(), "demux worker already started");
  impl_->stopping = false; impl_->status.running = true; impl_->status.state = "starting";
  try { impl_->thread = std::thread([this] { impl_->run(); }); }
  catch (...) { impl_->status.running = false; impl_->status.state = "stopped"; throw; }
}
void Worker::stop() noexcept {
  impl_->stopping = true; impl_->wake.notify_all();
  if (impl_->thread.joinable()) impl_->thread.join();
}
Status Worker::status() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->status; }

} // namespace xrm_demux
