#include "redis_pvxs_ioc/secrets.h"

#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace redis_pvxs_ioc {

std::string readSecretFile(const std::string& path, const std::string& setting) {
  const auto fail = [&](const std::string& reason) -> void {
    throw std::runtime_error(setting + ": " + reason);
  };
  if (path.empty() || path.find('\0') != std::string::npos) fail("secret file path must be nonempty text");
  const int fd = open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
  if (fd < 0) fail(std::string("cannot open secret file: ") + std::strerror(errno));
  struct Close { int fd; ~Close() { close(fd); } } closer{fd};
  struct stat info{};
  if (fstat(fd, &info) != 0) fail("cannot inspect secret file");
  if (!S_ISREG(info.st_mode)) fail("secret input must be a regular file");
  constexpr size_t limit = 16u * 1024u;
  if (info.st_size > static_cast<off_t>(limit + 2)) fail("secret exceeds 16384 bytes");
  // Read at most the limit plus CRLF plus one byte to detect concurrent growth.
  std::array<char, limit + 3> buffer{};
  size_t used = 0;
  while (used < buffer.size()) {
    const auto count = read(fd, buffer.data() + used, buffer.size() - used);
    if (count == 0) break;
    if (count < 0) {
      if (errno == EINTR) continue;
      fail("cannot read secret file");
    }
    used += static_cast<size_t>(count);
  }
  if (used && buffer[used - 1] == '\n') {
    --used;
    if (used && buffer[used - 1] == '\r') --used;
  }
  if (used > limit) fail("secret exceeds 16384 bytes");
  if (!used) fail("secret file must not be empty");
  const std::string value(buffer.data(), used);
  if (value.find_first_of(std::string("\r\n\0", 3)) != std::string::npos)
    fail("secret must be one line without NUL");
  return value;
}

std::string credentialFromEnvironment(const std::string& name) {
  const auto* direct = std::getenv(name.c_str());
  const auto fileName = name + "_FILE";
  const auto* file = std::getenv(fileName.c_str());
  if (file && *file) {
    if (direct && *direct) throw std::runtime_error(name + " and " + fileName + " are mutually exclusive");
    return readSecretFile(file, fileName);
  }
  return direct ? direct : "";
}

} // namespace redis_pvxs_ioc
