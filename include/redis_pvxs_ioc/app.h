#pragma once

#include <atomic>
#include <memory>
#include <string>

namespace redis_pvxs_ioc {

class Application {
public:
  explicit Application(std::string configPath);
  ~Application();

  bool validateOnly(std::string& summary, std::string& error) const;
  bool start(std::string& error);
  void requestReload();
  void pump();
  void stop();

private:
  bool applyConfig(const struct AppConfig& config, bool initialLoad, std::string& error);
  bool replaceAll(const struct AppConfig& config, uint64_t generation, std::string& error);
  bool applyIncremental(const struct AppConfig& config, uint64_t generation, std::string& error);

  std::string configPath_;
  std::atomic<bool> reloadRequested_{false};
  bool started_ = false;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace redis_pvxs_ioc
