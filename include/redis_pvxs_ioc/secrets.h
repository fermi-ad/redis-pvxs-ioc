#pragma once

#include <string>

namespace redis_pvxs_ioc {

// Text credentials are bounded to 16 KiB, with one optional trailing LF/CRLF.
// Errors identify the setting, never file contents. Symlinks to regular files
// are supported for projected container secrets; special files are rejected.
std::string readSecretFile(const std::string& path, const std::string& setting);
std::string credentialFromEnvironment(const std::string& name);

} // namespace redis_pvxs_ioc
