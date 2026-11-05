#pragma once
#include "RedisAdapter.hpp"
#include <cassert>
#include <chrono>
#include <string>
#include <thread>

inline void selectTestReaderMode(RA_Options& options, int argc, char** argv) {
  if (argc > 1) {
    assert(argc == 2 && std::string(argv[1]) == "--keyspace");
    options.readerMode = RA_ReaderMode::Keyspace;
  }
}

inline void assertTestReaderBackend(RedisAdapter& adapter, const RA_Options& options) {
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
  while (adapter.readerBackendStatus().effective != options.readerMode &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  assert(adapter.readerBackendStatus().requested == options.readerMode);
  assert(adapter.readerBackendStatus().effective == options.readerMode);
}
