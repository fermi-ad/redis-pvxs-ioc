#include <cassert>
#include <iostream>
#include <string>

#include "redis_pvxs_ioc/config.h"

using namespace redis_pvxs_ioc;

namespace {

bool throwsConfig(const char* text) {
  try {
    static_cast<void>(loadConfigString(text));
    return false;
  } catch (...) {
    return true;
  }
}

const char* const kLegacyConfig = R"YAML(
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
      backend: redis
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

const char* const kMultiBackendConfig = R"YAML(
server:
  instance: multi
redis_backends:
  digit10:
    base_key: BPM
    host: iota-bpm-proton-redis-10
    port: 6379
  system:
    base_key: BPM_SYSTEM
    host: localhost
    port: 6379
alarms:
  backend: system
  stream: acorn:alarms
pvs:
  - name: N:PB10_DDCP_V
    type: uint32
    shape: scalar
    read:
      backend: digit10
      key: DDCP_VERSION_R
  - name: N:PBSYNCGM
    type: uint32
    shape: scalar
    read:
      backend: system
      key: SYNC_GATE_IN_MODE_R
    write:
      backend: system
      key: SYNC_GATE_IN_MODE_S
    confirm:
      backend: system
      key: SYNC_GATE_IN_MODE_R
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

const char* const kDuplicateReadersDifferentBackends = R"YAML(
server:
  instance: test
redis_backends:
  a:
    base_key: one
    host: localhost
    port: 6379
  b:
    base_key: two
    host: localhost
    port: 6380
alarms:
  backend: a
pvs:
  - name: a
    type: float64
    shape: scalar
    read:
      backend: a
      key: same
  - name: b
    type: float64
    shape: scalar
    read:
      backend: b
      key: same
)YAML";

const char* const kUnknownBackend = R"YAML(
server:
  instance: test
redis_backends:
  valid:
    base_key: demo
    host: localhost
    port: 6379
alarms:
  backend: valid
pvs:
  - name: bad
    type: float64
    shape: scalar
    read:
      backend: missing
      key: rb
)YAML";

const char* const kMultiBackendMissingAlarmBackend = R"YAML(
server:
  instance: test
redis_backends:
  a:
    base_key: one
    host: localhost
    port: 6379
  b:
    base_key: two
    host: localhost
    port: 6380
pvs:
  - name: ok
    type: float64
    shape: scalar
    read:
      backend: a
      key: rb
)YAML";

const char* const kRpcConfig = R"YAML(
server:
  instance: rpc
  namespace: BI:BPM
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: live:rb
    type: float64
    shape: scalar
    read:
      key: rb
rpc_services:
  - endpoint: bpm-query-server:50051
    service: bpm.query.v1.BpmQuery
    suffix: _RPC
    defaults:
      digitizer: MTCA1-1
      length_ns: 1000000000
)YAML";

const char* const kRpcOnly = R"YAML(
server:
  instance: rpc
redis:
  base_key: demo
  host: localhost
  port: 6379
rpc_services:
  - endpoint: host:50051
    service: pkg.Svc
)YAML";

const char* const kRpcMissingEndpoint = R"YAML(
server:
  instance: rpc
redis:
  base_key: demo
  host: localhost
  port: 6379
rpc_services:
  - service: pkg.Svc
)YAML";

const char* const kNoPvsNoRpc = R"YAML(
server:
  instance: rpc
redis:
  base_key: demo
  host: localhost
  port: 6379
)YAML";

const char* const kReservedVersionPvName = R"YAML(
server:
  instance: test
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: test:version
    type: string
    shape: scalar
    read:
      key: version
)YAML";

const char* const kReservedRevisionPvName = R"YAML(
server:
  instance: test
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: test:revision
    type: string
    shape: scalar
    read:
      key: revision
)YAML";

const char* const kReservedSysVersionPvName = R"YAML(
server:
  instance: test
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: SYS:test:version
    type: string
    shape: scalar
    read:
      key: sys-version
)YAML";

const char* const kReservedSysRevisionPvName = R"YAML(
server:
  instance: test
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: SYS:test:revision
    type: string
    shape: scalar
    read:
      key: sys-revision
)YAML";

}  // namespace

int main() {
  const auto legacy = loadConfigString(kLegacyConfig);
  assert(legacy.server.instance == "test");
  assert(legacy.server.nameSpace == "DEMO");
  assert(legacy.redisBackends.size() == 1u);
  assert(legacy.redisBackends.count(kDefaultRedisBackendAlias) == 1u);
  assert(legacy.alarms.backend == kDefaultRedisBackendAlias);
  assert(legacy.pvs.size() == 2u);
  assert(legacy.pvs[0].read.backend == kDefaultRedisBackendAlias);
  assert(legacy.pvs[0].confirm == std::nullopt);
  assert(legacy.pvs[0].transform.has_value());
  assert(legacy.pvs[1].shape == Shape::Array);
  assert(versionPVName(legacy.server) == "test:version");
  assert(revisionPVName(legacy.server) == "test:revision");
  assert(adminPVName(legacy.server, "version") == "SYS:test:version");
  assert(adminPVName(legacy.server, "revision") == "SYS:test:revision");

  const auto multi = loadConfigString(kMultiBackendConfig);
  assert(multi.server.instance == "multi");
  assert(multi.redisBackends.size() == 2u);
  assert(multi.alarms.backend == "system");
  assert(multi.pvs.size() == 2u);
  assert(multi.pvs[0].read.backend == "digit10");
  assert(multi.pvs[1].write.has_value());
  assert(multi.pvs[1].write->backend == "system");
  assert(multi.pvs[1].confirm.has_value());
  assert(multi.pvs[1].confirm->backend == "system");

  auto multiChanged = multi;
  multiChanged.redisBackends.at("digit10").host = "other-host";
  assert(!sameRedisBackends(multi.redisBackends, multiChanged.redisBackends));

  auto alarmChanged = multi.alarms;
  alarmChanged.backend = "digit10";
  assert(!sameAlarmStreamConfig(multi.alarms, alarmChanged));

  assert(throwsConfig(kOldSchema));
  assert(throwsConfig(kDuplicateReaders));
  assert(!throwsConfig(kDuplicateReadersDifferentBackends));
  assert(throwsConfig(kUnknownBackend));
  assert(throwsConfig(kMultiBackendMissingAlarmBackend));
  assert(throwsConfig(kReservedVersionPvName));
  assert(throwsConfig(kReservedRevisionPvName));
  assert(throwsConfig(kReservedSysVersionPvName));
  assert(throwsConfig(kReservedSysRevisionPvName));

  // Generic gRPC RPC services (one PV per reflected method at runtime).
  const auto rpc = loadConfigString(kRpcConfig);
  assert(rpc.server.nameSpace == "BI:BPM");
  assert(rpc.pvs.size() == 1u);                  // the Redis PV
  assert(rpc.rpcServices.size() == 1u);
  assert(rpc.rpcServices[0].endpoint == "bpm-query-server:50051");
  assert(rpc.rpcServices[0].service == "bpm.query.v1.BpmQuery");
  assert(rpc.rpcServices[0].suffix == "_RPC");
  assert(rpc.rpcServices[0].defaults.at("digitizer") == "MTCA1-1");
  assert(rpc.rpcServices[0].defaults.at("length_ns") == "1000000000");

  const auto rpcOnly = loadConfigString(kRpcOnly);  // no pvs is allowed
  assert(rpcOnly.pvs.empty());
  assert(rpcOnly.rpcServices.size() == 1u);

  assert(throwsConfig(kRpcMissingEndpoint));     // service without endpoint
  assert(throwsConfig(kNoPvsNoRpc));             // neither pvs nor rpc_services

  std::cout << "config tests passed\n";
  return 0;
}
