#include <pvxs/client.h>
#include <yaml-cpp/yaml.h>
#include "xrm_demux.h"

#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/file.h>
#include <sys/stat.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
constexpr size_t maxFileBytes = 16u * 1024u * 1024u;
constexpr size_t maxChannels = 100000u;
constexpr size_t maxEndpoints = 8u;
volatile std::sig_atomic_t stopping = 0;
void stop(int) { stopping = 1; }
void pause(unsigned ms);

struct Options {
  std::string ip, prefix, manifest, compatibility, inventory, demuxConfig, checkDemuxConfig;
  unsigned discoveryPort = 5076, discoveryMs = 3000, requestMs = 3000, refreshMs = 60000, retryMs = 5000;
  bool once = false;
};
struct Inventory { std::string server; std::vector<std::string> channels; };
struct Entry { std::string name, upstream, server; std::vector<std::string> aliases; };
using Compat = std::map<std::pair<std::string, std::string>, std::vector<std::string>>;

void require(bool condition, const std::string& message) {
  if (!condition) throw std::runtime_error(message);
}
unsigned number(const std::string& text, unsigned low, unsigned high, const std::string& option) {
  unsigned value = 0;
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  require(result.ec == std::errc{} && result.ptr == text.data() + text.size() && value >= low && value <= high,
          option + " requires an integer from " + std::to_string(low) + " to " + std::to_string(high));
  return value;
}
std::string ipv4(const std::string& text) {
  in_addr address{};
  require(inet_pton(AF_INET, text.c_str(), &address) == 1, "--ip requires a numeric IPv4 address");
  const auto bits = ntohl(address.s_addr);
  require((bits >> 24) != 0 && (bits >> 24) < 224 && (bits & 0xffu) != 255,
          "--ip requires a unicast IPv4 device address");
  char buffer[INET_ADDRSTRLEN];
  require(inet_ntop(AF_INET, &address, buffer, sizeof(buffer)) != nullptr, "cannot format IPv4 address");
  return buffer;
}
std::string endpoint(const std::string& text, const std::string& ip) {
  const auto colon = text.find(':');
  require(colon != std::string::npos && text.find(':', colon + 1) == std::string::npos,
          "discovered endpoint must be numeric IPv4:port");
  require(ipv4(text.substr(0, colon)) == ip, "discovered endpoint is outside the named device");
  const auto port = number(text.substr(colon + 1), 1, 65535, "endpoint port");
  require(port != 4210, "acquisition port 4210 is forbidden");
  return ip + ":" + std::to_string(port);
}
void name(const std::string& text, const std::string& field) {
  require(!text.empty() && text.size() <= 1024, field + " must contain 1 to 1024 bytes");
  for (const unsigned char ch : text)
    require(ch > 0x20 && ch != 0x7f, field + " contains whitespace or a control character");
}
void usage(std::ostream& out) {
  out << "Usage: xrm-proxy --ip IPv4 --manifest FILE [--prefix PREFIX]\n"
      << "  [--compat-aliases FILE] [--once] [--refresh-ms 60000] [--retry-ms 5000]\n"
      << "  [--discovery-timeout-ms 3000] [--request-timeout-ms 3000]\n"
      << "  [--discovery-port 5076]  Override only for a known PVA discovery listener.\n"
      << "  [--demux-config FILE]  Optional independent Redis raw-stream demultiplexer.\n"
      << "  [--inventory FILE]  Offline PVA inventory; optional Redis worker is independent.\n"
      << "  xrm-proxy --check-demux-config FILE  Validate static config without network I/O.\n";
}
Options options(int argc, char** argv) {
  Options result;
  for (int i = 1; i < argc; ++i) {
    const std::string option = argv[i];
    if (option == "--help" || option == "-h") { usage(std::cout); std::exit(0); }
    if (option == "--once") { result.once = true; continue; }
    require(i + 1 < argc, option + " requires a value");
    const std::string value = argv[++i];
    if (option == "--ip") result.ip = ipv4(value);
    else if (option == "--manifest") result.manifest = value;
    else if (option == "--prefix") result.prefix = value;
    else if (option == "--compat-aliases") result.compatibility = value;
    else if (option == "--inventory") result.inventory = value;
    else if (option == "--demux-config") result.demuxConfig = value;
    else if (option == "--check-demux-config") result.checkDemuxConfig = value;
    else if (option == "--discovery-port") result.discoveryPort = number(value, 1, 65535, option);
    else if (option == "--refresh-ms") result.refreshMs = number(value, 100, 3600000, option);
    else if (option == "--retry-ms") result.retryMs = number(value, 100, 300000, option);
    else if (option == "--discovery-timeout-ms") result.discoveryMs = number(value, 1, 300000, option);
    else if (option == "--request-timeout-ms") result.requestMs = number(value, 1, 300000, option);
    else throw std::runtime_error("unknown argument: " + option);
  }
  if (!result.checkDemuxConfig.empty()) {
    require(argc == 3, "--check-demux-config must be used alone");
    return result;
  }
  require(!result.ip.empty() && !result.manifest.empty(), "--ip and --manifest are required");
  require(result.discoveryPort != 4210, "acquisition port 4210 is forbidden");
  if (!result.prefix.empty()) {
    name(result.prefix, "prefix");
    if (result.prefix.back() != ':') result.prefix += ':';
  }
  return result;
}
std::string read(const std::string& path) {
  std::ifstream input(path, std::ios::binary);
  require(input.good(), "cannot read " + path);
  std::string result;
  char buffer[8192];
  while (input) {
    input.read(buffer, sizeof(buffer));
    const auto count = static_cast<size_t>(input.gcount());
    require(count <= maxFileBytes - result.size(), "file exceeds 16 MiB: " + path);
    result.append(buffer, count);
  }
  require(input.eof(), "cannot finish reading " + path);
  return result;
}
void keys(const YAML::Node& node, std::initializer_list<const char*> allowed, const std::string& path) {
  require(node.IsMap(), path + " must be a mapping");
  std::set<std::string> seen;
  for (const auto& pair : node) {
    const auto key = pair.first.as<std::string>();
    require(seen.insert(key).second, path + " has duplicate field " + key);
    require(std::find(allowed.begin(), allowed.end(), key) != allowed.end(), path + " has unknown field " + key);
  }
}
YAML::Node document(const std::string& path) {
  const auto docs = YAML::LoadAll(read(path));
  require(docs.size() == 1, path + " must contain exactly one YAML document");
  return docs.front();
}
std::string scalar(const YAML::Node& node, const std::string& field) {
  require(node.IsScalar(), field + " must be a scalar string");
  const auto result = node.as<std::string>();
  name(result, field);
  return result;
}
std::vector<std::string> strings(const YAML::Node& node, const std::string& field) {
  require(node.IsSequence() && node.size() > 0 && node.size() <= maxChannels,
          field + " must be a nonempty bounded sequence");
  std::set<std::string> seen;
  for (const auto& item : node) require(seen.insert(scalar(item, field)).second, field + " has a duplicate name");
  return {seen.begin(), seen.end()};
}
Compat compatibility(const std::string& path) {
  Compat result;
  if (path.empty()) return result;
  const auto root = document(path);
  keys(root, {"schema_version", "aliases"}, "compatibility");
  require(root["schema_version"] && root["schema_version"].as<int>() == 1, "unsupported compatibility schema_version");
  require(root["aliases"].IsSequence() && root["aliases"].size() <= maxChannels,
          "compatibility.aliases must be a bounded sequence");
  for (const auto& item : root["aliases"]) {
    keys(item, {"role", "upstream", "aliases"}, "compatibility.aliases entry");
    const auto role = scalar(item["role"], "role");
    require(role == "BASE" || role == "APP", "compatibility role must be BASE or APP");
    const auto upstream = scalar(item["upstream"], "upstream");
    require(result.emplace(std::make_pair(role, upstream), strings(item["aliases"], "aliases")).second,
            "duplicate compatibility source");
  }
  return result;
}
std::vector<Inventory> fixture(const Options& opt) {
  const auto root = document(opt.inventory);
  keys(root, {"schema_version", "servers"}, "inventory");
  require(root["schema_version"] && root["schema_version"].as<int>() == 1, "unsupported inventory schema_version");
  require(root["servers"].IsSequence() && root["servers"].size() <= maxEndpoints, "inventory.servers must be a bounded sequence");
  std::vector<Inventory> result;
  for (const auto& item : root["servers"]) {
    keys(item, {"server", "channels"}, "inventory server");
    result.push_back({endpoint(scalar(item["server"], "server"), opt.ip), strings(item["channels"], "channels")});
  }
  return result;
}
std::vector<Inventory> discover(const Options& opt) {
  // Deliberately avoid fromEnv(): no inherited broadcasts or TCP name servers.
  pvxs::client::Config config;
  config.autoAddrList = false;
  config.addressList = {opt.ip + ":" + std::to_string(opt.discoveryPort)};
  config.udp_port = static_cast<unsigned short>(opt.discoveryPort);
  config.nameServers.clear();
  auto context = config.build();
  std::mutex mutex;
  std::set<std::string> endpoints;
  bool overflow = false;
  auto discovery = context.discover([&](const pvxs::client::Discovered& event) {
    if (event.event != pvxs::client::Discovered::Online || event.proto != "tcp") return;
    try {
      // Validate both the packet origin and advertised connection target.
      endpoint(event.peer, opt.ip);
      const auto server = endpoint(event.server, opt.ip);
      std::lock_guard<std::mutex> lock(mutex);
      if (endpoints.size() >= maxEndpoints && !endpoints.count(server)) overflow = true;
      else endpoints.insert(server);
    } catch (const std::exception&) { /* Unrelated beacons are never targets. */ }
  }).pingAll(true).exec();
  // Discovery delivers a custom callback, so Operation::wait() is not valid.
  // Keep the callback alive for the bounded discovery window, then synchronize.
  pause(opt.discoveryMs);
  discovery->cancel(); // Synchronizes before the captured stack state is read.
  require(!overflow, "too many PVA endpoints on named device");
  require(!endpoints.empty(), "no PVA servers discovered at " + opt.ip);
  std::vector<Inventory> result;
  for (const auto& server : endpoints) {
    if (stopping) throw std::runtime_error("stopping");
    auto value = context.rpc("server").server(server).arg("op", "channels").exec()->wait(opt.requestMs / 1000.0);
    const auto channels = value["value"].as<pvxs::shared_array<const std::string>>();
    require(!channels.empty() && channels.size() <= maxChannels, "invalid or oversized channel inventory from " + server);
    result.push_back({server, {channels.begin(), channels.end()}});
  }
  return result;
}
bool ends(const std::string& text, const std::string& suffix) {
  return text.size() > suffix.size() && text.compare(text.size() - suffix.size(), suffix.size(), suffix) == 0;
}
std::string role(const Inventory& inventory) {
  bool model = false, serial = false, fmt = false, hold = false;
  for (const auto& channel : inventory.channels) {
    model |= ends(channel, ":0:MODEL");
    serial |= ends(channel, ":0:SERIAL");
    fmt |= ends(channel, ":FMT:TAB:Tbl");
    hold |= ends(channel, ":SOE_HLD:TAB:Tbl");
  }
  const bool base = model && serial;
  const bool app = fmt && hold;
  require(base != app, "cannot uniquely classify BASE/APP inventory at " + inventory.server);
  return base ? "BASE" : "APP";
}
std::vector<Entry> entries(const Options& opt, const std::vector<Inventory>& inventories, const Compat& compat) {
  require(inventories.size() == 2, "complete discovery requires exactly one BASE and one APP server");
  std::set<std::string> roles, endpoints, served, upstreams;
  std::set<std::pair<std::string, std::string>> usedCompat;
  std::vector<Entry> result;
  for (const auto& inventory : inventories) {
    const auto server = endpoint(inventory.server, opt.ip);
    require(endpoints.insert(server).second, "duplicate inventory endpoint");
    const auto logicalRole = role(inventory);
    require(roles.insert(logicalRole).second, "ambiguous duplicate " + logicalRole + " server");
    std::set<std::string> sources;
    for (const auto& channel : inventory.channels) {
      name(channel, "upstream PV");
      require(sources.insert(channel).second, "duplicate source name in " + logicalRole);
      upstreams.insert(channel);
      Entry entry{opt.prefix + logicalRole + ":" + channel, channel, server, {}};
      name(entry.name, "exported name");
      const auto key = std::make_pair(logicalRole, channel);
      const auto alias = compat.find(key);
      if (alias != compat.end()) { entry.aliases = alias->second; usedCompat.insert(key); }
      result.push_back(std::move(entry));
    }
  }
  require(usedCompat.size() == compat.size(), "compatibility source is missing from complete inventory");
  require(result.size() <= maxChannels, "combined inventory exceeds channel limit");
  for (const auto& entry : result) {
    require(served.insert(entry.name).second, "duplicate exported name " + entry.name);
    require(!upstreams.count(entry.name), "exported name matches an upstream name " + entry.name);
    for (const auto& alias : entry.aliases) {
      require(served.insert(alias).second, "duplicate exported alias " + alias);
      require(!upstreams.count(alias), "exported alias matches an upstream name " + alias);
    }
  }
  require(served.size() <= maxChannels, "combined canonical and alias names exceed channel limit");
  std::sort(result.begin(), result.end(), [](const Entry& a, const Entry& b) { return a.name < b.name; });
  return result;
}
std::string manifest(const Options& opt, const std::vector<Entry>& entries) {
  YAML::Node pvs(YAML::NodeType::Sequence);
  for (const auto& entry : entries) {
    YAML::Node pv;
    pv["name"] = entry.name;
    pv["upstream"] = entry.upstream;
    pv["server"] = entry.server;
    if (!entry.aliases.empty()) for (const auto& alias : entry.aliases) pv["aliases"].push_back(alias);
    pvs.push_back(pv);
  }
  YAML::Emitter body;
  body << pvs;
  require(body.good(), "cannot serialize manifest entries");
  // A deterministic change identifier, not an authentication checksum.
  uint64_t hash = UINT64_C(14695981039346656037);
  for (const unsigned char ch : std::string(body.c_str())) { hash ^= ch; hash *= UINT64_C(1099511628211); }
  std::ostringstream revision;
  revision << "xrm-" << opt.ip << '-' << std::hex << std::setfill('0') << std::setw(16) << hash;
  YAML::Node root;
  root["schema_version"] = 1;
  root["revision"] = revision.str();
  root["pvs"] = pvs;
  YAML::Emitter output;
  output << root;
  require(output.good(), "cannot serialize manifest");
  std::string result = std::string(output.c_str()) + '\n';
  require(result.size() <= maxFileBytes, "generated manifest exceeds 16 MiB");
  return result;
}
struct File {
  int fd = -1;
  explicit File(int fd) : fd(fd) {}
  ~File() { if (fd >= 0) ::close(fd); }
  File(const File&) = delete;
  File& operator=(const File&) = delete;
};
bool publish(const std::string& path, const std::string& contents) {
  struct stat existing{};
  if (::lstat(path.c_str(), &existing) == 0) {
    require(S_ISREG(existing.st_mode), "manifest must be a regular file");
    if (read(path) == contents) return false;
  } else require(errno == ENOENT, "cannot inspect manifest: " + std::string(std::strerror(errno)));
  const auto parent = std::filesystem::path(path).parent_path();
  const auto dir = parent.empty() ? std::string(".") : parent.string();
  std::string temporary = dir + "/.xrm-manifest.XXXXXX";
  std::vector<char> writable(temporary.begin(), temporary.end());
  writable.push_back('\0');
  File file(::mkstemp(writable.data()));
  require(file.fd >= 0, "cannot create manifest temporary file");
  temporary = writable.data();
  try {
    require(::fchmod(file.fd, 0644) == 0, "cannot set manifest permissions");
    size_t written = 0;
    while (written < contents.size()) {
      const auto count = ::write(file.fd, contents.data() + written, contents.size() - written);
      if (count < 0 && errno == EINTR) continue;
      require(count > 0, "cannot write manifest temporary file");
      written += static_cast<size_t>(count);
    }
    require(::fsync(file.fd) == 0, "cannot synchronize manifest temporary file");
    const auto fd = file.fd;
    file.fd = -1;
    require(::close(fd) == 0, "cannot close manifest temporary file");
    require(::rename(temporary.c_str(), path.c_str()) == 0, "cannot atomically replace manifest");
    // Rename is the commit point. Directory durability failure must not imply rollback.
    File directory(::open(dir.c_str(), O_RDONLY | O_DIRECTORY));
    if (directory.fd < 0 || ::fsync(directory.fd) != 0)
      std::cerr << "xrm-proxy: manifest published; directory synchronization unavailable\n";
  } catch (...) { ::unlink(temporary.c_str()); throw; }
  return true;
}
void pause(unsigned ms) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
  while (!stopping && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
}
} // namespace

int main(int argc, char** argv) {
  try {
    const auto opt = options(argc, argv);
    if (!opt.checkDemuxConfig.empty()) {
      xrm_demux::loadConfig(opt.checkDemuxConfig);
      std::cout << "xrm-proxy: demux configuration is valid\n";
      return 0;
    }
    std::signal(SIGINT, stop);
    std::signal(SIGTERM, stop);
    File lock(::open((opt.manifest + ".lock").c_str(), O_CREAT | O_RDWR | O_NOFOLLOW, 0644));
    require(lock.fd >= 0 && ::flock(lock.fd, LOCK_EX | LOCK_NB) == 0, "manifest already owned or lock unavailable");
    std::unique_ptr<xrm_demux::Worker> demux;
    if (!opt.demuxConfig.empty()) {
      try {
        demux = std::make_unique<xrm_demux::Worker>(xrm_demux::loadConfig(opt.demuxConfig));
        demux->start();
      } catch (const std::exception& error) {
        demux.reset();
        std::cerr << "xrm-proxy: demux startup failed; PVA inventory continues: " << error.what() << '\n';
      }
    }
    std::string previousDemuxStatus;
    const auto reportDemux = [&] {
      if (!demux) return;
      try {
        const auto status = demux->status();
        std::ostringstream line;
        line << "state=" << status.state << " running=" << (status.running ? "true" : "false")
             << " processed=" << status.processed << " rejected=" << status.rejected
             << " failures=" << status.failures << " gaps=" << status.gaps;
        if (!status.error.empty()) line << " error=" << std::quoted(status.error.substr(0, 2048));
        if (line.str() != previousDemuxStatus) {
          previousDemuxStatus = line.str();
          std::cerr << "xrm-proxy: demux " << previousDemuxStatus << '\n';
        }
      } catch (const std::exception& error) {
        std::cerr << "xrm-proxy: demux status unavailable; PVA inventory continues: " << error.what() << '\n';
      }
    };
    do {
      reportDemux();
      try {
        const auto aliases = compatibility(opt.compatibility);
        const auto inventories = opt.inventory.empty() ? discover(opt) : fixture(opt);
        const auto pvs = entries(opt, inventories, aliases);
        if (stopping) break;
        const auto changed = publish(opt.manifest, manifest(opt, pvs));
        std::cout << "xrm-proxy: " << (changed ? "published " : "unchanged ") << pvs.size()
                  << " PVs from BASE and APP at " << opt.ip << '\n' << std::flush;
        reportDemux();
        if (opt.once) return 0;
        pause(opt.refreshMs);
      } catch (const std::exception& error) {
        std::cerr << "xrm-proxy: inventory rejected; previous manifest retained: " << error.what() << '\n';
        reportDemux();
        if (opt.once) return 1;
        pause(opt.retryMs);
      }
    } while (!stopping);
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "xrm-proxy: " << error.what() << '\n';
    return 1;
  }
}
