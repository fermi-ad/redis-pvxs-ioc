#pragma once
// Test helpers for reading the access audit records the IOC writes to stderr.

#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <tuple>
#include <vector>
#include <unistd.h>

struct CapturedStderr {
  int saved = -1;
  std::filesystem::path path;
  CapturedStderr() {
    path = std::filesystem::temp_directory_path() / ("audit-trail-" + std::to_string(getpid()) + ".log");
    std::fflush(stderr);
    saved = dup(STDERR_FILENO);
    FILE* file = std::fopen(path.c_str(), "w");
    assert(file);
    dup2(fileno(file), STDERR_FILENO);
    std::fclose(file);
  }
  std::string finish() {
    std::fflush(stderr);
    dup2(saved, STDERR_FILENO);
    close(saved);
    std::ifstream in(path);
    std::stringstream text;
    text << in.rdbuf();
    std::filesystem::remove(path);
    return text.str();
  }
};

struct AuditRecord {
  std::string id, phase, operation, result;
  bool operator==(const AuditRecord& other) const {
    return std::tie(id, phase, operation, result) == std::tie(other.id, other.phase, other.operation, other.result);
  }
};

inline std::vector<AuditRecord> auditRecords(const std::string& text) {
  static const std::regex audit(R"(id=(\d+) phase=(\w+) operation=(\w+) pv="[^"]*" result=(\w+))");
  std::vector<AuditRecord> records;
  std::istringstream lines(text);
  for (std::string line; std::getline(lines, line);) {
    std::smatch match;
    if (line.find("access audit ") != std::string::npos && std::regex_search(line, match, audit))
      records.push_back({match[1], match[2], match[3], match[4]});
  }
  return records;
}

// A write admitted under TRAPWRITE and then refused at dispatch keeps its id:
// allowed, denied, then a single denied completion -- and nothing under id 0.
inline bool deniedAtDispatch(const std::vector<AuditRecord>& records, const std::string& id,
                             const std::string& operation) {
  std::vector<AuditRecord> trail;
  for (const auto& record : records) {
    if (record.id == "0") return false;
    if (record.id == id) trail.push_back(record);
  }
  return trail == std::vector<AuditRecord>{{id, "authorization", operation, "allowed"},
                                           {id, "authorization", operation, "denied"},
                                           {id, "completion", operation, "denied"}};
}
