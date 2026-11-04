#include "xrm_capture.h"

#include <algorithm>
#include <cassert>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <fcntl.h>
#include <stdexcept>
#include <string>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

namespace {
template<class F> void rejects(F action) {
  bool failed = false;
  try { action(); } catch (const std::exception&) { failed = true; }
  assert(failed);
}
void word(std::string& value, size_t offset, uint32_t bits) {
  for (unsigned i = 0; i < 4; ++i) value[offset + i] = static_cast<char>((bits >> (8 * i)) & 0xff);
}
std::string pulse(uint32_t sequence) {
  std::string value(xrm_capture::pulseBytes, '\0');
  for (size_t offset = 0; offset < 16; offset += 4) word(value, offset, UINT32_C(0xaa55f154));
  word(value, 16, sequence); word(value, 24, sequence);
  word(value, 20, sequence + 7); word(value, 28, sequence + 7);
  value.back() = static_cast<char>(sequence & 0xff);
  return value;
}
xrm_demux::Config demux() {
  xrm_demux::Config config;
  xrm_demux::Source source;
  source.stream = "{XRM}:raw";
  source.format = "acq4xx-es-int16-le";
  source.samplesPerChannel = xrm_capture::samplesPerChannel;
  source.timestampMode = "absent";
  source.retention = source.inputRetention = 1000;
  for (unsigned i = 1; i <= 16; ++i) {
    xrm_demux::Channel channel;
    channel.channel = i;
    source.channels.push_back(channel);
  }
  config.sources.push_back(source);
  return config;
}
const std::string yaml =
    "schema_version: 1\nsource_stream: '{XRM}:raw'\n"
    "device: {host: 127.0.0.1, port: 4210}\n";
std::string replace(std::string input, const std::string& before, const std::string& after) {
  auto offset = input.find(before);
  assert(offset != std::string::npos);
  input.replace(offset, before.size(), after);
  return input;
}
void parseTests() {
  auto source = demux();
  auto config = xrm_capture::parseConfig(yaml, source);
  assert(config.host == "127.0.0.1" && config.port == 4210);
  assert(config.queueLimitBytes == 32u * 1024u * 1024u);
  assert(config.initialReconnectMs == 1000 && config.maxReconnectMs == 30000);
  assert(config.source.inputRetention == 1000 && config.redis.sources.empty());
  config = xrm_capture::parseConfig(replace(yaml, "4210", "45123") +
      "queue_limit_bytes: 524288\nreconnect: {initial_ms: 10, max_ms: 20}\n", source);
  assert(config.port == 45123 && config.queueLimitBytes == xrm_capture::pulseBytes);
  rejects([&] { xrm_capture::parseConfig(yaml + "extra: 1\n", source); });
  rejects([&] { xrm_capture::parseConfig(yaml + "schema_version: 1\n", source); });
  rejects([&] { xrm_capture::parseConfig(yaml + "---\n{}\n", source); });
  rejects([&] { xrm_capture::parseConfig(replace(yaml, "127.0.0.1", "localhost"), source); });
  rejects([&] { xrm_capture::parseConfig(replace(yaml, "127.0.0.1", "224.1.2.3"), source); });
  rejects([&] { xrm_capture::parseConfig(replace(yaml, "4210", "0"), source); });
  rejects([&] { xrm_capture::parseConfig(replace(yaml, "4210", "65536"), source); });
  rejects([&] { xrm_capture::parseConfig(replace(yaml, "'{XRM}:raw'", "missing"), source); });
  for (auto value : {"1", "67108865", "-1", "1.5"})
    rejects([&] { xrm_capture::parseConfig(yaml + "queue_limit_bytes: " + value + "\n", source); });
  rejects([&] { xrm_capture::parseConfig(yaml + "reconnect: {initial_ms: 100, max_ms: 99}\n", source); });
  for (unsigned kind = 0; kind < 7; ++kind) {
    auto invalid = source;
    auto& binding = invalid.sources.front();
    if (kind == 0) binding.samplesPerChannel = 32767;
    if (kind == 1) binding.channels.pop_back();
    if (kind == 2) binding.format = "int16-le";
    if (kind == 3) binding.inputRetention = 0;
    if (kind == 4) binding.inputRetention = 999;
    if (kind == 5) binding.payloadField = "bytes";
    if (kind == 6) {
      binding.timestampMode = "unix-nanoseconds";
      binding.timestampField = "acquisition_time_ns";
      binding.timestampValidField = "acquisition_time_valid";
    }
    rejects([&] { xrm_capture::parseConfig(yaml, invalid); });
  }
  const auto leaseKey = source.sources.front().stream + ":capture:lease:v1";
  for (unsigned kind = 0; kind < 10; ++kind) {
    auto invalid = source;
    xrm_demux::Source other;
    xrm_demux::CurrentConversion conversion;
    if (kind == 0) other.stream = leaseKey;
    if (kind == 1) other.checkpoint = leaseKey;
    if (kind == 2 || kind == 3) {
      xrm_demux::Channel channel;
      if (kind == 2) channel.raw = leaseKey;
      if (kind == 3) channel.volts = leaseKey;
      other.channels.push_back(channel);
    }
    if (kind == 4) conversion.stream = leaseKey;
    if (kind == 5) conversion.checkpoint = leaseKey;
    if (kind == 6) conversion.output = leaseKey;
    if (kind == 7) conversion.setting = leaseKey;
    if (kind == 8) conversion.applied = leaseKey;
    if (kind == 9) invalid.sources.front().checkpoint = leaseKey;
    invalid.sources.push_back(other);
    invalid.currentConversions.push_back(conversion);
    rejects([&] { xrm_capture::parseConfig(yaml, invalid); });
  }
}
void framingTests() {
  const auto first = pulse(1), second = pulse(2);
  for (size_t cut : {size_t(1), size_t(15), size_t(16), size_t(31), size_t(32), size_t(65535)}) {
    xrm_capture::Framer framer;
    std::vector<std::string> output;
    auto sink = [&](std::string&& value) { output.push_back(std::move(value)); };
    auto input = std::string("initial-junk") + first + second;
    for (size_t offset = 0; offset < input.size(); offset += cut) {
      framer.feed(input.data() + offset, std::min(cut, input.size() - offset), sink);
      assert(framer.bufferedBytes() <= xrm_capture::pulseBytes);
    }
    assert(output.size() == 2 && output[0] == first && output[1] == second);
    assert(framer.discardedBytes() == 12 && framer.rejected() == 0);
    assert(framer.bufferedBytes() == 0);
  }
  {
    xrm_capture::Framer framer;
    std::vector<std::string> output;
    auto sink = [&](std::string&& value) { output.push_back(std::move(value)); };
    framer.feed(first.data(), 10000, sink);
    framer.reset();
    framer.feed(second.data(), second.size(), sink);
    assert(output.size() == 1 && output[0] == second && framer.discardedBytes() == 10000);
  }
  for (unsigned invalid = 0; invalid < 3; ++invalid) {
    auto broken = first;
    if (invalid == 0) word(broken, 24, 99);
    if (invalid == 1)
      for (size_t offset = 0; offset < 16; offset += 4) word(broken, offset, UINT32_C(0xaa55f158));
    if (invalid == 2) broken.replace(1001, 32, first.substr(0, 32));
    xrm_capture::Framer framer;
    std::vector<std::string> output;
    auto input = broken + second;
    framer.feed(input.data(), input.size(), [&](std::string&& value) { output.push_back(std::move(value)); });
    assert(output.size() == 1 && output.front() == second);
    assert(framer.rejected() >= 1 && framer.discardedBytes() == first.size());
  }
  {
    xrm_capture::Framer framer;
    auto input = std::string(3 * xrm_capture::pulseBytes, '\0');
    rejects([&] { framer.feed(input.data(), input.size(), [](std::string&&) { assert(false); }); });
    assert(framer.faulted() && framer.bufferedBytes() <= xrm_capture::pulseBytes);
    rejects([&] { framer.feed(first.data(), first.size(), [](std::string&&) { assert(false); }); });
    framer.reset();
    assert(framer.faulted());
  }
}
void blockedLoggingChild() {
  int pipefd[2];
  assert(::pipe(pipefd) == 0);
  const auto flags = ::fcntl(pipefd[1], F_GETFL, 0);
  assert(flags >= 0 && ::fcntl(pipefd[1], F_SETFL, flags | O_NONBLOCK) == 0);
  std::string fill(4096, 'x');
  while (::write(pipefd[1], fill.data(), fill.size()) > 0) {}
  assert(errno == EAGAIN || errno == EWOULDBLOCK);
  assert(::fcntl(pipefd[1], F_SETFL, flags & ~O_NONBLOCK) == 0);
  const auto original = ::dup(STDERR_FILENO);
  assert(original >= 0 && ::dup2(pipefd[1], STDERR_FILENO) == STDERR_FILENO);
  auto config = xrm_capture::parseConfig(yaml, demux());
  config.redis.port = 1;
  config.redis.connectTimeoutMs = config.redis.commandTimeoutMs = 20;
  config.redis.retryMs = 10;
  {
    const auto before = ::fcntl(STDERR_FILENO, F_GETFL, 0);
    xrm_capture::Worker worker(config);
    assert(::fcntl(STDERR_FILENO, F_GETFL, 0) == before);
    worker.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    const auto started = std::chrono::steady_clock::now();
    worker.stop();
    assert(std::chrono::steady_clock::now() - started < std::chrono::seconds(1));
    assert(worker.status().logDrops > 0);
  }
  assert(::dup2(original, STDERR_FILENO) == STDERR_FILENO);
  ::close(original); ::close(pipefd[0]); ::close(pipefd[1]);
}
void blockedLoggingTest() {
  const auto child = ::fork();
  assert(child >= 0);
  if (child == 0) { blockedLoggingChild(); ::_exit(0); }
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
  int status = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = ::waitpid(child, &status, WNOHANG);
    if (result == child) { assert(WIFEXITED(status) && WEXITSTATUS(status) == 0); return; }
    assert(result == 0 || (result < 0 && errno == EINTR));
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ::kill(child, SIGKILL);
  ::waitpid(child, &status, 0);
  assert(false && "capture stop stalled behind a full stderr pipe");
}
}  // namespace

int main() {
  parseTests();
  framingTests();
  blockedLoggingTest();
  std::cout << "capture configuration and framing tests passed\n";
}
