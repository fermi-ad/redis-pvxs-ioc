#include "xrm_demux.h"
#include "xrm_event.h"

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
#include <unistd.h>

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
void inputRequire(bool yes, const char* message) {
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
  // Keep existing plain-int16/checkpoint bindings byte-for-byte compatible.
  if (source.format != "int16-le") part(source.format);
  if (source.inputRetention) { part("owned-input-retention"); part(std::to_string(source.inputRetention)); }
  uint64_t hash = UINT64_C(14695981039346656037);
  for (unsigned char ch : data.str()) { hash ^= ch; hash *= UINT64_C(1099511628211); }
  std::ostringstream value;
  value << "xrm-demux-1-" << std::hex << std::setw(16) << std::setfill('0') << hash;
  return value.str();
}
std::string fingerprint(const CurrentConversion& source) {
  Source binding = source;
  binding.channels.push_back({1, source.output, source.setting, source.bootstrap, 0.0f});
  binding.channels.push_back({2, source.applied, "current-conversion-float32-v1", 0.0f, 0.0f});
  return fingerprint(binding);
}
size_t inputBytes(const Source& source) {
  require(!source.channels.empty() && source.channels.size() <= maxChannels && source.samplesPerChannel > 0,
          "invalid decoder bounds");
  require(source.format == "int16-le" || source.format == "acq4xx-es-int16-le", "unsupported raw format");
  const bool event = source.format == "acq4xx-es-int16-le";
  if (event) require(source.channels.size() == 16, "ES profile requires exactly 16 channels");
  const auto maximum = maxPayload / 2 / source.channels.size();
  require(source.samplesPerChannel <= maximum - (event ? 1 : 0), "configured complete pulse exceeds 1 MiB");
  return (source.samplesPerChannel + (event ? 1 : 0)) * 2 * source.channels.size();
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
// Conversion leases live in their already-owned checkpoint hashes. Every
// mutation is fenced; expiry permits recovery after a crash without deleting
// any key. Server time is used only for the lease, never acquisition metadata.
const std::string leaseScript = R"LUA(
local function nowms()
  local t = redis.call('TIME')
  return tonumber(t[1])*1000 + math.floor(tonumber(t[2])/1000)
end
local function fence(k,token,duration,claim)
  if token == '' then return end
  local now = nowms()
  local owner = redis.call('HGET',k,'writer_token')
  local expiry = tonumber(redis.call('HGET',k,'writer_until_ms') or '0')
  if owner ~= token and (not claim or expiry > now) then error('current conversion writer lease is held by another worker') end
  if not claim and expiry <= now then error('current conversion writer lease expired') end
  redis.call('HSET',k,'writer_token',token,'writer_until_ms',string.format('%.0f',now+tonumber(duration)))
end
)LUA";
const std::string readScript = leaseScript + R"LUA(
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
if ARGV[7] and ARGV[7] ~= '' then fence(KEYS[2],ARGV[7],ARGV[8],false) end
local cursor = redis.call('HGET', KEYS[2], 'last_id')
if not cursor or string.len(cursor) > 41 or not string.match(cursor, '^%d+%-%d+$') then return redis.error_reply('invalid checkpoint cursor') end
local function response(kind,id,payload,stamp,valid,gap,reason)
  local result
  if kind == 'reject' then result = {'reject',cursor,id,reason,gap}
  else result = {'entry',cursor,id,payload,stamp,valid,gap} end
  if ARGV[7] and ARGV[7] ~= '' then
    table.insert(result,redis.call('HGET',KEYS[2],'pending_gain') or '')
    table.insert(result,redis.call('HGET',KEYS[2],'pending_gain_command_id') or '')
    table.insert(result,redis.call('HGET',KEYS[2],'pending_metadata') or '{}')
  end
  return result
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
local metadata = {}
local function retain(kind,payload,stamp,valid,reason)
  if ARGV[7] and ARGV[7] ~= '' then
    local gain = redis.call('HGET',KEYS[2],'gain_value')
    local gainid = redis.call('HGET',KEYS[2],'gain_command_id')
    if not gain or string.len(gain) ~= 4 or not gainid then return redis.error_reply('validated current gain is unavailable') end
    redis.call('HSET',KEYS[2],'pending_gain',gain,'pending_gain_command_id',gainid,'pending_metadata',cjson.encode(metadata))
  end
  redis.call('HSET',KEYS[2],'pending_id',id,'pending_kind',kind,'pending_payload',payload,
    'pending_timestamp',stamp,'pending_valid',valid,'pending_gap',gap,'pending_reason',reason)
  return response(kind,id,payload,stamp,valid,gap,reason)
end
local function reject(reason) return retain('reject','','','',reason) end
local conversion = ARGV[7] and ARGV[7] ~= ''
if #fields > (conversion and 32 or 6) or #fields % 2 ~= 0 then return reject('unsupported field count') end
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
  elseif conversion and (k == 'schema' or k == 'format' or k == 'channel' or k == 'source_stream' or k == 'source_id' or k == 'source_continuity' or k == 'event_counter_0' or k == 'event_counter_1' or k == 'acquisition_time_valid') then
    if string.len(v) > 512 then return reject('producer metadata exceeds bound') end
    if k == 'acquisition_time_valid' and v ~= '0' then return reject('valid acquisition time requires configured timestamp fields') end
    metadata[k] = v
  else return reject('unsupported producer field or mixed layout') end
end
if not payload then return reject('payload field missing') end
if next(metadata) ~= nil and (metadata.schema ~= 'fermi-ad/xrm-demux:1' or metadata.format ~= 'float32-le') then return reject('unsupported current input metadata schema') end
return retain('entry',payload,stamp,valid,'')
)LUA";

// Preflight ALL types, IDs, checkpoint, and existing same-ID records before
// the first write. Identical already-written records are skipped on retry:
// Lua errors/OOM do not roll back earlier writes, and a lost reply may hide a
// successful commit. Compare-and-set on the cursor also permits two workers
// to race safely without duplicating fanout.
const std::string commitScript = leaseScript + R"LUA(
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
fence(KEYS[1],ARGV[8],ARGV[9],false)
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
local id, count, pos = ARGV[3], tonumber(ARGV[7]), 10
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
  'pending_gain','','pending_gain_command_id','','pending_metadata','',
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

// Return at most one bounded command. An outstanding acknowledgement is
// retried before newer commands, including after a lost reply or Redis error.
const std::string gainReadScript = leaseScript + R"LUA(
local function typ(k) return redis.call('TYPE',k).ok end
local ct,st,rt = typ(KEYS[1]),typ(KEYS[2]),typ(KEYS[3])
if ct ~= 'none' and ct ~= 'hash' then return redis.error_reply('checkpoint must be a hash') end
if st ~= 'none' and st ~= 'stream' then return redis.error_reply('gain setting must be a stream') end
if rt ~= 'none' and rt ~= 'stream' then return redis.error_reply('applied gain must be a stream') end
local cfg = redis.call('HGET',KEYS[1],'config')
if cfg and cfg ~= ARGV[1] then return redis.error_reply('checkpoint configuration mismatch') end
if not cfg then
  if ct ~= 'none' then return redis.error_reply('checkpoint is not owned by demux') end
  redis.call('HSET',KEYS[1],'config',ARGV[1],'source_stream',ARGV[2],'last_id',ARGV[3],
    'processed',0,'rejected',0,'gaps',0,'gain_setting',KEYS[2],'gain_applied',KEYS[3])
end
if redis.call('HGET',KEYS[1],'source_stream') ~= ARGV[2] or redis.call('HGET',KEYS[1],'gain_setting') ~= KEYS[2] or redis.call('HGET',KEYS[1],'gain_applied') ~= KEYS[3] then return redis.error_reply('checkpoint gain/source binding mismatch') end
fence(KEYS[1],ARGV[4],ARGV[5],true)
if st == 'none' and ARGV[9] == '1' then redis.call('XADD',KEYS[2],'MAXLEN','=',ARGV[7],ARGV[8],'_',ARGV[6]) end
if ARGV[9] ~= '1' then return {'idle',redis.call('HGET',KEYS[1],'gain_value') or '',redis.call('HGET',KEYS[1],'gain_command_id') or '',redis.call('HGET',KEYS[1],'gain_command_cursor') or ''} end
local pending = redis.call('HGET',KEYS[1],'gain_pending_id')
if pending and pending ~= '' then
  local p = redis.call('HMGET',KEYS[1],'gain_pending_value','gain_pending_error')
  if not p[1] or not p[2] or string.len(p[1]) > 4 or string.len(p[2]) > 256 then return redis.error_reply('invalid pending gain command') end
  return {'command',pending,p[1],p[2]}
end
local cursor = redis.call('HGET',KEYS[1],'gain_command_cursor')
local rows
if cursor then rows = redis.call('XRANGE',KEYS[2],'(' .. cursor,'+','COUNT',1)
else rows = redis.call('XREVRANGE',KEYS[2],'+','-','COUNT',1) end
if #rows == 0 then return {'idle',redis.call('HGET',KEYS[1],'gain_value') or '',redis.call('HGET',KEYS[1],'gain_command_id') or '',redis.call('HGET',KEYS[1],'gain_command_cursor') or ''} end
local id,fields,value,reason = rows[1][1],rows[1][2],'',''
if #fields ~= 2 or fields[1] ~= '_' then reason = 'gain command requires exactly the binary float32 _ field'
elseif string.len(fields[2]) ~= 4 then reason = 'gain command must contain exactly four float32 bytes'
else value = fields[2] end
redis.call('HSET',KEYS[1],'gain_pending_id',id,'gain_pending_value',value,'gain_pending_error',reason)
return {'command',id,value,reason}
)LUA";

// A new channel joins the already accepted global gain without replaying a
// setting or acknowledging it again. Validate every group fence before copying
// state; a pending waveform's saved gain and cursor are deliberately untouched.
const std::string gainHydrateScript = leaseScript + R"LUA(
local setting = redis.call('HGET',KEYS[1],'gain_setting')
local applied = redis.call('HGET',KEYS[1],'gain_applied')
if not setting or not applied or redis.call('HGET',KEYS[1],'gain_value') ~= ARGV[3] or redis.call('HGET',KEYS[1],'gain_command_id') ~= ARGV[4] or (redis.call('HGET',KEYS[1],'gain_command_cursor') or '') ~= ARGV[5] then return redis.error_reply('coordinated gain changed during hydration') end
for _,k in ipairs(KEYS) do
  if redis.call('TYPE',k).ok ~= 'hash' or redis.call('HGET',k,'gain_setting') ~= setting or redis.call('HGET',k,'gain_applied') ~= applied then return redis.error_reply('shared gain checkpoint is not initialized') end
  fence(k,ARGV[1],ARGV[2],false)
end
local cursor = ARGV[5]
for i=2,#KEYS do
  local gain = redis.call('HGET',KEYS[i],'gain_value')
  if not gain or gain == '' then
    redis.call('HSET',KEYS[i],'gain_value',ARGV[3],'gain_command_id',ARGV[4])
    if cursor ~= '' then
      redis.call('HSET',KEYS[i],'gain_command_cursor',cursor)
      -- A fresh coordinator may already have queued the latest S before the
      -- donor establishes that it was processed (possibly rejected) earlier.
      if redis.call('HGET',KEYS[i],'gain_pending_id') == cursor then
        redis.call('HSET',KEYS[i],'gain_pending_id','','gain_pending_value','','gain_pending_error','','gain_pending_ack_id','')
      end
    end
    for _,field in ipairs({'gain_last_rejected_id','gain_last_error'}) do
      local value = redis.call('HGET',KEYS[1],field)
      if value then redis.call('HSET',KEYS[i],field,value) end
    end
  end
end
return 'hydrated'
)LUA";

// KEYS[4..] are the other conversions sharing this global gain. All worker
// states are validated and updated before the one readback acknowledgement.
// An immutable pending ack ID makes retries idempotent after partial Lua work.
const std::string gainApplyScript = leaseScript + R"LUA(
local function typ(k) return redis.call('TYPE',k).ok end
local function legacyTime(id,message)
  local m,n = string.match(id,'^(%d+)%-(%d+)$')
  if not m or #id > 41 or (#m > 1 and string.sub(m,1,1) == '0') or (#n > 1 and string.sub(n,1,1) == '0') then error(message) end
  local ms,ns = tonumber(m),tonumber(n)
  if not ms or not ns or ns > 999999 or ms > 9223372036853 or (ms == 0 and ns == 0) then error(message) end
  return ms,ns
end
if typ(KEYS[1]) ~= 'hash' or redis.call('HGET',KEYS[1],'config') ~= ARGV[1] then return redis.error_reply('checkpoint configuration mismatch') end
if typ(KEYS[3]) ~= 'none' and typ(KEYS[3]) ~= 'stream' then return redis.error_reply('applied gain must be a stream') end
local checkpoints = {KEYS[1]}
for i=4,#KEYS do table.insert(checkpoints,KEYS[i]) end
for _,k in ipairs(checkpoints) do
  if typ(k) ~= 'hash' or redis.call('HGET',k,'gain_setting') ~= KEYS[2] or redis.call('HGET',k,'gain_applied') ~= KEYS[3] then return redis.error_reply('shared gain checkpoint is not initialized') end
  fence(k,ARGV[2],ARGV[3],false)
end
if redis.call('HGET',KEYS[1],'gain_pending_id') ~= ARGV[4] then return 'stale' end
if ARGV[6] ~= '' then
  redis.call('HSET',KEYS[1],'gain_command_cursor',ARGV[4],'gain_last_rejected_id',ARGV[4],
    'gain_last_error',ARGV[6],'gain_pending_id','','gain_pending_value','','gain_pending_error','')
  return 'rejected'
end
if string.len(ARGV[5]) ~= 4 then return redis.error_reply('validated gain has invalid size') end
local ackid = redis.call('HGET',KEYS[1],'gain_pending_ack_id')
if not ackid or ackid == '' then
  local last = redis.call('XREVRANGE',KEYS[3],'+','-','COUNT',1)
  -- A replacement coordinator may have no checkpoint state while S/R retain
  -- this exact accepted command. Reuse its acknowledgement identity instead
  -- of appending history for a command that is already confirmed.
  if #last ~= 0 and #last[1][2] == 4 then
    local f,values = last[1][2],{}
    for i=1,#f,2 do values[f[i]]=f[i+1] end
    if values['_'] == ARGV[5] and values['gain_command_id'] == ARGV[4] then
      ackid = last[1][1]
    end
  end
  if not ackid or ackid == '' then
  local ms,ns = legacyTime(ARGV[7],'invalid legacy gain acknowledgement time')
  if #last ~= 0 then
    local pm,pn = legacyTime(last[1][1],'applied gain has invalid legacy time')
    if pm > ms or (pm == ms and pn >= ns) then ms,ns = pm,pn+1 end
    if ns == 1000000 then ms,ns = ms+1,0 end
  end
  local cm,cn = legacyTime(ARGV[4],'gain command has invalid legacy time')
  if cm > ms or (cm == ms and cn >= ns) then ms,ns = cm,cn+1 end
  if ns == 1000000 then ms,ns = ms+1,0 end
  ackid = string.format('%.0f-%.0f',ms,ns)
  end
end
legacyTime(ackid,'gain acknowledgement has invalid legacy time')
redis.call('HSET',KEYS[1],'gain_pending_ack_id',ackid)
local existing = redis.call('XRANGE',KEYS[3],ackid,ackid,'COUNT',1)
if #existing ~= 0 then
  local f = existing[1][2]
  if #f ~= 4 then return redis.error_reply('gain acknowledgement collision') end
  local v = {}; for i=1,#f,2 do v[f[i]]=f[i+1] end
  if v['_'] ~= ARGV[5] or v['gain_command_id'] ~= ARGV[4] then return redis.error_reply('gain acknowledgement collision') end
end
for _,k in ipairs(checkpoints) do redis.call('HSET',k,'gain_value',ARGV[5],'gain_command_id',ARGV[4]) end
if #existing == 0 then redis.call('XADD',KEYS[3],'MAXLEN','=',ARGV[8],ackid,'_',ARGV[5],'gain_command_id',ARGV[4]) end
for _,k in ipairs(checkpoints) do
  redis.call('HSET',k,'gain_command_cursor',ARGV[4],'gain_last_error','',
    'gain_pending_id','','gain_pending_value','','gain_pending_error','','gain_pending_ack_id','')
end
return 'applied'
)LUA";

const std::string releaseScript = R"LUA(
for _,k in ipairs(KEYS) do
  if redis.call('TYPE',k).ok == 'hash' and redis.call('HGET',k,'writer_token') == ARGV[1] then redis.call('HSET',k,'writer_until_ms','0') end
end
return 'released'
)LUA";

void floatBytes(std::string& bytes, size_t offset, float value) {
  static_assert(sizeof(float) == 4 && std::numeric_limits<float>::is_iec559, "IEEE754 float32 required");
  uint32_t bits;
  std::memcpy(&bits, &value, sizeof bits);
  for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<char>((bits >> (8 * i)) & 0xffu);
}
float readFloat(const std::string& bytes, size_t offset = 0) {
  uint32_t bits = 0;
  for (unsigned i=0;i<4;++i) bits |= uint32_t(static_cast<unsigned char>(bytes.at(offset+i))) << (8*i);
  float value; std::memcpy(&value,&bits,sizeof value); return value;
}
std::string legacyNowId() {
  const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  require(ns > 0, "wall clock is unavailable for gain acknowledgement");
  return std::to_string(ns/1000000) + "-" + std::to_string(ns%1000000);
}
} // namespace

Config parseConfig(const std::string& yaml) {
  require(yaml.size() <= maxConfig, "demux config exceeds 1 MiB");
  const auto docs = YAML::LoadAll(yaml);
  require(docs.size() == 1, "demux config requires exactly one YAML document");
  const auto root = docs.front();
  keys(root, {"schema_version", "redis", "sources", "current_conversions"}, "demux");
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
  require(!sources || (sources.IsSequence() && sources.size() <= maxSources), "sources requires at most 32 entries");
  std::set<std::string> allKeys;
  std::set<std::string> rawVolts, currentInputs;
  for (const auto& node : sources) {
    keys(node, {"stream", "checkpoint", "payload_field", "format", "layout", "samples_per_channel", "start_id", "retention", "input_retention", "timestamp", "channels"}, "source");
    Source source;
    source.stream = text(node["stream"], "source.stream");
    source.checkpoint = text(node["checkpoint"], "source.checkpoint");
    require(allKeys.insert(source.stream).second && allKeys.insert(source.checkpoint).second, "source/checkpoint/output keys must be globally disjoint");
    if (node["payload_field"]) source.payloadField = text(node["payload_field"], "payload_field", 128);
    source.format = text(node["format"], "format");
    require(source.format == "int16-le" || source.format == "acq4xx-es-int16-le",
            "only int16-le or the qualified acq4xx-es-int16-le profile is supported");
    require(text(node["layout"], "layout") == "sample-major", "only aligned sample-major layout is supported");
    source.samplesPerChannel = number(node["samples_per_channel"], 0, 1, maxPayload / 2, "samples_per_channel");
    require(source.samplesPerChannel > 0, "samples_per_channel must be explicit");
    source.retention = number(node["retention"], 0, 1, 100000, "retention");
    require(source.retention > 0, "retention must be explicit");
    if (node["input_retention"]) source.inputRetention = number(node["input_retention"], 0, 1, 100000, "input_retention");
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
      rawVolts.insert(channel.volts);
      source.channels.push_back(std::move(channel));
    }
    (void)inputBytes(source);
    source.fingerprint = fingerprint(source);
    config.sources.push_back(std::move(source));
  }
  const auto conversions = root["current_conversions"];
  require(!conversions || (conversions.IsSequence() && conversions.size() <= maxSources), "current_conversions requires at most 32 entries");
  std::map<std::string,std::pair<std::string,float>> settings;
  for (const auto& node : conversions) {
    keys(node,{"stream","output","checkpoint","payload_field","samples_per_pulse","start_id","retention","timestamp","gain"},"current_conversion");
    CurrentConversion source;
    source.stream = text(node["stream"],"current_conversion.stream");
    source.output = text(node["output"],"current_conversion.output");
    source.checkpoint = text(node["checkpoint"],"current_conversion.checkpoint");
    require(currentInputs.insert(source.stream).second && (rawVolts.count(source.stream) || allKeys.insert(source.stream).second) && allKeys.insert(source.output).second && allKeys.insert(source.checkpoint).second,
            "source/checkpoint/output/setting keys must be globally disjoint");
    if (node["payload_field"]) source.payloadField = text(node["payload_field"],"payload_field",128);
    source.samplesPerChannel = number(node["samples_per_pulse"],0,1,16384,"samples_per_pulse");
    require(source.samplesPerChannel > 0,"samples_per_pulse must be explicit");
    source.retention = number(node["retention"],0,1,100000,"retention");
    require(source.retention > 0,"retention must be explicit");
    source.startId = text(node["start_id"],"start_id"); streamId(source.startId);
    const auto stamp = node["timestamp"];
    keys(stamp,{"mode","field","valid_field"},"timestamp");
    source.timestampMode = text(stamp["mode"],"timestamp.mode");
    require(source.timestampMode == "absent" || source.timestampMode == "unix-nanoseconds","unsupported timestamp layout");
    if (source.timestampMode == "absent") require(!stamp["field"] && !stamp["valid_field"],"absent timestamp mode cannot name metadata fields");
    else {
      source.timestampField = text(stamp["field"],"timestamp.field",128);
      source.timestampValidField = text(stamp["valid_field"],"timestamp.valid_field",128);
      require(source.timestampField != source.timestampValidField && source.timestampField != source.payloadField && source.timestampValidField != source.payloadField,
              "payload and timestamp fields must be distinct");
    }
    const auto gain = node["gain"];
    keys(gain,{"setting","applied","bootstrap"},"gain");
    source.setting = text(gain["setting"],"gain.setting");
    source.applied = text(gain["applied"],"gain.applied");
    source.bootstrap = calibration(gain["bootstrap"],"gain.bootstrap");
    const auto prior = settings.find(source.setting);
    if (prior == settings.end()) {
      require(allKeys.insert(source.setting).second && allKeys.insert(source.applied).second,
              "source/checkpoint/output/setting keys must be globally disjoint");
      settings.emplace(source.setting,std::make_pair(source.applied,source.bootstrap));
    } else require(prior->second.first == source.applied && prior->second.second == source.bootstrap,
                   "shared gain settings require identical applied key and bootstrap");
    source.fingerprint = fingerprint(source);
    config.currentConversions.push_back(std::move(source));
  }
  require(!config.sources.empty() || !config.currentConversions.empty(),"demux requires sources or current_conversions");
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
  inputRequire(payload.size() == inputBytes(source), "payload size does not match configured complete pulse");
  const bool eventFormat = source.format == "acq4xx-es-int16-le";
  EventPulse event;
  if (eventFormat) {
    try { event = decodeEventPulse(payload, source.channels.size(), source.samplesPerChannel); }
    catch (const std::invalid_argument& error) { throw InputError(error.what()); }
  }
  const auto& adc = eventFormat ? event.adcBytes : payload;
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
      raw[2 * sample] = adc[offset]; raw[2 * sample + 1] = adc[offset + 1];
      const auto bits = uint32_t(static_cast<unsigned char>(adc[offset])) | (uint32_t(static_cast<unsigned char>(adc[offset + 1])) << 8);
      const int32_t count = bits >= 0x8000u ? int32_t(bits) - 65536 : int32_t(bits);
      float calibrated;
      if (eventFormat) {
        // Match the reference NumPy float32 multiply then float32 add. The
        // intermediate store also prevents contraction into a single FMA.
        volatile float scaled = static_cast<float>(count) * channel.eslo;
        calibrated = scaled + channel.eoff;
      } else calibrated = static_cast<float>(double(count) * double(channel.eslo) + double(channel.eoff));
      inputRequire(std::isfinite(calibrated), "calibrated float32 overflow");
      floatBytes(volts, sample * 4, calibrated);
    }
    std::map<std::string, std::string> fields{
      {"schema", "fermi-ad/xrm-demux:1"}, {"source_stream", source.stream}, {"source_id", sourceId},
      {"channel", std::to_string(channel.channel)}, {"acquisition_time_valid", valid ? "1" : "0"},
      {"source_continuity", continuityUnknown ? "unknown" : "observed"}};
    if (!timestamp.empty()) fields["acquisition_time_ns"] = timestamp;
    if (eventFormat) {
      fields["event_counter_0"] = std::to_string(event.sampleCounter);
      fields["event_counter_1"] = std::to_string(event.sampleClockCounter);
    }
    fields["_"] = std::move(raw); fields["format"] = "int16-le";
    result.push_back({channel.raw, fields});
    fields["_"] = std::move(volts); fields["format"] = "float32-le";
    result.push_back({channel.volts, std::move(fields)});
  }
  return result;
}

Record convertCurrent(const CurrentConversion& source, const std::string& sourceId,
                      const std::string& payload, const std::string& timestamp,
                      const std::string& timestampValid, float gain,
                      const std::string& gainCommandId, bool continuityUnknown,
                      const std::map<std::string,std::string>& provenance) {
  streamId(sourceId); streamId(gainCommandId);
  require(sourceId != "0-0" && gainCommandId != "0-0","source and gain command IDs cannot be zero");
  require(source.samplesPerChannel > 0 && source.samplesPerChannel <= 16384,"invalid current converter bounds");
  inputRequire(payload.size() == source.samplesPerChannel*4,"payload size does not match configured complete pulse");
  require(std::isfinite(gain),"gain must be finite float32");
  bool valid = false;
  if (source.timestampMode == "absent") inputRequire(timestamp.empty() && timestampValid.empty(),"unexpected acquisition timestamp");
  else {
    require(source.timestampMode == "unix-nanoseconds","unsupported timestamp layout");
    inputRequire(timestampValid.empty() || timestampValid == "0" || timestampValid == "1","invalid acquisition time validity");
    if (!timestamp.empty()) {
      int64_t ns = 0; const auto parsed = std::from_chars(timestamp.data(),timestamp.data()+timestamp.size(),ns);
      inputRequire(parsed.ec == std::errc{} && parsed.ptr == timestamp.data()+timestamp.size(),"acquisition_time_ns must be an exact signed int64 decimal integer");
    }
    valid = timestampValid == "1";
    inputRequire(!valid || !timestamp.empty(),"valid acquisition time is missing timestamp");
  }
  std::string current(payload.size(),'\0'), appliedGain(4,'\0');
  floatBytes(appliedGain,0,gain);
  for (size_t sample=0;sample<source.samplesPerChannel;++sample) {
    const auto volts = readFloat(payload,sample*4);
    // One binary32 product matches NumPy's float32 volts * float32 slope.
    const float value = volts*gain;
    inputRequire(std::isfinite(volts) && std::isfinite(value),"current conversion contains nonfinite float32");
    floatBytes(current,sample*4,value);
  }
  Record record{source.output,{{"_",std::move(current)},{"schema","fermi-ad/xrm-current:1"},{"format","float32-le"},
    {"source_stream",source.stream},{"source_id",sourceId},{"gain",appliedGain},{"gain_command_id",gainCommandId},
    {"acquisition_time_valid",valid ? "1" : "0"},{"source_continuity",continuityUnknown ? "unknown" : "observed"}}};
  if (!timestamp.empty()) record.fields["acquisition_time_ns"] = timestamp;
  for (const auto& field : provenance) {
    if (field.first == "source_stream" || field.first == "source_id") record.fields["upstream_"+field.first] = field.second;
    else if (field.first == "event_counter_0" || field.first == "event_counter_1") {
      uint32_t value = 0; const auto& text = field.second;
      const auto parsed = std::from_chars(text.data(),text.data()+text.size(),value);
      inputRequire(!text.empty() && parsed.ec == std::errc{} && parsed.ptr == text.data()+text.size(),"event counter must be an exact unsigned32 decimal integer");
      record.fields[field.first] = text;
    }
    else if (field.first == "channel") record.fields[field.first] = field.second;
    else if (field.first == "source_continuity" && field.second == "unknown") record.fields["source_continuity"] = "unknown";
  }
  return record;
}

struct Worker::Impl {
  Config config;
  mutable std::mutex mutex;
  std::condition_variable wake;
  std::thread thread;
  std::atomic<bool> stopping{false};
  Status status;
  std::string writerToken;
  unsigned leaseMs = 0;
  explicit Impl(Config value) : config(std::move(value)) {
    // Conversion channels are independent. A shared acquisition order avoids
    // opposite YAML order retaining incompatible prefixes of writer leases.
    // Never sort raw source channels: their order is the ADC wire contract.
    std::sort(config.currentConversions.begin(),config.currentConversions.end(),
      [](const CurrentConversion& a, const CurrentConversion& b) { return a.checkpoint < b.checkpoint; });
    static std::atomic<uint64_t> serial{0};
    writerToken = std::to_string(::getpid()) + ":" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ":" + std::to_string(++serial);
    leaseMs = std::max(5000u,config.pollIntervalMs + config.retryMs + 4*config.commandTimeoutMs + 1000);
  }
  void wait(unsigned ms) {
    std::unique_lock<std::mutex> lock(mutex);
    wake.wait_for(lock, std::chrono::milliseconds(ms), [&] { return stopping.load(); });
  }
  void update(const std::string& state, const std::string& error = {}) {
    std::lock_guard<std::mutex> lock(mutex);
    status.state = state; status.error = error.substr(0, 512);
  }
  Context connect(bool releasing = false) {
    const auto timeoutMs = releasing ? std::min(config.connectTimeoutMs,100u) : config.connectTimeoutMs;
    const timeval connection{static_cast<time_t>(timeoutMs / 1000), static_cast<suseconds_t>((timeoutMs % 1000) * 1000)};
    Context context(redisConnectWithTimeout(config.host.c_str(), config.port, connection), &redisFree);
    require(context && !context->err, "Redis connection failed");
    const int flags = ::fcntl(context->fd, F_GETFL, 0);
    require(flags >= 0 && ::fcntl(context->fd, F_SETFL, flags | O_NONBLOCK) == 0, "cannot set nonblocking Redis socket");
    context->flags &= ~REDIS_BLOCK;
    if (config.db) {
      const std::atomic<bool> releaseStopping{false};
      command(context.get(), {"SELECT", std::to_string(config.db)},releasing ? 50 : config.commandTimeoutMs,releasing ? releaseStopping : stopping);
    }
    return context;
  }
  void releaseLeases(Context& context) {
    if (config.currentConversions.empty()) return;
    if (!context || context->err) context = connect(true);
    std::vector<std::string> args{"EVAL",releaseScript,std::to_string(config.currentConversions.size())};
    for (const auto& source : config.currentConversions) args.push_back(source.checkpoint);
    args.push_back(writerToken);
    const std::atomic<bool> releaseStopping{false};
    command(context.get(),args,50,releaseStopping);
  }
  bool commit(redisContext* context, const Source& source, const std::string& previous, const std::string& id,
              const std::vector<Record>& records, const std::string& error, bool gap, bool current = false) {
    std::vector<std::string> args{"EVAL", commitScript, std::to_string(records.size() + 1), source.checkpoint};
    for (const auto& record : records) args.push_back(record.key);
    args.insert(args.end(), {source.fingerprint, previous, id, std::to_string(source.retention), error, gap ? "1" : "0",
      std::to_string(records.size()),current ? writerToken : "",std::to_string(leaseMs)});
    for (const auto& record : records) {
      args.push_back(std::to_string(record.fields.size() * 2));
      for (const auto& field : record.fields) { args.push_back(field.first); args.push_back(field.second); }
    }
    const auto reply = command(context, args, config.commandTimeoutMs, stopping);
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
  bool poll(redisContext* context, const Source& source) {
    auto reply = command(context, {"EVAL", readScript, "2", source.stream, source.checkpoint,
      source.fingerprint, source.startId, source.payloadField,
      std::to_string(inputBytes(source)), source.timestampField, source.timestampValidField}, config.commandTimeoutMs, stopping);
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
    const bool committed = commit(context,source,previous,id,records,error,gap);
    if (source.inputRetention) {
      // This is an explicit ownership grant for the one configured input key.
      // It never trims by pattern, settings, another device, or a checkpoint.
      command(context, {"XTRIM", source.stream, "MAXLEN", "=", std::to_string(source.inputRetention)},
              config.commandTimeoutMs, stopping);
    }
    return committed;
  }
  Reply readGain(redisContext* context, const CurrentConversion& source) {
    std::string bootstrap(4,'\0'); floatBytes(bootstrap,0,source.bootstrap);
    bool coordinator = true;
    for (const auto& other : config.currentConversions)
      if (other.setting == source.setting && other.checkpoint < source.checkpoint) coordinator = false;
    return command(context,{"EVAL",gainReadScript,"3",source.checkpoint,source.setting,source.applied,
      source.fingerprint,source.stream,source.startId,writerToken,std::to_string(leaseMs),bootstrap,
      std::to_string(source.retention),legacyNowId(),coordinator ? "1" : "0"},config.commandTimeoutMs,stopping);
  }
  void hydrateGain(redisContext* context, const CurrentConversion& source, const std::string& gain,
                   const std::string& id, const std::string& cursor) {
    require(gain.size() == 4 && std::isfinite(readFloat(gain)),"persisted gain is not finite float32");
    streamId(id); require(id != "0-0","persisted gain command ID cannot be zero");
    if (!cursor.empty()) { streamId(cursor); require(cursor != "0-0","persisted gain command cursor cannot be zero"); }
    std::vector<std::string> args{"EVAL",gainHydrateScript,"",source.checkpoint};
    for (const auto& other : config.currentConversions)
      if (other.checkpoint != source.checkpoint && other.setting == source.setting) args.push_back(other.checkpoint);
    args[2] = std::to_string(args.size()-3);
    args.insert(args.end(),{writerToken,std::to_string(leaseMs),gain,id,cursor});
    command(context,args,config.commandTimeoutMs,stopping);
  }
  bool refreshGain(redisContext* context, const CurrentConversion& source) {
    const auto reply = readGain(context,source);
    require(reply->type == REDIS_REPLY_ARRAY && reply->elements >= 3,"invalid current gain reply");
    const auto kind = str(reply->element[0]);
    if (kind == "idle") {
      const auto gain = str(reply->element[1]);
      require(gain.empty() || (gain.size() == 4 && std::isfinite(readFloat(gain))),"persisted gain is not finite float32");
      if (gain.empty()) return false;
      require(reply->elements == 4,"invalid persisted gain reply");
      hydrateGain(context,source,gain,str(reply->element[2]),str(reply->element[3]));
      return true;
    }
    require(kind == "command" && reply->elements == 4,"invalid gain command reply");
    const auto id = str(reply->element[1]), payload = str(reply->element[2]);
    streamId(id);
    auto error = str(reply->element[3]);
    if (error.empty() && (payload.size() != 4 || !std::isfinite(readFloat(payload)))) error = "gain command must be finite float32";
    std::vector<std::string> args{"EVAL",gainApplyScript,"",source.checkpoint,source.setting,source.applied};
    for (const auto& other : config.currentConversions)
      if (other.checkpoint != source.checkpoint && other.setting == source.setting) args.push_back(other.checkpoint);
    args[2] = std::to_string(args.size()-3);
    args.insert(args.end(),{source.fingerprint,writerToken,std::to_string(leaseMs),id,payload,error,legacyNowId(),std::to_string(source.retention)});
    const auto applied = command(context,args,config.commandTimeoutMs,stopping);
    const auto result = str(applied.get());
    require(result == "applied" || result == "rejected" || result == "stale","invalid gain application reply");
    if (result == "rejected") {
      update("rejected-gain",source.setting + ": " + error);
      return false;
    }
    return result == "applied";
  }
  bool pollCurrent(redisContext* context, const CurrentConversion& source) {
    auto reply = command(context,{"EVAL",readScript,"2",source.stream,source.checkpoint,source.fingerprint,
      source.startId,source.payloadField,std::to_string(source.samplesPerChannel*4),source.timestampField,
      source.timestampValidField,writerToken,std::to_string(leaseMs)},config.commandTimeoutMs,stopping);
    require(reply->type == REDIS_REPLY_ARRAY && reply->elements >= 2,"invalid current read reply");
    const auto kind = str(reply->element[0]);
    if (kind == "empty" || stopping) return false;
    require((kind == "entry" && reply->elements == 10) || (kind == "reject" && reply->elements == 8),"invalid current read record");
    const auto previous = str(reply->element[1]), id = str(reply->element[2]);
    streamId(previous); streamId(id);
    const bool gap = str(reply->element[kind == "entry" ? 6 : 4]) == "1";
    std::string error;
    std::vector<Record> records;
    if (kind == "reject") error = str(reply->element[3]);
    else {
      const auto gain = str(reply->element[7]);
      require(gain.size() == 4 && std::isfinite(readFloat(gain)),"pending gain is not finite float32");
      const auto metadata = str(reply->element[9]);
      require(metadata.size() <= 4096,"pending current metadata exceeds bound");
      std::map<std::string,std::string> provenance;
      const auto parsed = YAML::Load(metadata);
      require(parsed.IsMap(),"pending current metadata must be a mapping");
      for (const auto& field : parsed) provenance.emplace(field.first.as<std::string>(),field.second.as<std::string>());
      try { records.push_back(convertCurrent(source,id,str(reply->element[3]),str(reply->element[4]),str(reply->element[5]),readFloat(gain),str(reply->element[8]),gap,provenance)); }
      catch (const InputError& failure) { error = failure.what(); }
    }
    return commit(context,source,previous,id,records,error,gap,true);
  }
  void run() noexcept {
    Context context(nullptr, &redisFree);
    while (!stopping) {
      bool work = false, failed = false;
      try {
        if (!context) {
          update("connecting"); context = connect(); update("ready");
          // Establish every lease before applying a setting shared by several
          // channels. The following loop applies their worker states together.
          try {
            struct Donor { const CurrentConversion* source; std::string gain,id,cursor; };
            std::map<std::string,Donor> donors;
            for (const auto& source : config.currentConversions) {
              const auto reply = readGain(context.get(),source);
              if (reply->type == REDIS_REPLY_ARRAY && reply->elements == 4 && str(reply->element[0]) == "idle" && !str(reply->element[1]).empty())
                donors.emplace(source.setting,Donor{&source,str(reply->element[1]),str(reply->element[2]),str(reply->element[3])});
            }
            // Any persisted accepted member can initialize a fresh coordinator,
            // including when the latest command has already been rejected.
            for (const auto& item : donors) {
              const auto& donor = item.second;
              hydrateGain(context.get(),*donor.source,donor.gain,donor.id,donor.cursor);
            }
          }
          catch (...) {
            // A failed group acquisition must not renew an owned prefix forever
            // while another worker holds the rest. Release only our own token.
            try { releaseLeases(context); } catch (...) { /* finite expiry remains */ }
            throw;
          }
        }
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
        // Apply shared gains once at the round boundary, before retaining any
        // new channel waveform. A command arriving during this round waits for
        // the next boundary; an existing pending waveform keeps its saved gain.
        std::map<std::string,bool> gainsReady;
        for (const auto& source : config.currentConversions) {
          if (stopping || context->err) break;
          try {
            bool coordinator = true;
            for (const auto& other : config.currentConversions)
              if (other.setting == source.setting && other.checkpoint < source.checkpoint) coordinator = false;
            if (coordinator) gainsReady[source.setting] = refreshGain(context.get(),source);
            else readGain(context.get(),source);
          } catch (const std::exception& error) {
            { std::lock_guard<std::mutex> lock(mutex); ++status.failures; }
            update("retrying",source.setting + ": " + error.what()); failed = true;
          }
        }
        for (const auto& source : config.currentConversions) {
          if (stopping || context->err) break;
          if (!gainsReady[source.setting]) continue;
          try { work |= pollCurrent(context.get(),source); }
          catch (const std::exception& error) {
            { std::lock_guard<std::mutex> lock(mutex); ++status.failures; }
            update("retrying",source.stream + ": " + error.what()); failed = true;
          }
        }
      } catch (const std::exception& error) {
        { std::lock_guard<std::mutex> lock(mutex); ++status.failures; }
        update("retrying", error.what()); failed = true;
      } catch (...) { update("retrying", "unknown demux worker failure"); failed = true; }
      if (failed) { context.reset(); wait(config.retryMs); }
      else if (!work) wait(config.pollIntervalMs);
    }
    if (!config.currentConversions.empty()) {
      try {
        // A stop may interrupt an unanswered command and discard its socket.
        // Use a fresh, separately bounded connection to release our own lease;
        // otherwise an ordinary restart unnecessarily waits for crash expiry.
        releaseLeases(context);
      } catch (...) { /* The finite server-time lease also recovers a crash. */ }
    }
    std::lock_guard<std::mutex> lock(mutex);
    status.running = false; status.state = "stopped";
  }
};

Worker::Worker(Config config) : impl_(new Impl(std::move(config))) {
  require(!impl_->config.sources.empty() || !impl_->config.currentConversions.empty(), "demux worker requires validated sources or current conversions");
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
