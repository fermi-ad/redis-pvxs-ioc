#include "redis_pvxs_ioc/access_control.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
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

struct AccessController::Impl {
  explicit Impl(AccessConfig initial)
      : config(std::move(initial)), staticSource(pvxs::server::StaticSource::build()) {}

  AccessConfig config;
  mutable std::mutex mutex;
  std::mutex reloadMutex;
  pvxs::server::StaticSource staticSource;
  std::shared_ptr<pvxs::server::Source> securedSource;
  std::unordered_map<std::string, std::shared_ptr<AccessMember>> members;
  std::vector<std::weak_ptr<ChannelState>> dirty;
  std::atomic<uint64_t> generation{0};
  std::atomic<uint64_t> activeClients{0};
  std::atomic<uint64_t> deniedReads{0};
  std::atomic<uint64_t> deniedWrites{0};
  std::atomic<uint64_t> rightsChanges{0};
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
  std::map<std::string, std::chrono::steady_clock::time_point> denialLogTimes;

  bool reloadLocked(const std::string& trigger,
                    const AccessConfig& configSnapshot,
                    const std::set<std::string>& requiredAsgs,
                    std::string& error);
  void markDirty(const std::shared_ptr<ChannelState>& state);
  void drainDirty();
  void recomputeAllClients();
  void recordDenied(const ChannelState& state, bool write, const pvxs::Value* value);
  void recordAudit(const ChannelState& state, const pvxs::Value& value);
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
      : owner(owner), member(std::move(accessMember)), name(std::move(channelName)), peer(credentials.peer),
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
      } catch (const std::bad_weak_ptr&) {
      }
    }
  }

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
      if ((rights & kTrapWrite) != 0u) state->owner.recordAudit(*state, value);
      fn(std::move(op), std::move(value));
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
        state->owner.recordDenied(*state, true, &value);
        op->error("access denied");
        return;
      }
      if ((rights & kTrapWrite) != 0u) state->owner.recordAudit(*state, value);
      fn(std::move(op), std::move(value));
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

class AuthorizedSource final : public pvxs::server::Source {
public:
  AuthorizedSource(AccessController::Impl& owner, std::shared_ptr<pvxs::server::Source> target)
      : owner_(owner), target_(std::move(target)) {}

  void onSearch(Search& search) override { target_->onSearch(search); }

  void onCreate(std::unique_ptr<pvxs::server::ChannelControl>&& op) override {
    const auto name = op->name();
    std::shared_ptr<AccessMember> member;
    {
      std::lock_guard<std::mutex> guard(owner_.mutex);
      const auto found = owner_.members.find(op->name());
      if (found == owner_.members.end()) return;
      member = found->second;
    }
    try {
      auto state = std::make_shared<ChannelState>(owner_, std::move(member), name, *op->credentials());
      state->initialize();
      target_->onCreate(std::make_unique<AuthorizedChannelControl>(std::move(op), std::move(state)));
    } catch (const std::exception& ex) {
      std::fprintf(stderr, "[redis-pvxs-ioc] access channel setup failed for %s: %s\n",
                   name.c_str(), ex.what());
      if (op) op->close();
    }
  }

  List onList() override { return target_->onList(); }
  void show(std::ostream& stream) override { target_->show(stream); }

private:
  AccessController::Impl& owner_;
  std::shared_ptr<pvxs::server::Source> target_;
};

}  // namespace

void AccessController::Impl::markDirty(const std::shared_ptr<ChannelState>& state) {
  std::lock_guard<std::mutex> guard(mutex);
  dirty.emplace_back(state);
}

void AccessController::Impl::drainDirty() {
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
                                          const pvxs::Value* value) {
  (write ? deniedWrites : deniedReads).fetch_add(1u, std::memory_order_relaxed);
  const auto preview = value ? valuePreview(*value) : std::string{};
  if (write) {
    const auto timestamp = auditTimestamp();
    std::fprintf(stderr,
                 "[redis-pvxs-ioc] access audit timestamp=%s operation=write pv=%s result=denied "
                 "asg=%s asl=%d user=%s peer=%s auth=%s value=%s\n",
                 timestamp.c_str(), state.name.c_str(), state.member->assignment.asg.c_str(),
                 state.member->assignment.asl, state.account.c_str(), state.peer.c_str(),
                 state.method.c_str(), preview.c_str());
  }
  const auto now = std::chrono::steady_clock::now();
  const auto key = state.name + "\n" + state.peer + "\n" + (write ? "write" : "read");
  bool emit = false;
  {
    std::lock_guard<std::mutex> guard(mutex);
    auto& prior = denialLogTimes[key];
    if (prior.time_since_epoch().count() == 0 || now - prior >= std::chrono::seconds(5)) {
      prior = now;
      emit = true;
    }
  }
  if (!emit) return;
  std::fprintf(stderr,
               "[redis-pvxs-ioc] access denied operation=%s pv=%s result=denied asg=%s asl=%d user=%s peer=%s auth=%s%s%s\n",
               write ? "write" : "read", state.name.c_str(), state.member->assignment.asg.c_str(),
               state.member->assignment.asl,
               state.account.c_str(), state.peer.c_str(), state.method.c_str(),
               value ? " value=" : "", value ? preview.c_str() : "");
}

void AccessController::Impl::recordAudit(const ChannelState& state, const pvxs::Value& value) {
  const auto preview = valuePreview(value);
  const auto timestamp = auditTimestamp();
  std::fprintf(stderr,
               "[redis-pvxs-ioc] access audit timestamp=%s operation=write pv=%s result=allowed asg=%s asl=%d user=%s peer=%s auth=%s value=%s\n",
               timestamp.c_str(), state.name.c_str(), state.member->assignment.asg.c_str(),
               state.member->assignment.asl,
               state.account.c_str(), state.peer.c_str(), state.method.c_str(), preview.c_str());
}

AccessController::AccessController(AccessConfig config)
    : impl_(std::make_unique<Impl>(std::move(config))) {
  impl_->securedSource = std::make_shared<AuthorizedSource>(*impl_, impl_->staticSource.source());
}

AccessController::~AccessController() = default;

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

void AccessController::addPV(const std::string& name,
                             const pvxs::server::SharedPV& pv,
                             const AccessAssignment& assignment) {
  auto member = std::make_shared<AccessMember>(assignment);
  std::lock_guard<std::mutex> guard(impl_->mutex);
  impl_->staticSource.add(name, pv);
  impl_->members[name] = std::move(member);
}

void AccessController::removePV(const std::string& name) {
  std::shared_ptr<AccessMember> member;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->members.find(name);
    if (found != impl_->members.end()) {
      member = std::move(found->second);
      impl_->members.erase(found);
    }
  }
  impl_->staticSource.remove(name);
  if (member) member->closeClients();
}

void AccessController::setAssignment(const std::string& name, const AccessAssignment& assignment) {
  std::shared_ptr<AccessMember> old;
  {
    std::lock_guard<std::mutex> guard(impl_->mutex);
    const auto found = impl_->members.find(name);
    if (found == impl_->members.end()) throw std::runtime_error("unknown secured PV '" + name + "'");
    if (sameAccessAssignment(found->second->assignment, assignment)) return;
    auto replacement = std::make_shared<AccessMember>(assignment);
    old = std::move(found->second);
    found->second = std::move(replacement);
  }
  old->closeClients();
}

std::shared_ptr<pvxs::server::Source> AccessController::source() const { return impl_->securedSource; }

AccessStatus AccessController::status() const {
  AccessStatus result;
  result.enabled = true;
  result.generation = impl_->generation.load(std::memory_order_relaxed);
  result.activeClients = impl_->activeClients.load(std::memory_order_relaxed);
  result.deniedReads = impl_->deniedReads.load(std::memory_order_relaxed);
  result.deniedWrites = impl_->deniedWrites.load(std::memory_order_relaxed);
  result.rightsChanges = impl_->rightsChanges.load(std::memory_order_relaxed);
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
