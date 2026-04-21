#include <cassert>
#include <iostream>
#include <string>

#include "redis_pvxs_ioc/config.h"

using namespace redis_pvxs_ioc;

namespace {

const char* const kValidConfig = R"YAML(
server:
  instance: test
  namespace: DEMO
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: readback
    type: float64
    shape: scalar
    read:
      key: rb
    metadata:
      units: A
      precision: 3
      form: engineering
      display:
        low: 0
        high: 10
      control:
        low: 0
        high: 12
    alarm:
      low_warning: 1
      high_warning: 9
    transform:
      scale: 0.5
      offset: 1.0
    initial: 2.0
  - name: waveform
    type: float32
    shape: array
    read:
      key: wave
    initial: [1.0, 2.0, 3.0]
)YAML";

const char* const kOldSchema = R"YAML(
PVBase: DEMO
PVList: []
)YAML";

const char* const kDuplicateReaders = R"YAML(
server:
  instance: test
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: a
    type: float64
    shape: scalar
    read:
      key: same
  - name: b
    type: float64
    shape: scalar
    read:
      key: same
)YAML";

}  // namespace

int main() {
  const auto config = loadConfigString(kValidConfig);
  assert(config.server.instance == "test");
  assert(config.server.nameSpace == "DEMO");
  assert(config.pvs.size() == 2);
  assert(config.pvs[0].confirm == std::nullopt);
  assert(config.pvs[0].transform.has_value());
  assert(config.pvs[1].shape == Shape::Array);

  bool threw = false;
  try {
    static_cast<void>(loadConfigString(kOldSchema));
  } catch (...) {
    threw = true;
  }
  assert(threw);

  threw = false;
  try {
    static_cast<void>(loadConfigString(kDuplicateReaders));
  } catch (...) {
    threw = true;
  }
  assert(threw);

  std::cout << "config tests passed\n";
  return 0;
}
