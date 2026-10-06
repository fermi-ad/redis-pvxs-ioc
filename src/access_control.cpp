// ExecOp forwarding requires the complete Timer type from the pinned PVXS API.
#ifndef PVXS_ENABLE_EXPERT_API
#define PVXS_ENABLE_EXPERT_API
#endif
#include "redis_pvxs_ioc/access_control.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <stdexcept>
#include <unordered_map>
#include <utility>
#include <vector>

#include <asLib.h>
#include <errSymTbl.h>
#include <macLib.h>

#include <pvxs/data.h>
#include <pvxs/source.h>

namespace redis_pvxs_ioc {
namespace {

constexpr uint8_t kRead = 0x01u;
constexpr uint8_t kWrite = 0x02u;
constexpr uint8_t kTrapWrite = 0x04u;
constexpr size_t kMaxRawPolicyBytes = 1024u * 1024u;
constexpr size_t kMaxExpandedPolicyBytes = 4u * 1024u * 1024u;

std::string readTextFile(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  if (!input) {
    throw std::runtime_error("cannot open ACF file '" + path + "'");
  }
  input.seekg(0, std::ios::end);
  const auto length = input.tellg();
  if (length < 0) {
    throw std::runtime_error("cannot determine ACF file size for '" + path + "'");
  }
  if (static_cast<uint64_t>(length) > kMaxRawPolicyBytes) {
    throw std::runtime_error("ACF file exceeds the 1 MiB limit: '" + path + "'");
  }
  input.seekg(0, std::ios::beg);
  std::ostringstream stream;
  stream << input.rdbuf();
  if (!input.good() && !input.eof()) {
    throw std::runtime_error("cannot read ACF file '" + path + "'");
  }
  auto result = stream.str();
  if (result.find('\0') != std::string::npos) {
    throw std::runtime_error("ACF file contains a NUL byte: '" + path + "'");
  }
  return result;
}

std::string expandMacros(const std::string& input,
                         const std::map<std::string, std::string>& macros) {
  MAC_HANDLE* handle = nullptr;
  std::vector<const char*> pairs;
  pairs.reserve(macros.size() * 2u + 1u);
  for (const auto& entry : macros) {
    pairs.push_back(entry.first.c_str());
    pairs.push_back(entry.second.c_str());
  }
  pairs.push_back(nullptr);

  if (macCreateHandle(&handle, pairs.data()) != 0 || !handle) {
    throw std::runtime_error("failed to create ACF macro context");
  }

  struct HandleGuard {
    MAC_HANDLE* handle;
    ~HandleGuard() { macDeleteHandle(handle); }
  } guard{handle};

  size_t capacity = std::min(kMaxExpandedPolicyBytes + 1u,
                             std::max<size_t>(input.size() + 1024u, 4096u));
  while (capacity <= kMaxExpandedPolicyBytes + 1u) {
    std::vector<char> output(capacity, '\0');
    const long result = macExpandString(handle, input.c_str(), output.data(), static_cast<long>(output.size()));
    if (result < 0) {
      throw std::runtime_error("ACF contains an undefined macro");
    }
    if (static_cast<size_t>(result) + 1u < output.size()) {
      return std::string(output.data(), static_cast<size_t>(result));
    }
    if (capacity == kMaxExpandedPolicyBytes + 1u) break;
    capacity = std::min(kMaxExpandedPolicyBytes + 1u, capacity * 2u);
  }
  throw std::runtime_error("expanded ACF exceeds supported size");
}

std::string fingerprint(const std::string& text) {
  uint64_t hash = 1469598103934665603ull;
  for (const unsigned char ch : text) {
    hash ^= ch;
    hash *= 1099511628211ull;
  }
  std::ostringstream stream;
  stream << std::hex << std::setfill('0') << std::setw(16) << hash;
  return stream.str();
}

struct Token {
  std::string text;
  size_t line = 1;
  size_t column = 1;
  bool quoted = false;
};

std::vector<Token> tokenizeAcf(const std::string& text) {
  std::vector<Token> tokens;
  size_t line = 1;
  size_t column = 1;
  for (size_t index = 0; index < text.size();) {
    const char ch = text[index];
    if (std::isspace(static_cast<unsigned char>(ch))) {
      if (ch == '\n') {
        ++line;
        column = 1;
      } else {
        ++column;
      }
      ++index;
      continue;
    }
    if (ch == '#') {
      while (index < text.size() && text[index] != '\n') {
        ++index;
        ++column;
      }
      continue;
    }
    if (ch == '"') {
      const size_t startLine = line;
      const size_t startColumn = column;
      std::string value;
      ++index;
      ++column;
      bool escaped = false;
      bool closed = false;
      while (index < text.size()) {
        const char current = text[index++];
        ++column;
        if (current == '\n') {
          ++line;
          column = 1;
        }
        if (escaped) {
          value.push_back(current);
          escaped = false;
        } else if (current == '\\') {
          escaped = true;
        } else if (current == '"') {
          closed = true;
          break;
        } else {
          value.push_back(current);
        }
      }
      if (!closed) {
        std::ostringstream error;
        error << "ACF " << startLine << ":" << startColumn << ": unterminated string";
        throw std::runtime_error(error.str());
      }
      tokens.push_back({value, startLine, startColumn, true});
      continue;
    }
    if (ch != '(' && ch != ')' && ch != '{' && ch != '}' && ch != ',') {
      const size_t start = index;
      const size_t startColumn = column;
      while (index < text.size()) {
        const unsigned char current = static_cast<unsigned char>(text[index]);
        if (std::isspace(current) || current == '(' || current == ')' ||
            current == '{' || current == '}' || current == ',' ||
            current == '#' || current == '"') break;
        ++index;
        ++column;
      }
      tokens.push_back({text.substr(start, index - start), line, startColumn});
      continue;
    }
    if (ch == '(' || ch == ')' || ch == '{' || ch == '}' || ch == ',') {
      tokens.push_back({std::string(1, ch), line, column});
    }
    ++index;
    ++column;
  }
  return tokens;
}

std::string upper(std::string text) {
  std::transform(text.begin(), text.end(), text.begin(), [](const unsigned char ch) {
    return static_cast<char>(std::toupper(ch));
  });
  return text;
}

[[noreturn]] void acfError(const Token& token, const std::string& message) {
  std::ostringstream error;
  error << "ACF " << token.line << ":" << token.column << ": " << message;
  throw std::runtime_error(error.str());
}

class AcfSubsetParser {
public:
  explicit AcfSubsetParser(const std::string& text) : tokens_(tokenizeAcf(text)) {}

  std::set<std::string> parse() {
    std::set<std::string> groups;
    while (!done()) {
      rejectDeferred(peek());
      const auto keyword = upper(peek().text);
      if (keyword == "UAG" || keyword == "HAG") {
        parseNamedList(keyword);
      } else if (keyword == "ASG") {
        const auto group = parseAsg();
        if (!groups.insert(group).second) {
          acfError(tokens_[position_ - 1u], "duplicate ASG '" + group + "'");
        }
      } else {
        acfError(peek(), "unknown top-level construct '" + peek().text + "'");
      }
    }
    for (const auto& reference : references_) {
      const auto& definitions = reference.kind == "UAG" ? uags_ : hags_;
      if (definitions.count(reference.name) == 0u) {
        acfError(reference.token,
                 reference.kind + " references undefined group '" + reference.name + "'");
      }
    }
    groups.insert("DEFAULT");
    return groups;
  }

private:
  bool done() const { return position_ == tokens_.size(); }

  const Token& peek() const {
    if (done()) throw std::runtime_error("ACF: unexpected end of policy");
    return tokens_[position_];
  }

  Token take() {
    const auto result = peek();
    ++position_;
    return result;
  }

  void expect(const std::string& text) {
    if (peek().text != text) acfError(peek(), "expected '" + text + "'");
    ++position_;
  }

  Token name() {
    const auto result = take();
    if (result.text.empty() || (!result.quoted &&
        (result.text == "(" || result.text == ")" || result.text == "{" ||
         result.text == "}" || result.text == ","))) {
      acfError(result, "expected a name");
    }
    return result;
  }

  void rejectDeferred(const Token& token) const {
    if (token.quoted) return;
    const auto keyword = upper(token.text);
    if (keyword == "CALC" ||
        (keyword.size() == 4u && keyword.rfind("INP", 0u) == 0u &&
         keyword[3] >= 'A' && keyword[3] <= 'U')) {
      acfError(token, keyword + " is outside the supported ACF subset");
    }
  }

  std::vector<std::string> callNames(const std::string& keyword) {
    const auto actual = take();
    rejectDeferred(actual);
    if (actual.quoted || upper(actual.text) != keyword) {
      acfError(actual, "expected " + keyword);
    }
    expect("(");
    std::vector<std::string> result{name().text};
    while (peek().text == ",") {
      take();
      result.push_back(name().text);
    }
    expect(")");
    return result;
  }

  std::string definitionName(const std::string& keyword) {
    const auto names = callNames(keyword);
    if (names.size() != 1u) {
      acfError(tokens_[position_ - 1u], keyword + " definitions require exactly one name");
    }
    return names.front();
  }

  void parseNamedList(const std::string& keyword) {
    const auto definition = definitionName(keyword);
    auto& definitions = keyword == "UAG" ? uags_ : hags_;
    if (!definitions.insert(definition).second) {
      acfError(tokens_[position_ - 1u], "duplicate " + keyword + " '" + definition + "'");
    }
    expect("{");
    if (peek().text != "}") {
      while (true) {
        name();
        if (peek().text == "}") break;
        expect(",");
      }
    }
    expect("}");
  }

  std::string parseAsg() {
    const auto group = definitionName("ASG");
    expect("{");
    while (peek().text != "}") parseRule();
    expect("}");
    return group;
  }

  void parseRule() {
    const auto rule = take();
    rejectDeferred(rule);
    if (rule.quoted || upper(rule.text) != "RULE") {
      acfError(rule, "only RULE is supported inside ASG");
    }
    expect("(");
    const auto level = name();
    if (level.quoted || (level.text != "0" && level.text != "1")) {
      acfError(level, "RULE access level must be 0 or 1");
    }
    expect(",");
    const auto permission = name();
    const auto permissionName = permission.quoted ? std::string{} : upper(permission.text);
    if (permissionName != "NONE" && permissionName != "READ" && permissionName != "WRITE") {
      acfError(permission, "RULE permission must be NONE, READ, or WRITE");
    }
    if (peek().text == ",") {
      take();
      const auto trap = name();
      const auto trapName = trap.quoted ? std::string{} : upper(trap.text);
      if (trapName != "TRAPWRITE" && trapName != "NOTRAPWRITE") {
        acfError(trap, "RULE option must be TRAPWRITE or NOTRAPWRITE");
      }
    }
    expect(")");
    if (done() || peek().text != "{") return;
    take();
    while (peek().text != "}") {
      rejectDeferred(peek());
      const auto condition = upper(peek().text);
      if (condition != "UAG" && condition != "HAG") {
        acfError(peek(), "only UAG and HAG conditions are supported in RULE");
      }
      const auto token = peek();
      for (const auto& referenced : callNames(condition)) {
        references_.push_back({condition, referenced, token});
      }
    }
    expect("}");
  }

  std::vector<Token> tokens_;
  size_t position_ = 0u;
  std::set<std::string> uags_;
  std::set<std::string> hags_;
  struct Reference {
    std::string kind;
    std::string name;
    Token token;
  };
  std::vector<Reference> references_;
};

std::set<std::string> inspectAcf(const std::string& text) {
  return AcfSubsetParser(text).parse();
}

struct PreparedPolicy {
  std::string raw;
  std::string rawFingerprint;
  std::string expanded;
  std::string fingerprint;
  std::set<std::string> groups;
};

PreparedPolicy preparePolicy(const AccessConfig& config,
                             const std::set<std::string>& requiredAsgs) {
  PreparedPolicy prepared;
  prepared.raw = readTextFile(config.file);
  prepared.rawFingerprint = fingerprint(prepared.raw);
  prepared.expanded = expandMacros(prepared.raw, config.macros);
  prepared.groups = inspectAcf(prepared.expanded);
  for (const auto& asg : requiredAsgs) {
    if (prepared.groups.count(asg) == 0u) {
      throw std::runtime_error("ACF does not define required ASG '" + asg + "'");
    }
  }
  prepared.fingerprint = fingerprint(prepared.expanded);
  return prepared;
}

std::string clientHost(const std::string& peer) {
  std::string host = peer;
  if (!host.empty() && host.front() == '[') {
    const auto end = host.find(']');
    if (end != std::string::npos) host = host.substr(1u, end - 1u);
  } else {
    const auto colon = host.rfind(':');
    if (colon != std::string::npos && host.find(':') == colon) host.resize(colon);
  }
  const std::string mapped = "::ffff:";
  if (host.rfind(mapped, 0u) == 0u) host.erase(0u, mapped.size());
  return host;
}

std::vector<std::string> clientUsers(const pvxs::server::ClientCredentials& credentials) {
  std::vector<std::string> users;
  if (credentials.method == "ca") {
    const auto slash = credentials.account.find_last_of('/');
    users.push_back(slash == std::string::npos ? credentials.account : credentials.account.substr(slash + 1u));
  } else {
    users.push_back(credentials.method + "/" + credentials.account);
  }
  for (const auto& role : credentials.roles()) users.push_back("role/" + role);
  return users;
}

class BoundedStreamBuffer final : public std::streambuf {
public:
  explicit BoundedStreamBuffer(const size_t limit) : limit_(limit) { output_.reserve(limit); }

  std::string result() const { return output_ + (truncated_ ? "..." : ""); }

protected:
  std::streamsize xsputn(const char* data, const std::streamsize count) override {
    const auto available = limit_ - output_.size();
    const auto copied = std::min<size_t>(available, static_cast<size_t>(count));
    output_.append(data, copied);
    if (copied != static_cast<size_t>(count)) truncated_ = true;
    return count;
  }

  int overflow(const int ch) override {
    if (ch == traits_type::eof()) return traits_type::not_eof(ch);
    if (output_.size() < limit_) output_.push_back(static_cast<char>(ch));
    else truncated_ = true;
    return ch;
  }

private:
  size_t limit_;
  std::string output_;
  bool truncated_ = false;
};

std::string valuePreview(const pvxs::Value& value) {
  BoundedStreamBuffer buffer(256u);
  std::ostream stream(&buffer);
  stream << value.format().arrayLimit(8u);
  auto result = buffer.result();
  std::replace(result.begin(), result.end(), '\n', ' ');
  std::replace(result.begin(), result.end(), '\r', ' ');
  return result;
}

std::string auditTimestamp() {
  const auto now = std::chrono::system_clock::now();
  const auto seconds = std::chrono::system_clock::to_time_t(now);
  std::tm utc{};
#ifdef _WIN32
  gmtime_s(&utc, &seconds);
#else
  gmtime_r(&seconds, &utc);
#endif
  const auto millis = std::chrono::duration_cast<std::chrono::milliseconds>(
      now.time_since_epoch()).count() % 1000;
  std::ostringstream stream;
  stream << std::put_time(&utc, "%Y-%m-%dT%H:%M:%S") << '.'
         << std::setfill('0') << std::setw(3) << millis << 'Z';
  return stream.str();
}

std::string auditField(const std::string& input) {
  auto result = input.substr(0, 256);
  for (auto& ch : result) if (static_cast<unsigned char>(ch) < 32 || ch == 127 || ch == '"' || ch == '\\') ch = '?';
  if (input.size() > 256) result += "...";
  return result;
}

class AccessMember;
class ChannelState;

struct CloseHandle {
  std::mutex mutex;
  std::weak_ptr<pvxs::server::ChannelControl> channel;

  void close() {
    std::shared_ptr<pvxs::server::ChannelControl> current;
    {
      std::lock_guard<std::mutex> guard(mutex);
      current = channel.lock();
    }
    if (current) current->close();
  }
};

}  // namespace

struct AccessController::Impl : public std::enable_shared_from_this<AccessController::Impl> {
  explicit Impl(AccessConfig initial)
      : config(std::move(initial)), registry(std::make_shared<PVRegistry>()) {}

  AccessConfig config;
  mutable std::mutex mutex;
  std::mutex reloadMutex;
  std::shared_ptr<PVRegistry> registry;
  std::unordered_map<std::string, std::shared_ptr<AccessMember>> members;
  std::vector<std::weak_ptr<ChannelState>> dirty;
  std::atomic<bool> recomputeNeeded{false};
  std::atomic<uint64_t> generation{0};
  std::atomic<uint64_t> activeClients{0};
  std::atomic<uint64_t> deniedReads{0};
  std::atomic<uint64_t> deniedWrites{0};
  std::atomic<uint64_t> rightsChanges{0};
  std::atomic<uint64_t> authorizedOperations{0}, operationsInFlight{0};
  std::atomic<uint64_t> operationsSucceeded{0}, operationsFailed{0};
  std::atomic<uint64_t> operationsCancelled{0}, operationsAbandoned{0}, operationsDenied{0};
  std::atomic<uint64_t> denialLogsSuppressed{0};
  std::string lastStatus = "initializing";
  std::string lastError;
  std::string policyFingerprint;
  std::string activePolicy;
  AccessConfig previousConfig;
  std::set<std::string> previousConfiguredAsgs;
  std::string previousPolicy;
  std::string previousPolicyFingerprint;
  std::string previousRaw;
  uint64_t previousGeneration = 0u;
  bool hasPreviousPolicy = false;
  std::string watchStatus = "disabled";
  std::string pendingTrigger;
  std::chrono::steady_clock::time_point lastMaintenance{};
  std::chrono::steady_clock::time_point lastWatchPoll{};
  std::chrono::steady_clock::time_point watchCandidateSince{};
  std::string observedRaw;
  std::string watchCandidateRaw;
  std::string watchLastAttemptRaw;
  bool watchMissingReported = false;
  std::set<std::string> configuredAsgs;

  bool reloadLocked(const std::string& trigger,
                    const AccessConfig& configSnapshot,
                    const std::set<std::string>& requiredAsgs,
                    std::string& error);
  void markDirty(const std::shared_ptr<ChannelState>& state);
  void drainDirty();
  void recomputeAllClients();
  // id is the admitted operation's audit id for a dispatch-time denial; zero
  // for requests refused at admission, which were never assigned one.
  void recordDenied(const ChannelState& state, bool write, const pvxs::Value* value,
                    const char* operation = "put", uint64_t id = 0);
  void recordAudit(const ChannelState& state, uint64_t id, const char* operation,
                   const char* phase, const char* result, const pvxs::Value* value = nullptr) noexcept;
};

namespace {

class AccessMember {
public:
  explicit AccessMember(AccessAssignment value) : assignment(std::move(value)) {
    if (asAddMember(&member, assignment.asg.c_str()) != 0 || !member) {
      throw std::runtime_error("failed to add access member for ASG '" + assignment.asg + "'");
    }
  }

  ~AccessMember() {
    if (member) {
      const auto status = asRemoveMember(&member);
      if (status != 0) {
        std::fprintf(stderr, "[redis-pvxs-ioc] access member cleanup failed for ASG %s: %s\n",
                     assignment.asg.c_str(), errSymMsg(status));
      }
    }
  }

  void addClient(const std::shared_ptr<ChannelState>& state) {
    std::lock_guard<std::mutex> guard(mutex);
    clients.erase(std::remove_if(clients.begin(), clients.end(),
                                 [](const auto& prior) { return prior.expired(); }), clients.end());
    clients.emplace_back(state);
  }

  void closeClients();
  std::vector<std::shared_ptr<ChannelState>> liveClients();

  AccessAssignment assignment;
  ASMEMBERPVT member = nullptr;
  std::mutex mutex;
  std::vector<std::weak_ptr<ChannelState>> clients;
};

class ChannelState : public std::enable_shared_from_this<ChannelState> {
public:
  ChannelState(AccessController::Impl& owner,
               std::shared_ptr<AccessMember> accessMember,
               std::string channelName,
               const pvxs::server::ClientCredentials& credentials)
      : lifetime(owner.shared_from_this()), owner(owner), member(std::move(accessMember)), name(std::move(channelName)), peer(credentials.peer),
        host(clientHost(credentials.peer)), method(credentials.method), account(credentials.account),
        users(clientUsers(credentials)), closer(std::make_shared<CloseHandle>()) {}

  ~ChannelState() {
    for (auto& client : clients) {
      if (client) asRemoveClient(&client);
    }
    if (counted) owner.activeClients.fetch_sub(1u, std::memory_order_relaxed);
  }

  void initialize() {
    clients.resize(users.size(), nullptr);
    for (size_t index = 0; index < users.size(); ++index) {
      if (asAddClient(&clients[index], member->member, member->assignment.asl,
                      users[index].c_str(), host.data()) != 0 || !clients[index]) {
        throw std::runtime_error("failed to add access client");
      }
    }
    for (auto client : clients) {
      asPutClientPvt(client, this);
      if (asRegisterClientCallback(client, &ChannelState::accessChanged) != 0) {
        throw std::runtime_error("failed to register access callback");
      }
    }
    recompute(false);
    initialized.store(true, std::memory_order_release);
    member->addClient(shared_from_this());
    owner.activeClients.fetch_add(1u, std::memory_order_relaxed);
    counted = true;
  }

  uint8_t loadRights() const { return rights.load(std::memory_order_acquire); }

  void recompute(bool closeOnChange) {
    uint8_t next = 0u;
    for (auto client : clients) {
      if (asCheckGet(client)) next |= kRead;
      if (asCheckPut(client)) {
        next |= kWrite;
        const auto* raw = static_cast<const ASGCLIENT*>(client);
        if (raw->trapMask) next |= kTrapWrite;
      }
    }
    const auto current = rights.exchange(next, std::memory_order_acq_rel);
    dirty.store(false, std::memory_order_release);
    const auto callbackPrior = rightsBeforeChange.exchange(0u, std::memory_order_acq_rel);
    const auto prior = callbackPrior != 0u ? callbackPrior : current;
    if (closeOnChange && prior != next) {
      owner.rightsChanges.fetch_add(1u, std::memory_order_relaxed);
      closer->close();
    }
  }

  static void accessChanged(ASCLIENTPVT client, asClientStatus status) {
    if (status != asClientCOAR) return;
    auto* self = static_cast<ChannelState*>(asGetClientPvt(client));
    if (!self) return;
    const auto prior = self->rights.exchange(0u, std::memory_order_acq_rel);
    self->rightsBeforeChange.fetch_or(prior, std::memory_order_acq_rel);
    if (!self->initialized.load(std::memory_order_acquire)) return;
    if (!self->dirty.exchange(true, std::memory_order_acq_rel)) {
      try {
        self->owner.markDirty(self->shared_from_this());
      } catch (...) {
        // Never unwind a C access-library callback after policy activation.
        self->owner.recomputeNeeded = true;
      }
    }
  }

  std::shared_ptr<AccessController::Impl> lifetime;
  AccessController::Impl& owner;
  std::shared_ptr<AccessMember> member;
  std::string name;
  std::string peer;
  std::string host;
  std::string method;
  std::string account;
  std::vector<std::string> users;
  std::vector<ASCLIENTPVT> clients;
  std::shared_ptr<CloseHandle> closer;
  std::atomic<uint8_t> rights{0u};
  // Fixed-size throttling state expires with this live channel. Never retain
  // historical PV/peer keys (a new TCP port used to grow that map indefinitely).
  mutable std::atomic<int64_t> lastDeniedReadMs{0}, lastDeniedWriteMs{0};
  std::atomic<uint8_t> rightsBeforeChange{0u};
  std::atomic<bool> dirty{false};
  std::atomic<bool> initialized{false};
  bool counted = false;
};

void AccessMember::closeClients() {
  const auto live = liveClients();
  for (const auto& state : live) state->closer->close();
}

std::vector<std::shared_ptr<ChannelState>> AccessMember::liveClients() {
  std::vector<std::shared_ptr<ChannelState>> live;
  {
    std::lock_guard<std::mutex> guard(mutex);
    for (auto it = clients.begin(); it != clients.end();) {
      if (auto state = it->lock()) {
        live.emplace_back(std::move(state));
        ++it;
      } else {
        it = clients.erase(it);
      }
    }
  }
  return live;
}

struct AccessOperation {
  std::shared_ptr<ChannelState> state;
  const char* operation;
  const bool trap;
  uint64_t id;
  std::atomic<bool> done{false};
  AccessOperation(std::shared_ptr<ChannelState> channel, const char* kind, bool audited,
                   const pvxs::Value& value)
      : state(std::move(channel)), operation(kind), trap(audited), id(++state->owner.authorizedOperations) {
    ++state->owner.operationsInFlight;
    if (trap) state->owner.recordAudit(*state, id, operation, "authorization", "allowed",
                                      std::strcmp(operation, "rpc") == 0 ? nullptr : &value);
  }
  ~AccessOperation() { finish("abandoned"); }
  void finish(const char* outcome) noexcept {
    if (done.exchange(true)) return;
    auto& owner = state->owner;
    --owner.operationsInFlight;
    if (std::strcmp(outcome, "success") == 0) ++owner.operationsSucceeded;
    else if (std::strcmp(outcome, "error") == 0) ++owner.operationsFailed;
    else if (std::strcmp(outcome, "cancelled") == 0) ++owner.operationsCancelled;
    else if (std::strcmp(outcome, "denied") == 0) ++owner.operationsDenied;
    else ++owner.operationsAbandoned;
    if (trap) owner.recordAudit(*state, id, operation, "completion", outcome);
  }
};

class AuthorizedExecOp final : public pvxs::server::ExecOp, public OperationAuthorization {
public:
  AuthorizedExecOp(std::unique_ptr<pvxs::server::ExecOp> target, std::shared_ptr<ChannelState> state,
                   const char* operation, bool trap, const pvxs::Value& value)
      : ExecOp(target->name(), target->credentials(), target->op(), target->pvRequest()),
        target_(std::move(target)), state_(std::move(state)),
        audit_(std::make_shared<AccessOperation>(state_, operation, trap, value)) {
    auto audit = audit_;
    target_->onCancel([audit] { audit->finish("cancelled"); });
  }
  ~AuthorizedExecOp() override { audit_->finish("abandoned"); }
  void reply() override { target_->reply(); audit_->finish("success"); }
  void reply(const pvxs::Value& value) override { target_->reply(value); audit_->finish("success"); }
  void error(const std::string& message) override { target_->error(message); audit_->finish("error"); }
  void logRemote(pvxs::Level level, const std::string& message) override { target_->logRemote(level, message); }
  void onCancel(std::function<void()>&& fn) override {
    auto audit = audit_;
    target_->onCancel([audit, fn = std::move(fn)] { audit->finish("cancelled"); if (fn) fn(); });
  }
  bool authorized(const pvxs::Value& value) override {
    if ((state_->loadRights() & kWrite) != 0u) return authorizeWriteDispatch(*target_, value);
    // Rights changed after admission. Record the refusal under the admitted
    // audit id so it is not mistaken for a later backend error, then close the
    // operation as denied; the caller's PVA error reply cannot reclassify it.
    state_->owner.recordDenied(*state_, true, std::strcmp(audit_->operation, "rpc") == 0 ? nullptr : &value,
                               audit_->operation, audit_->trap ? audit_->id : 0);
    audit_->finish("denied");
    return false;
  }
private:
  pvxs::Timer _timerOneShot(double delay, std::function<void()>&& fn) override {
#ifdef PVXS_EXPERT_API_ENABLED
    return target_->timerOneShot(delay, std::move(fn));
#else
    (void)delay; (void)fn;
    throw std::logic_error("PVXS expert timer API is unavailable");
#endif
  }
  std::unique_ptr<pvxs::server::ExecOp> target_;
  std::shared_ptr<ChannelState> state_;
  std::shared_ptr<AccessOperation> audit_;
};

class AuthorizedConnectOp final : public pvxs::server::ConnectOp {
public:
  AuthorizedConnectOp(std::unique_ptr<pvxs::server::ConnectOp> target,
                      std::shared_ptr<ChannelState> state)
      : ConnectOp(target->name(), target->credentials(), target->op(), target->pvRequest()),
        target_(std::move(target)), state_(std::move(state)) {}

  void connect(const pvxs::Value& prototype) override {
    // GET_FIELD is a one-shot ConnectOp and never invokes onGet().  Enforce
    // READ here for that operation while leaving GET/PUT setup alone so their
    // execution paths retain the single cached-rights load.
    if (op() == pvxs::server::OpBase::Info && (state_->loadRights() & kRead) == 0u) {
      state_->owner.recordDenied(*state_, false, nullptr);
      target_->error("access denied");
      return;
    }
    target_->connect(prototype);
  }

  void error(const std::string& message) override { target_->error(message); }
  void logRemote(pvxs::Level level, const std::string& message) override { target_->logRemote(level, message); }
  void onClose(std::function<void(const std::string&)>&& fn) override { target_->onClose(std::move(fn)); }

  void onGet(std::function<void(std::unique_ptr<pvxs::server::ExecOp>&&)>&& fn) override {
    auto state = state_;
    target_->onGet([state, fn = std::move(fn)](std::unique_ptr<pvxs::server::ExecOp>&& op) mutable {
      if ((state->loadRights() & kRead) == 0u) {
        state->owner.recordDenied(*state, false, nullptr);
        op->error("access denied");
        return;
      }
      fn(std::move(op));
    });
  }

  void onPut(std::function<void(std::unique_ptr<pvxs::server::ExecOp>&&, pvxs::Value&&)>&& fn) override {
    auto state = state_;
    target_->onPut([state, fn = std::move(fn)](std::unique_ptr<pvxs::server::ExecOp>&& op,
                                               pvxs::Value&& value) mutable {
      const auto rights = state->loadRights();
      if ((rights & kWrite) == 0u) {
        state->owner.recordDenied(*state, true, &value);
        op->error("access denied");
        return;
      }
      auto secured = std::make_unique<AuthorizedExecOp>(std::move(op), state, "put", (rights & kTrapWrite) != 0u, value);
      fn(std::move(secured), std::move(value));
    });
  }

private:
  std::unique_ptr<pvxs::server::ConnectOp> target_;
  std::shared_ptr<ChannelState> state_;
};

class AuthorizedChannelControl final : public pvxs::server::ChannelControl {
public:
  AuthorizedChannelControl(std::unique_ptr<pvxs::server::ChannelControl> target,
                           std::shared_ptr<ChannelState> state)
      : ChannelControl(target->name(), target->credentials(), target->op()),
        target_(std::move(target)), state_(std::move(state)) {
    std::lock_guard<std::mutex> guard(state_->closer->mutex);
    state_->closer->channel = target_;
  }

  ~AuthorizedChannelControl() override {
    std::lock_guard<std::mutex> guard(state_->closer->mutex);
    state_->closer->channel.reset();
  }

  void onOp(std::function<void(std::unique_ptr<pvxs::server::ConnectOp>&&)>&& fn) override {
    auto state = state_;
    target_->onOp([state, fn = std::move(fn)](std::unique_ptr<pvxs::server::ConnectOp>&& op) mutable {
      fn(std::make_unique<AuthorizedConnectOp>(std::move(op), state));
    });
  }

  void onRPC(std::function<void(std::unique_ptr<pvxs::server::ExecOp>&&, pvxs::Value&&)>&& fn) override {
    auto state = state_;
    target_->onRPC([state, fn = std::move(fn)](std::unique_ptr<pvxs::server::ExecOp>&& op,
                                               pvxs::Value&& value) mutable {
      const auto rights = state->loadRights();
      if ((rights & kWrite) == 0u) {
        state->owner.recordDenied(*state, true, nullptr, "rpc");
        op->error("access denied");
        return;
      }
      auto secured = std::make_unique<AuthorizedExecOp>(std::move(op), state, "rpc", (rights & kTrapWrite) != 0u, value);
      fn(std::move(secured), std::move(value));
    });
  }

  void onSubscribe(std::function<void(std::unique_ptr<pvxs::server::MonitorSetupOp>&&)>&& fn) override {
    auto state = state_;
    target_->onSubscribe([state, fn = std::move(fn)](std::unique_ptr<pvxs::server::MonitorSetupOp>&& op) mutable {
      if ((state->loadRights() & kRead) == 0u) {
        state->owner.recordDenied(*state, false, nullptr);
        op->error("access denied");
        return;
      }
      fn(std::move(op));
    });
  }

  void onClose(std::function<void(const std::string&)>&& fn) override { target_->onClose(std::move(fn)); }
  void close() override { target_->close(); }

private:
  void _updateInfo(const std::shared_ptr<const pvxs::server::ReportInfo>& info) override {
#ifdef PVXS_EXPERT_API_ENABLED
    target_->updateInfo(info);
#else
    (void)info;
#endif
  }

  std::shared_ptr<pvxs::server::ChannelControl> target_;
  std::shared_ptr<ChannelState> state_;
};


}  // namespace

bool authorizeWriteDispatch(pvxs::server::ExecOp& operation, const pvxs::Value& value) {
  auto* authorized = dynamic_cast<OperationAuthorization*>(&operation);
  return !authorized || authorized->authorized(value);
}

void AccessController::Impl::markDirty(const std::shared_ptr<ChannelState>& state) {
  std::lock_guard<std::mutex> guard(mutex);
  dirty.emplace_back(state);
}

void AccessController::Impl::drainDirty() {
  if (recomputeNeeded.exchange(false)) { recomputeAllClients(); return; }
  std::vector<std::weak_ptr<ChannelState>> pending;
  {
    std::lock_guard<std::mutex> guard(mutex);
    pending.swap(dirty);
  }
  for (const auto& weak : pending) {
    if (const auto state = weak.lock()) state->recompute(true);
  }
}

void AccessController::Impl::recomputeAllClients() {
  std::vector<std::shared_ptr<AccessMember>> currentMembers;
  {
    std::lock_guard<std::mutex> guard(mutex);
    currentMembers.reserve(members.size());
    for (const auto& entry : members) currentMembers.push_back(entry.second);
    dirty.clear();
  }
  for (const auto& member : currentMembers) {
    for (const auto& state : member->liveClients()) state->recompute(true);
  }
}

void AccessController::Impl::recordDenied(const ChannelState& state,
                                          const bool write,
                                          const pvxs::Value* value,
                                          const char* operation,
                                          const uint64_t id) {
  (write ? deniedWrites : deniedReads).fetch_add(1u, std::memory_order_relaxed);
  if (write) recordAudit(state, id, operation, "authorization", "denied", value);
  const auto now = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now().time_since_epoch()).count() + 1;
  auto& stamp = write ? state.lastDeniedWriteMs : state.lastDeniedReadMs;
  auto prior = stamp.load(std::memory_order_relaxed);
  const bool emit = (prior == 0 || now - prior >= 5000) &&
                    stamp.compare_exchange_strong(prior, now, std::memory_order_relaxed);
  if (!emit) { ++denialLogsSuppressed; return; }
  const auto preview = value ? auditField(valuePreview(*value)) : std::string{};
  std::fprintf(stderr,
               "[redis-pvxs-ioc] access denied phase=authorization operation=%s pv=\"%s\" result=denied asg=\"%s\" asl=%d user=\"%s\" peer=\"%s\" auth=\"%s\"%s%s\n",
               write ? operation : "read", auditField(state.name).c_str(), auditField(state.member->assignment.asg).c_str(),
               state.member->assignment.asl,
               auditField(state.account).c_str(), auditField(state.peer).c_str(), auditField(state.method).c_str(),
               value ? " value=" : "", value ? preview.c_str() : "");
}

void AccessController::Impl::recordAudit(const ChannelState& state, uint64_t id, const char* operation,
    const char* phase, const char* result, const pvxs::Value* value) noexcept {
  try {
    const auto preview = value ? auditField(valuePreview(*value)) : "<omitted>";
    const auto timestamp = auditTimestamp();
    std::fprintf(stderr,
        "[redis-pvxs-ioc] access audit timestamp=%s id=%llu phase=%s operation=%s pv=\"%s\" result=%s "
        "asg=\"%s\" asl=%d user=\"%s\" peer=\"%s\" auth=\"%s\" value=\"%s\"\n",
        timestamp.c_str(), static_cast<unsigned long long>(id), phase, operation,
        auditField(state.name).c_str(), result, auditField(state.member->assignment.asg).c_str(),
        state.member->assignment.asl, auditField(state.account).c_str(), auditField(state.peer).c_str(),
        auditField(state.method).c_str(), preview.c_str());
  } catch (...) { /* logging allocation failures must not change authorization/completion */ }
}

AccessController::AccessController(AccessConfig config)
    : impl_(std::make_shared<Impl>(std::move(config))) {}

AccessController::~AccessController() {
  try { clearBindings(); }
  catch (const std::exception& ex) {
    std::fprintf(stderr, "[redis-pvxs-ioc] access endpoint cleanup failed: %s\n", ex.what());
  }
}

struct AccessController::PreparedConfiguration {
  std::shared_ptr<AccessController::Impl> owner;
  std::unique_lock<std::mutex> serialized;
  AccessConfig config;
  std::set<std::string> requiredAsgs;
  PreparedPolicy policy;
  std::string status = "config reload active";
  std::string watchStatus;
  bool policyChanged = false;
  explicit PreparedConfiguration(AccessController::Impl& impl)
      : owner(impl.shared_from_this()), serialized(impl.reloadMutex) {}
};

std::shared_ptr<AccessController::PreparedConfiguration> AccessController::prepareConfiguration(
    const AccessConfig& config, const std::set<std::string>& requiredAsgs) {
  auto result = std::make_shared<PreparedConfiguration>(*impl_);
  result->config = config;
  result->requiredAsgs = requiredAsgs;
  result->policy = preparePolicy(config, requiredAsgs);
  result->watchStatus = config.watch.enabled ? "watching " + config.file : "disabled";
  std::lock_guard<std::mutex> guard(impl_->mutex);
  if (config.enabled != impl_->config.enabled)
    throw std::runtime_error("access.enabled is immutable after startup");
  result->policyChanged = result->policy.expanded != impl_->activePolicy;
  return result;
}

bool AccessController::activateConfiguration(const std::shared_ptr<PreparedConfiguration>& prepared,
                                             std::string& error) {
  if (!prepared || prepared->owner.get() != impl_.get() || !prepared->serialized.owns_lock()) {
    error = "invalid prepared access configuration";
    return false;
  }
  if (prepared->policyChanged) {
    const auto status = asInitMem(prepared->policy.expanded.c_str(), nullptr);
    if (status != 0) {
      error = std::string("ACF parse failed: ") + errSymMsg(status);
      return false;
    }
  }
  // Everything after the library's successful activation is a prepared swap.
  // Client revocation is deferred until the complete endpoint set is published.
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    std::swap(impl_->config, prepared->config);
    impl_->configuredAsgs.swap(prepared->requiredAsgs);
    impl_->activePolicy.swap(prepared->policy.expanded);
    impl_->policyFingerprint.swap(prepared->policy.fingerprint);
    impl_->observedRaw.swap(prepared->policy.raw);
    impl_->lastStatus.swap(prepared->status);
    impl_->watchStatus.swap(prepared->watchStatus);
    impl_->lastError.clear();
    impl_->watchLastAttemptRaw.clear();
    impl_->watchCandidateRaw.clear();
    impl_->watchMissingReported = false;
    impl_->hasPreviousPolicy = false;
  }
  ++impl_->generation;
  error.clear();
  return true;
}

void AccessController::finishConfiguration() { impl_->recomputeAllClients(); }

bool AccessController::start(const std::set<std::string>& requiredAsgs, std::string& error) {
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->config.enabled) {
      error = "access controller requires enabled configuration";
      return false;
    }
    impl_->configuredAsgs = requiredAsgs;
  }
  asCheckClientIP = 1;
  return reload("startup", error);
}

bool AccessController::reconfigure(const AccessConfig& config,
                                   const std::set<std::string>& requiredAsgs,
                                   std::string& error) {
  std::lock_guard<std::mutex> reloadGuard(impl_->reloadMutex);
  AccessConfig previous;
  std::set<std::string> previousAsgs;
  std::string previousPolicy;
  std::string previousPolicyFingerprint;
  std::string previousRaw;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (config.enabled != impl_->config.enabled) {
      error = "access.enabled is immutable after startup";
      return false;
    }
    previous = impl_->config;
    previousAsgs = impl_->configuredAsgs;
    previousPolicy = impl_->activePolicy;
    previousPolicyFingerprint = impl_->policyFingerprint;
    previousRaw = impl_->observedRaw;
    impl_->config = config;
    impl_->configuredAsgs = requiredAsgs;
  }
  if (!impl_->reloadLocked("config", config, requiredAsgs, error)) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->config = previous;
    impl_->configuredAsgs = std::move(previousAsgs);
    return false;
  }
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->previousConfig = previous;
    impl_->previousConfiguredAsgs = std::move(previousAsgs);
    impl_->previousPolicy = std::move(previousPolicy);
    impl_->previousPolicyFingerprint = std::move(previousPolicyFingerprint);
    impl_->previousRaw = std::move(previousRaw);
    impl_->previousGeneration = impl_->generation.load(std::memory_order_relaxed) - 1u;
    impl_->hasPreviousPolicy = true;
  }
  return true;
}

bool AccessController::restorePrevious(std::string& error) {
  std::lock_guard<std::mutex> reloadGuard(impl_->reloadMutex);
  AccessConfig previousConfig;
  std::set<std::string> previousAsgs;
  std::string previousPolicy;
  std::string previousFingerprint;
  std::string previousRaw;
  uint64_t previousGeneration = 0u;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->hasPreviousPolicy) {
      error = "no previous access policy is available";
      return false;
    }
    previousConfig = impl_->previousConfig;
    previousAsgs = impl_->previousConfiguredAsgs;
    previousPolicy = impl_->previousPolicy;
    previousFingerprint = impl_->previousPolicyFingerprint;
    previousRaw = impl_->previousRaw;
    previousGeneration = impl_->previousGeneration;
  }
  const long status = asInitMem(previousPolicy.c_str(), nullptr);
  if (status != 0) {
    error = std::string("previous ACF restore failed: ") + errSymMsg(status);
    return false;
  }
  impl_->recomputeAllClients();
  const auto restoredFingerprint = previousFingerprint;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->config = std::move(previousConfig);
    impl_->configuredAsgs = std::move(previousAsgs);
    impl_->activePolicy = std::move(previousPolicy);
    impl_->policyFingerprint = std::move(previousFingerprint);
    impl_->observedRaw = std::move(previousRaw);
    impl_->lastStatus = "previous policy restored";
    impl_->lastError.clear();
    impl_->watchStatus = impl_->config.watch.enabled ? "watching " + impl_->config.file : "disabled";
    impl_->hasPreviousPolicy = false;
  }
  impl_->generation.store(previousGeneration, std::memory_order_relaxed);
  std::fprintf(stderr,
               "[redis-pvxs-ioc] previous access policy restored generation=%llu fingerprint=%s clients=%llu\n",
               static_cast<unsigned long long>(previousGeneration), restoredFingerprint.c_str(),
               static_cast<unsigned long long>(impl_->activeClients.load(std::memory_order_relaxed)));
  error.clear();
  return true;
}

bool AccessController::Impl::reloadLocked(const std::string& trigger,
                                          const AccessConfig& configSnapshot,
                                          const std::set<std::string>& requiredAsgs,
                                          std::string& error) {
  try {
    const auto prepared = preparePolicy(configSnapshot, requiredAsgs);
    const long status = asInitMem(prepared.expanded.c_str(), nullptr);
    if (status != 0) {
      throw std::runtime_error(std::string("ACF parse failed: ") + errSymMsg(status));
    }
    recomputeAllClients();
    {
      std::lock_guard<std::mutex> guard(mutex);
      activePolicy = prepared.expanded;
      policyFingerprint = prepared.fingerprint;
      observedRaw = prepared.raw;
      watchLastAttemptRaw.clear();
      watchMissingReported = false;
      lastStatus = trigger + " reload active";
      lastError.clear();
      watchStatus = configSnapshot.watch.enabled ? "watching " + configSnapshot.file : "disabled";
    }
    const auto currentGeneration = generation.fetch_add(1u, std::memory_order_relaxed) + 1u;
    std::fprintf(stderr,
                 "[redis-pvxs-ioc] access policy active trigger=%s generation=%llu fingerprint=%s clients=%llu\n",
                 trigger.c_str(), static_cast<unsigned long long>(currentGeneration),
                 prepared.fingerprint.c_str(),
                 static_cast<unsigned long long>(activeClients.load(std::memory_order_relaxed)));
    error.clear();
    return true;
  } catch (const std::exception& ex) {
    error = ex.what();
    std::string activeFingerprint;
    {
      std::lock_guard<std::mutex> guard(mutex);
      lastStatus = trigger + " reload failed";
      lastError = error;
      activeFingerprint = policyFingerprint;
    }
    std::fprintf(stderr,
                 "[redis-pvxs-ioc] access policy reload failed trigger=%s generation=%llu "
                 "fingerprint=%s clients=%llu error=%s\n",
                 trigger.c_str(),
                 static_cast<unsigned long long>(generation.load(std::memory_order_relaxed)),
                 activeFingerprint.c_str(),
                 static_cast<unsigned long long>(activeClients.load(std::memory_order_relaxed)),
                 error.c_str());
    return false;
  }
}

bool AccessController::reload(const std::string& trigger, std::string& error) {
  std::lock_guard<std::mutex> reloadGuard(impl_->reloadMutex);
  AccessConfig configSnapshot;
  std::set<std::string> requiredAsgs;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    configSnapshot = impl_->config;
    requiredAsgs = impl_->configuredAsgs;
  }
  return impl_->reloadLocked(trigger, configSnapshot, requiredAsgs, error);
}

void AccessController::requestReload(const std::string& trigger) {
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->pendingTrigger = trigger;
}

void AccessController::pump() {
  std::string trigger;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    trigger.swap(impl_->pendingTrigger);
  }
  if (!trigger.empty()) {
    std::string error;
    reload(trigger, error);
  }

  const auto now = std::chrono::steady_clock::now();
  if (now - impl_->lastMaintenance >= std::chrono::seconds(1)) {
    impl_->lastMaintenance = now;
    unsigned changed = 0u;
    const auto status = asRefreshHag(&changed);
    if (status != 0 && status != S_asLib_asNotActive) {
      std::fprintf(stderr, "[redis-pvxs-ioc] HAG refresh failed: %s\n", errSymMsg(status));
    } else if (changed != 0u) {
      impl_->recomputeAllClients();
    } else {
      impl_->drainDirty();
    }
  }

  AccessConfig configSnapshot;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    configSnapshot = impl_->config;
  }
  if (!configSnapshot.watch.enabled ||
      now - impl_->lastWatchPoll < std::chrono::milliseconds(configSnapshot.watch.intervalMs)) return;
  impl_->lastWatchPoll = now;

  try {
    const auto raw = readTextFile(configSnapshot.file);
    const bool recoveredMissing = impl_->watchMissingReported;
    impl_->watchMissingReported = false;
    if (raw == impl_->observedRaw) {
      {
        std::lock_guard<std::mutex> guard(impl_->mutex);
        if (recoveredMissing || impl_->lastStatus == "watch reload failed") {
          impl_->lastStatus = "watch active";
          impl_->lastError.clear();
        }
      }
      impl_->watchCandidateRaw.clear();
      impl_->watchLastAttemptRaw.clear();
      return;
    }
    if (raw == impl_->watchLastAttemptRaw) {
      impl_->watchCandidateRaw.clear();
      return;
    }
    if (raw != impl_->watchCandidateRaw) {
      impl_->watchCandidateRaw = raw;
      impl_->watchCandidateSince = now;
      return;
    }
    if (now - impl_->watchCandidateSince >=
        std::chrono::milliseconds(configSnapshot.watch.settleMs)) {
      impl_->watchLastAttemptRaw = raw;
      std::string error;
      reload("watch", error);
      impl_->watchCandidateRaw.clear();
    }
  } catch (const std::exception& ex) {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    if (!impl_->watchMissingReported) {
      impl_->lastStatus = "watch reload failed";
      impl_->lastError = ex.what();
      impl_->watchMissingReported = true;
      std::fprintf(stderr,
                   "[redis-pvxs-ioc] access policy reload failed trigger=watch generation=%llu "
                   "fingerprint=%s clients=%llu error=%s\n",
                   static_cast<unsigned long long>(impl_->generation.load(std::memory_order_relaxed)),
                   impl_->policyFingerprint.c_str(),
                   static_cast<unsigned long long>(impl_->activeClients.load(std::memory_order_relaxed)),
                   ex.what());
    }
  }
}

struct AccessController::PreparedBindings {
  std::shared_ptr<PVRegistry::Prepared> publication;
  std::unordered_map<std::string, std::shared_ptr<AccessMember>> members;
};

std::shared_ptr<AccessController::PreparedBindings> AccessController::prepareBindings(const PVBindings& bindings) {
  auto result = std::make_shared<PreparedBindings>();
  const auto existing = impl_->registry->bindings();
  decltype(result->members) previous;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    previous = impl_->members;
  }
  PVRegistry::Wrappers wrappers;
  for (const auto& entry : bindings) {
    const auto before = existing.find(entry.first);
    const auto member = previous.find(entry.first);
    std::shared_ptr<AccessMember> prepared;
    if (before != existing.end() && member != previous.end() &&
        before->second.owner == entry.second.owner &&
        sameAccessAssignment(before->second.access, entry.second.access))
      prepared = member->second;
    else
      prepared = std::make_shared<AccessMember>(entry.second.access);
    result->members.emplace(entry.first, prepared);
    wrappers.emplace(entry.first, [weak = std::weak_ptr<Impl>(impl_), prepared, name = entry.first](PVRegistry::Channel channel) -> PVRegistry::Channel {
      const auto owner = weak.lock();
      if (!owner) { channel->close(); return {}; }
      auto state = std::make_shared<ChannelState>(*owner, prepared, name, *channel->credentials());
      state->initialize();
      return std::make_unique<AuthorizedChannelControl>(std::move(channel), std::move(state));
    });
  }
  result->publication = impl_->registry->prepare(bindings, wrappers);
  return result;
}

bool AccessController::publishBindings(const std::shared_ptr<PreparedBindings>& prepared,
    const std::function<bool(std::string&)>& beforeCommit, std::string& error) {
  if (!prepared) { error = "invalid prepared endpoints"; return false; }
  return impl_->registry->publish(prepared->publication, [&](std::string& failure) {
    if (beforeCommit && !beforeCommit(failure)) return false;
    std::lock_guard<std::mutex> guard(impl_->mutex);
    impl_->members.swap(prepared->members);
    return true;
  }, error);
}

void AccessController::finishBindings(const std::shared_ptr<PreparedBindings>& prepared) {
  impl_->registry->finish(prepared->publication);
}

void AccessController::clearBindings() {
  std::string error;
  const auto prepared = prepareBindings({});
  if (!publishBindings(prepared, {}, error)) throw std::runtime_error(error);
  finishBindings(prepared);
}

void AccessController::addPV(const std::string& name,
                             const pvxs::server::SharedPV& pv,
                             const AccessAssignment& assignment) {
  auto desired = impl_->registry->bindings();
  auto owner = std::make_shared<pvxs::server::SharedPV>(pv);
  if (!desired.emplace(name, PVBinding{pv, std::move(owner), assignment}).second)
    throw std::runtime_error("duplicate secured PV '" + name + "'");
  std::string error;
  const auto prepared = prepareBindings(desired);
  if (!publishBindings(prepared, {}, error)) throw std::runtime_error(error);
  finishBindings(prepared);
}

void AccessController::removePV(const std::string& name) {
  auto desired = impl_->registry->bindings();
  if (!desired.erase(name)) return;
  std::string error;
  const auto prepared = prepareBindings(desired);
  if (!publishBindings(prepared, {}, error)) throw std::runtime_error(error);
  finishBindings(prepared);
}

void AccessController::setAssignment(const std::string& name, const AccessAssignment& assignment) {
  auto desired = impl_->registry->bindings();
  const auto found = desired.find(name);
  if (found == desired.end()) throw std::runtime_error("unknown secured PV '" + name + "'");
  if (sameAccessAssignment(found->second.access, assignment)) return;
  found->second.access = assignment;
  std::string error;
  const auto prepared = prepareBindings(desired);
  if (!publishBindings(prepared, {}, error)) throw std::runtime_error(error);
  finishBindings(prepared);
}

std::shared_ptr<pvxs::server::Source> AccessController::source() const { return impl_->registry; }

AccessStatus AccessController::status() const {
  AccessStatus result;
  result.enabled = true;
  result.generation = impl_->generation.load(std::memory_order_relaxed);
  result.activeClients = impl_->activeClients.load(std::memory_order_relaxed);
  result.deniedReads = impl_->deniedReads.load(std::memory_order_relaxed);
  result.deniedWrites = impl_->deniedWrites.load(std::memory_order_relaxed);
  result.rightsChanges = impl_->rightsChanges.load(std::memory_order_relaxed);
  result.authorizedOperations = impl_->authorizedOperations;
  result.operationsInFlight = impl_->operationsInFlight;
  result.operationsSucceeded = impl_->operationsSucceeded; result.operationsFailed = impl_->operationsFailed;
  result.operationsCancelled = impl_->operationsCancelled; result.operationsAbandoned = impl_->operationsAbandoned;
  result.operationsDenied = impl_->operationsDenied;
  result.denialLogsSuppressed = impl_->denialLogsSuppressed;
  std::lock_guard<std::mutex> guard(impl_->mutex);
  result.lastStatus = impl_->lastStatus;
  result.lastError = impl_->lastError;
  result.policyFingerprint = impl_->policyFingerprint;
  result.watchStatus = impl_->watchStatus;
  return result;
}

bool validateAccessPolicy(const AccessConfig& config,
                          const std::set<std::string>& requiredAsgs,
                          std::string& resultFingerprint,
                          std::string& error) {
  if (!config.enabled) {
    resultFingerprint.clear();
    error.clear();
    return true;
  }
  try {
    asCheckClientIP = 1;
    const auto prepared = preparePolicy(config, requiredAsgs);
    resultFingerprint = prepared.fingerprint;
    error.clear();
    return true;
  } catch (const std::exception& ex) {
    resultFingerprint.clear();
    error = ex.what();
    return false;
  }
}

}  // namespace redis_pvxs_ioc
