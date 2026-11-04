#include <cassert>
#include <iostream>
#include <string>

#include "redis_pvxs_ioc/config.h"
#include "redis_pvxs_ioc/config_diff.h"

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
    aliases:
      - EXTERNAL:readback
      - EXTERNAL:readback:secondary
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

const char* const kDuplicatePvNames = R"YAML(
server:
  instance: test
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: duplicate
    type: float64
    shape: scalar
    read:
      key: first
  - name: duplicate
    type: float64
    shape: scalar
    read:
      key: second
)YAML";

const char* const kEmptyAliases = R"YAML(
server:
  instance: test
  namespace: DEMO
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: readback
    aliases: []
    type: float64
    shape: scalar
    read:
      key: rb
)YAML";

const char* const kEmptyAlias = R"YAML(
server:
  instance: test
  namespace: DEMO
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: readback
    aliases: [""]
    type: float64
    shape: scalar
    read:
      key: rb
)YAML";

const char* const kAliasMatchesCanonical = R"YAML(
server:
  instance: test
  namespace: DEMO
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: readback
    aliases: [DEMO:readback]
    type: float64
    shape: scalar
    read:
      key: rb
)YAML";

const char* const kDuplicateAliasesOnPv = R"YAML(
server:
  instance: test
  namespace: DEMO
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: readback
    aliases: [EXTERNAL:readback, EXTERNAL:readback]
    type: float64
    shape: scalar
    read:
      key: rb
)YAML";

const char* const kAliasMatchesOtherCanonical = R"YAML(
server:
  instance: test
  namespace: DEMO
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: first
    aliases: [DEMO:second]
    type: float64
    shape: scalar
    read:
      key: first
  - name: second
    type: float64
    shape: scalar
    read:
      key: second
)YAML";

const char* const kAliasMatchesOtherAlias = R"YAML(
server:
  instance: test
  namespace: DEMO
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: first
    aliases: [EXTERNAL:shared]
    type: float64
    shape: scalar
    read:
      key: first
  - name: second
    aliases: [EXTERNAL:shared]
    type: float64
    shape: scalar
    read:
      key: second
)YAML";

const char* const kReservedAlias = R"YAML(
server:
  instance: test
  namespace: DEMO
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: readback
    aliases: [SYS:test:version]
    type: float64
    shape: scalar
    read:
      key: rb
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

const char* const kAccessConfig = R"YAML(
access:
  enabled: true
  file: policies/test.acf
  macros:
    IOC: demo
  watch:
    enabled: true
    interval_ms: 1500
    settle_ms: 300
  defaults:
    pv: {asg: DATA, asl: 0}
    rpc: {asg: RPC, asl: 1}
    admin_read: {asg: STATUS, asl: 0}
    admin_write: {asg: ADMIN, asl: 1}
server:
  instance: secure
redis:
  base_key: demo
  host: localhost
  port: 6379
pvs:
  - name: inherited
    aliases: [EXTERNAL:inherited]
    type: float64
    shape: scalar
    read: {key: inherited}
  - name: overridden
    type: float64
    shape: scalar
    read: {key: overridden}
    access: {asg: SPECIAL, asl: 1}
rpc_services:
  - endpoint: host:50051
    service: pkg.Svc
)YAML";

const char* const kDisabledEndpointAccess = R"YAML(
server: {instance: insecure}
redis: {base_key: demo, host: localhost, port: 6379}
pvs:
  - name: bad
    type: float64
    shape: scalar
    read: {key: bad}
    access: {asg: LOOKS_SECURE, asl: 0}
)YAML";

const char* const kDisabledWatch = R"YAML(
access:
  enabled: false
  watch: {enabled: true}
server: {instance: insecure}
redis: {base_key: demo, host: localhost, port: 6379}
pvs:
  - name: bad
    type: float64
    shape: scalar
    read: {key: bad}
)YAML";

const char* const kDisabledWatchSettings = R"YAML(
access:
  enabled: false
  watch: {enabled: false, interval_ms: 500}
server: {instance: insecure}
redis: {base_key: demo, host: localhost, port: 6379}
pvs:
  - name: bad
    type: float64
    shape: scalar
    read: {key: bad}
)YAML";

const char* const kUnknownAccessKey = R"YAML(
access: {enabled: true, file: test.acf, enabeld: true}
server: {instance: secure}
redis: {base_key: demo, host: localhost, port: 6379}
pvs:
  - name: bad
    type: float64
    shape: scalar
    read: {key: bad}
)YAML";

const char* const kUnknownAssignmentKey = R"YAML(
access: {enabled: true, file: test.acf}
server: {instance: secure}
redis: {base_key: demo, host: localhost, port: 6379}
pvs:
  - name: bad
    type: float64
    shape: scalar
    read: {key: bad}
    access: {asg: DATA, level: 1}
)YAML";

const char* const kEnabledMissingFile = R"YAML(
access: {enabled: true}
server: {instance: secure}
redis: {base_key: demo, host: localhost, port: 6379}
pvs:
  - name: bad
    type: float64
    shape: scalar
    read: {key: bad}
)YAML";

const char* const kInvalidAsl = R"YAML(
access: {enabled: true, file: test.acf}
server: {instance: secure}
redis: {base_key: demo, host: localhost, port: 6379}
pvs:
  - name: bad
    type: float64
    shape: scalar
    read: {key: bad}
    access: {asg: DATA, asl: 2}
)YAML";

}  // namespace

int main(int argc, char** argv) {
  const auto legacy = loadConfigString(kLegacyConfig);
  assert(!legacy.access.enabled);
  assert(legacy.server.instance == "test");
  assert(legacy.server.nameSpace == "DEMO");
  assert(legacy.redisBackends.size() == 1u);
  assert(legacy.redisBackends.count(kDefaultRedisBackendAlias) == 1u);
  assert(legacy.alarms.backend == kDefaultRedisBackendAlias);
  assert(legacy.pvs.size() == 2u);
  assert(legacy.pvs[0].read.backend == kDefaultRedisBackendAlias);
  assert(legacy.pvs[0].aliases.size() == 2u);
  assert(legacy.pvs[0].aliases[0] == "EXTERNAL:readback");
  assert(legacy.pvs[0].aliases[1] == "EXTERNAL:readback:secondary");
  const auto readbackNames = fullPVNames(legacy.server, legacy.pvs[0]);
  assert(readbackNames.size() == 3u);
  assert(readbackNames[0] == "DEMO:readback");
  assert(readbackNames[1] == "EXTERNAL:readback");
  assert(readbackNames[2] == "EXTERNAL:readback:secondary");
  assert(summarizeConfig(legacy).find(
      "aliases=EXTERNAL:readback,EXTERNAL:readback:secondary") != std::string::npos);
  assert(legacy.pvs[0].confirm == std::nullopt);
  assert(legacy.pvs[0].transform.has_value());
  assert(legacy.pvs[1].shape == Shape::Array);
  assert(versionPVName(legacy.server) == "test:version");
  assert(revisionPVName(legacy.server) == "test:revision");
  assert(adminPVName(legacy.server, "version") == "SYS:test:version");
  assert(adminPVName(legacy.server, "revision") == "SYS:test:revision");

  const auto ndarray = loadConfigString(R"YAML(
server: {instance: spike, namespace: "SPIKE:REDIS"}
redis: {base_key: spike, host: localhost, port: 6385}
pvs:
  - name: Image
    kind: ntndarray
    read: {backend: default, key: image}
    max_frame_bytes: 33554432
)YAML");
  assert(ndarray.pvs.size() == 1u);
  assert(ndarray.pvs[0].kind == PVKind::NTNDArray);
  assert(ndarray.pvs[0].maxFrameBytes == 33554432u);
  assert(ndarray.pvs[0].maxFrameGap == kDefaultNDArrayMaxFrameGap);
  assert(ndarray.pvs[0].write == std::nullopt);
  assert(summarizeConfig(ndarray).find("ntndarray max_frame_bytes=33554432") != std::string::npos);
  const std::string gapConfig = "server: {instance: gap}\nredis: {base_key: gap, host: localhost, port: 6385}\npvs:\n"
      "  - name: Image\n    kind: ntndarray\n    read: {key: image}\n    max_frame_gap: ";
  assert(loadConfigString(gapConfig + "0\n").pvs[0].maxFrameGap == 0u);
  assert(loadConfigString(gapConfig + "2147483646\n").pvs[0].maxFrameGap == 2147483646u);
  for (const auto* invalidGap : {"-1", "2147483647", "true"}) {
    assert(throwsConfig((gapConfig + invalidGap + "\n").c_str()));
  }
  assert(throwsConfig("server: {instance: gap}\nredis: {base_key: gap, host: localhost, port: 6385}\npvs:\n"
      "  - name: scalar\n    type: uint32\n    shape: scalar\n    read: {key: value}\n    max_frame_gap: 1\n"));
  assert(throwsConfig(R"YAML(
server: {instance: spike}
redis: {base_key: spike, host: localhost, port: 6385}
pvs:
  - name: Image
    kind: ntndarray
    type: uint8
    read: {key: image}
)YAML"));
  assert(throwsConfig(R"YAML(
server: {instance: spike}
redis: {base_key: spike, host: localhost, port: 6385}
pvs:
  - name: Image
    kind: ntndarray
    read: {key: image}
    write: {key: image:set}
)YAML"));

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
  assert(throwsConfig(kDuplicatePvNames));
  assert(throwsConfig(kEmptyAliases));
  assert(throwsConfig(kEmptyAlias));
  assert(throwsConfig(kAliasMatchesCanonical));
  assert(throwsConfig(kDuplicateAliasesOnPv));
  assert(throwsConfig(kAliasMatchesOtherCanonical));
  assert(throwsConfig(kAliasMatchesOtherAlias));
  assert(throwsConfig(kReservedAlias));
  assert(throwsConfig(kDuplicateReaders));
  assert(!throwsConfig(kDuplicateReadersDifferentBackends));
  assert(throwsConfig(kUnknownBackend));
  assert(throwsConfig(kMultiBackendMissingAlarmBackend));
  assert(throwsConfig(kReservedVersionPvName));
  assert(throwsConfig(kReservedRevisionPvName));
  assert(throwsConfig(kReservedSysVersionPvName));
  assert(throwsConfig(kReservedSysRevisionPvName));
  assert(throwsConfig(kDisabledEndpointAccess));
  assert(throwsConfig(kDisabledWatch));
  assert(throwsConfig(kDisabledWatchSettings));
  assert(throwsConfig(kUnknownAccessKey));
  assert(throwsConfig(kUnknownAssignmentKey));
  assert(!throwsConfig(kEnabledMissingFile));
  assert(throwsConfig(kInvalidAsl));

  const auto access = loadConfigString(kAccessConfig);
  assert(access.access.enabled);
  assert(access.access.file.find("policies/test.acf") != std::string::npos);
  assert(access.access.macros.at("IOC") == "demo");
  assert(access.access.watch.enabled);
  assert(access.access.watch.intervalMs == 1500u);
  assert(access.access.watch.settleMs == 300u);
  assert(access.pvs[0].access.has_value());
  assert(access.pvs[0].access->asg == "DATA");
  assert(access.pvs[0].access->asl == 0);
  assert(access.pvs[0].aliases.size() == 1u);
  assert(access.pvs[1].access->asg == "SPECIAL");
  assert(access.pvs[1].access->asl == 1);
  assert(access.rpcServices[0].access->asg == "RPC");
  assert(access.rpcServices[0].access->asl == 1);
  assert(access.access.defaults.adminRead.asg == "STATUS");
  assert(access.access.defaults.adminWrite.asg == "ADMIN");

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
  assert(!rpc.rpcServices[0].optional && rpc.rpcServices[0].discoveryTimeoutMs == 3000);
  assert(rpc.rpcServices[0].timeoutMs == 10000 && rpc.rpcServices[0].retryIntervalMs == 5000);
  const auto rpcLimits = loadConfigString(std::string(kLegacyConfig) +
      "\nlimits: {rpc_workers: 2, queued_rpc_per_method: 3, queued_rpc_bytes: 4096}\n");
  assert(rpcLimits.limits.rpcWorkers == 2 && rpcLimits.limits.queuedRpcPerMethod == 3 && rpcLimits.limits.queuedRpcBytes == 4096);
  assert(!sameOperationLimits(rpcLimits.limits, rpc.limits));
  for (const auto* setting : {"rpc_workers: 0", "rpc_workers: 65", "queued_rpc_per_method: 0", "queued_rpc_bytes: 1023"}) {
    const auto invalidLimits = std::string(kLegacyConfig) + "\nlimits: {" + setting + "}\n";
    assert(throwsConfig(invalidLimits.c_str()));
  }
  const auto configuredRpc = loadConfigString(std::string(kRpcOnly) +
      "    optional: true\n    discovery_timeout_ms: 200\n    timeout_ms: 1500\n"
      "    retry_interval_ms: 500\n    method_defaults: {Value: {number: 4}}\n");
  assert(configuredRpc.rpcServices[0].optional && configuredRpc.rpcServices[0].discoveryTimeoutMs == 200);
  assert(configuredRpc.rpcServices[0].methodDefaults.at("Value").at("number") == "4");
  for (const auto* setting : {"optional: maybe", "discovery_timeout_ms: 0", "timeout_ms: 300001",
                             "retry_interval_ms: 99", "method_defaults: {Value: [1]}",
                             "method_defaults: {'': {number: 1}}", "defaults: {'': 1}"}) {
    const auto text = std::string(kRpcOnly) + "    " + setting + "\n";
    assert(throwsConfig(text.c_str()));
  }

  const auto rpcOnly = loadConfigString(kRpcOnly);  // no pvs is allowed
  assert(rpcOnly.pvs.empty());
  assert(rpcOnly.rpcServices.size() == 1u);

  assert(throwsConfig(kRpcMissingEndpoint));     // service without endpoint
  assert(throwsConfig(kNoPvsNoRpc));             // neither pvs nor rpc_services

  assert(loadConfigString(kLegacyConfig).discovery.enabled);
  const std::string healthBase = "server: {instance: health}\nredis: {base_key: test, host: localhost, port: 6379";
  const std::string healthPV = "\npvs: [{name: value, type: float64, shape: scalar, read: {key: value}, source_health: {";
  const auto healthConfig = loadConfigString(healthBase + ", reader_probe_ms: 0}" + healthPV + "required: false, stale_after_ms: 1000}}]");
  assert(healthConfig.redisBackends.at("default").readerProbeMs == 0);
  assert(!healthConfig.pvs[0].sourceHealth.required && healthConfig.pvs[0].sourceHealth.staleAfterMs == 1000);
  const auto imageHealth = loadConfigString(healthBase + "}\npvs: [{name: image, kind: ntndarray, read: {key: image}, source_health: {required: false, stale_after_ms: 86400000}}]");
  assert(!imageHealth.pvs[0].sourceHealth.required && imageHealth.pvs[0].sourceHealth.staleAfterMs == 86400000);
  auto beforeHealth = imageHealth, afterHealth = imageHealth;
  afterHealth.pvs[0].sourceHealth.required = true;
  assert(sameReaderTopology(beforeHealth.pvs[0], afterHealth.pvs[0]));
  auto probeChanged = beforeHealth.redisBackends.at("default"); probeChanged.readerProbeMs = 0;
  assert(!sameRedisConfig(beforeHealth.redisBackends.at("default"), probeChanged));
  const auto defaults = loadConfigString(kLegacyConfig);
  assert(defaults.redisBackends.at("default").readerProbeMs == 1000);
  assert(defaults.pvs[0].sourceHealth.required && defaults.pvs[0].sourceHealth.staleAfterMs == 0);
  assert(defaults.pvs[0].sourceTime.mode == SourceTimeMode::LegacyRedisId);
  assert(defaults.pvs[1].sourceTime.mode == SourceTimeMode::LegacyRedisId);
  assert(defaults.pvs[1].sourceTime.field.empty() && defaults.pvs[1].sourceTime.validField.empty());

  const std::string timeBase = "server: {instance: timing}\nredis: {base_key: test, host: localhost, port: 6379}\npvs:\n  - name: wave\n    type: float64\n    shape: array\n    read: {key: wave}\n";
  const auto timingConfig = [&](const std::string& timing, const std::string& extra = "") {
    return timeBase + extra + "    source_time: " + timing + "\n";
  };
  const std::string unixTiming = "{mode: unix-nanoseconds-v1, field: acquisition_time_ns, valid_field: acquisition_time_valid}";
  const auto explicitLegacy = loadConfigString(timingConfig("{mode: legacy-redis-id}"));
  const auto implicitLegacy = loadConfigString(timeBase);
  const auto legacyTimeDiff = diffConfigs(implicitLegacy, explicitLegacy);
  assert(legacyTimeDiff.replaced.empty() && legacyTimeDiff.metadataChanged.empty() && legacyTimeDiff.restartRequired.empty());
  assert(sameReaderTopology(implicitLegacy.pvs[0], explicitLegacy.pvs[0]));
  const auto unixConfig = loadConfigString(timingConfig(unixTiming));
  assert(unixConfig.pvs[0].sourceTime.mode == SourceTimeMode::UnixNanosecondsV1);
  assert(unixConfig.pvs[0].sourceTime.field == "acquisition_time_ns");
  assert(unixConfig.pvs[0].sourceTime.validField == "acquisition_time_valid");
  const auto noTime = loadConfigString(timingConfig("{mode: unavailable}"));
  assert(noTime.pvs[0].sourceTime.mode == SourceTimeMode::Unavailable);
  for (const auto& changed : {unixConfig, noTime}) {
    assert(!sameReaderTopology(implicitLegacy.pvs[0], changed.pvs[0]));
    const auto diff = diffConfigs(implicitLegacy, changed);
    assert(diff.replaced == std::vector<std::string>{"wave"});
    assert(diff.metadataChanged.empty() && diff.restartRequired.empty());
  }
  for (const bool changeValidity : {false, true}) {
    auto changed = unixConfig;
    (changeValidity ? changed.pvs[0].sourceTime.validField : changed.pvs[0].sourceTime.field) = "new_field";
    assert(!sameReaderTopology(unixConfig.pvs[0], changed.pvs[0]));
    const auto diff = diffConfigs(unixConfig, changed);
    assert(diff.replaced == std::vector<std::string>{"wave"} && diff.metadataChanged.empty());
  }
  for (const auto* timing : {"{}", "null", "[]", "unix-nanoseconds-v1", "{mode: unknown}",
      "{mode: unix-nanoseconds}", "{mode: unavailable, field: ''}", "{mode: legacy-redis-id, valid_field: flag}",
      "{mode: unix-nanoseconds-v1}", "{mode: unix-nanoseconds-v1, field: time}",
      "{mode: unix-nanoseconds-v1, valid_field: valid}",
      "{mode: unix-nanoseconds-v1, field: '', valid_field: valid}",
      "{mode: unix-nanoseconds-v1, field: time, valid_field: ''}",
      "{mode: unix-nanoseconds-v1, field: _, valid_field: valid}",
      "{mode: unix-nanoseconds-v1, field: time, valid_field: _}",
      "{mode: unix-nanoseconds-v1, field: time, valid_field: time}",
      "{mode: unavailable, unknown: true}", "{mode: unavailable, mode: unavailable}",
      "{mode: unix-nanoseconds-v1, field: a, field: b, valid_field: c}",
      "{mode: unix-nanoseconds-v1, field: a, valid_field: b, valid_field: c}"})
    assert(throwsConfig(timingConfig(timing).c_str()));
  for (const auto& timing : {unixTiming, std::string("{mode: unavailable}")}) {
    assert(throwsConfig(timingConfig(timing, "    write: {key: command}\n").c_str()));
    assert(throwsConfig(timingConfig(timing, "    write: {key: command}\n    confirm: {key: ack}\n").c_str()));
    assert(throwsConfig(timingConfig(timing, "    confirm: {key: ack}\n").c_str()));
    auto scalarTime = timingConfig(timing);
    scalarTime.replace(scalarTime.find("shape: array"), std::string("shape: array").size(), "shape: scalar");
    assert(throwsConfig(scalarTime.c_str()));
    for (const auto* type : {"bool", "string"}) {
      auto unsupported = timingConfig(timing);
      unsupported.replace(unsupported.find("type: float64"), std::string("type: float64").size(), std::string("type: ") + type);
      assert(throwsConfig(unsupported.c_str()));
    }
  }
  for (const auto* type : {"int8", "uint8", "int16", "uint16", "int32", "uint32", "int64", "uint64", "float32", "float64"}) {
    auto numeric = timingConfig(unixTiming);
    numeric.replace(numeric.find("type: float64"), std::string("type: float64").size(), std::string("type: ") + type);
    assert(!throwsConfig(numeric.c_str()));
  }
  for (const auto* timing : {"{mode: legacy-redis-id}", "{mode: unavailable}",
      "{mode: unix-nanoseconds-v1, field: time, valid_field: valid}"}) {
    const auto image = "server: {instance: image}\nredis: {base_key: test, host: localhost, port: 6379}\npvs:\n  - name: image\n    kind: ntndarray\n    read: {key: image}\n    source_time: " + std::string(timing);
    assert(throwsConfig(image.c_str()));
  }
  // Explicit legacy mode does not narrow pre-existing scalar/write support.
  assert(!throwsConfig(timingConfig("{mode: legacy-redis-id}", "    write: {key: command}\n    confirm: {key: ack}\n").c_str()));
  auto legacyScalar = timingConfig("{mode: legacy-redis-id}");
  legacyScalar.replace(legacyScalar.find("shape: array"), std::string("shape: array").size(), "shape: scalar");
  assert(!throwsConfig(legacyScalar.c_str()));
  for (const auto* setting : {"reader_probe_ms: -1", "reader_probe_ms: 99", "reader_probe_ms: 60001", "reader_probe_ms: 1.5", "reader_probe_mz: 1000"}) {
    const auto text = healthBase + ", " + setting + "}" + healthPV + "}}]";
    assert(throwsConfig(text.c_str()));
  }
  for (const auto* setting : {"stale_after_ms: -1", "stale_after_ms: 86400001", "required: maybe", "require: false", "required: true, required: false"}) {
    const auto text = healthBase + "}" + healthPV + setting + "}}]";
    assert(throwsConfig(text.c_str()));
  }
  for (const auto* setting : {"udp_port: 65536", "timeout_ms: 0", "max_records: 0", "max_bytes: 1023",
                             "bind_address: localhost", "unknown: true", "enabled: true, enabled: false"}) {
    const auto text = std::string(kLegacyConfig) + "\ndiscovery: {" + setting + "}\n";
    assert(throwsConfig(text.c_str()));
  }
  const auto disabled = loadConfigString(std::string(kLegacyConfig) + "\ndiscovery: {enabled: false}\n");
  assert(!disabled.discovery.enabled);

  const std::string gatewayBase =
      "server: {instance: gateway, namespace: EXPORT}\n"
      "redis: {base_key: gateway, host: localhost, port: 6379}\n";
  const auto gateway = loadConfigString(gatewayBase +
      "pva_gateway: {pvs: [{name: table, upstream: 'DEVICE:table', server: '127.0.0.1:15075'}]}\n");
  assert(gateway.pvaGateway.pvs.size() == 1);
  assert(fullGatewayName(gateway.server, gateway.pvaGateway.pvs[0]) == "EXPORT:table");
  assert(gateway.pvaGateway.pvs[0].upstream == "DEVICE:table");
  assert(gateway.pvaGateway.pvs[0].server == "127.0.0.1:15075");
  for (const auto* declaration : {
      "{pvs: [{name: table, upstream: 'DEVICE:table'}]}",
      "{address_list: ['127.0.0.1:5076'], pvs: [{name: table, upstream: 'DEVICE:table', server: '127.0.0.1:5075'}]}",
      "{pvs: [{name: table, upstream: 'DEVICE:table', server: '127.0.0.1'}]}",
      "{pvs: [{name: table, upstream: 'DEVICE:table', server: 'localhost:5075'}]}",
      "{pvs: [{name: table, upstream: 'DEVICE:table', server: '255.255.255.255:5075'}]}",
      "{pvs: [{name: table, upstream: 'DEVICE:table', server: '224.0.0.1:5075'}]}",
      "{pvs: [{name: table, upstream: 'DEVICE:table', server: '127.0.0.1:0'}]}",
      "{pvs: [{name: table, upstream: 'DEVICE:table', server: '127.0.0.1:65536'}]}",
      "{pvs: []}",
      "{pvs: [{name: '', upstream: 'DEVICE:table', server: '127.0.0.1:5075'}]}",
      "{pvs: [{name: table, upstream: 'EXPORT:table', server: '127.0.0.1:5075'}]}",
      "{pvs: [{name: a, upstream: 'EXPORT:b', server: '127.0.0.1:5075'}, {name: b, upstream: 'DEVICE:b', server: '127.0.0.1:5075'}]}",
      "{pvs: [{name: a, upstream: 'DEVICE:a', server: '127.0.0.1:5075'}, {name: b, upstream: 'DEVICE:a', server: '127.0.0.1:5075'}]}",
      "{pvs: [{name: a, upstream: 'DEVICE:a', server: '127.0.0.1:5075', access: {asg: DEVICE}}]}"}) {
    const auto text = gatewayBase + "pva_gateway: " + declaration + "\n";
    assert(throwsConfig(text.c_str()));
  }
  const auto collision = std::string(kLegacyConfig) +
      "\npva_gateway: {pvs: [{name: readback, upstream: 'DEVICE:table', server: '127.0.0.1:5075'}]}\n";
  assert(throwsConfig(collision.c_str()));
  const auto aliasLoop = std::string(kLegacyConfig) +
      "\npva_gateway: {pvs: [{name: remote, upstream: 'EXTERNAL:readback', server: '127.0.0.1:5075'}]}\n";
  assert(throwsConfig(aliasLoop.c_str()));
  const auto sameNameDifferentServers = gatewayBase +
      "pva_gateway: {pvs: [{name: a, upstream: 'DEVICE:a', server: '127.0.0.1:5075'}, "
      "{name: b, upstream: 'DEVICE:a', server: '127.0.0.1:5076'}]}\n";
  assert(loadConfigString(sameNameDifferentServers).pvaGateway.pvs.size() == 2);
  const auto aliasGateway = loadConfigString(gatewayBase +
      "pva_gateway: {file: inventory.yaml, cache_file: state/last-good.yaml, watch: {enabled: true, interval_ms: 100, settle_ms: 0}, pvs: [{name: table, upstream: 'DEVICE:table', server: '127.0.0.1:15075', aliases: ['LEGACY:table'] }]}\n");
  assert(fullGatewayNames(aliasGateway.server, aliasGateway.pvaGateway.pvs[0]) ==
         (std::vector<std::string>{"EXPORT:table", "LEGACY:table"}));
  assert(aliasGateway.pvaGateway.watch.enabled && aliasGateway.pvaGateway.watch.intervalMs == 100);
  assert(aliasGateway.pvaGateway.cacheFile.find("state/last-good.yaml") != std::string::npos);
  const auto manifestBase = loadConfigString(gatewayBase + "pva_gateway: {file: inventory.yaml}\n");
  const auto manifest = loadGatewayManifestString(
      "schema_version: 1\nrevision: r1\npvs: [{name: remote, upstream: 'DEVICE:remote', server: '127.0.0.1:15075', aliases: ['LEGACY:remote']}]\n", manifestBase);
  assert(manifest.revision == "r1" && manifest.pvs.size() == 1);
  assert(withGatewayManifest(manifestBase, manifest).pvaGateway.pvs.size() == 1);
  for (const auto* text : {
      "schema_version: 2\nrevision: bad\npvs: []",
      "schema_version: 1\npvs: []",
      "schema_version: 1\nrevision: bad\naccess: {enabled: false}\npvs: []",
      "schema_version: 1\nrevision: bad\npvs: [{name: a, upstream: A, server: '127.0.0.1:5075', access: {asg: DEFAULT}}]",
      "schema_version: 1\nrevision: bad\npvs: [{name: a, upstream: A, server: '127.0.0.1:5075', aliases: ['SYS:gateway:gateway:status']}]",
      "schema_version: 1\nrevision: bad\npvs: [{name: a, upstream: 'LEGACY:a', server: '127.0.0.1:5075', aliases: ['LEGACY:a']}]",
      "schema_version: 1\nrevision: bad\npvs: [{name: a, upstream: A, server: '127.0.0.1:5075', aliases: ['EXPORT:a']}]",
      "schema_version: 1\nrevision: bad\nrevision: duplicate\npvs: []",
      "schema_version: 1\nrevision: bad\npvs: []\n---\naccess: {enabled: false}"}) {
    bool rejected = false;
    try { loadGatewayManifestString(text, manifestBase); } catch (const std::exception&) { rejected = true; }
    assert(rejected);
  }
  const auto emptyManifest = loadGatewayManifestString("schema_version: 1\nrevision: empty\npvs: []", manifestBase);
  assert(emptyManifest.pvs.empty());
  auto aliased = gateway;
  aliased.pvaGateway.pvs[0].aliases.push_back("LEGACY:table");
  assert(diffConfigs(gateway, aliased).aliasesChanged == std::vector<std::string>{"EXPORT:table"});
  for (const auto* declaration : {
      "{file: x, watch: {enabled: true, interval_ms: 0}}",
      "{file: x, cache_file: x}",
      "{watch: {enabled: true}, pvs: []}",
      "{file: x, access_overrides: {A: {asg: DEFAULT}}}",
      "{pvs: [{name: a, upstream: A, server: '127.0.0.1:5075', aliases: ['EXPORT:a']}]}",
      "{pvs: [{name: a, upstream: A, server: '127.0.0.1:5075', aliases: ['B']}, {name: b, upstream: B, server: '127.0.0.1:5075'}]}"}) {
    assert(throwsConfig((gatewayBase + "pva_gateway: " + declaration + "\n").c_str()));
  }
  auto changedGateway = gateway;
  changedGateway.pvaGateway.pvs[0].upstream = "DEVICE:other";
  auto gatewayDiff = diffConfigs(gateway, changedGateway);
  assert(gatewayDiff.replaced == std::vector<std::string>{"EXPORT:table"});
  changedGateway = gateway;
  changedGateway.pvaGateway.pvs[0].server = "127.0.0.1:15077";
  assert(diffConfigs(gateway, changedGateway).replaced == std::vector<std::string>{"EXPORT:table"});
  changedGateway = gateway;
  changedGateway.pvaGateway.pvs[0].access = AccessAssignment{"DEVICE", 0};
  gatewayDiff = diffConfigs(gateway, changedGateway);
  assert(gatewayDiff.replaced == std::vector<std::string>{"EXPORT:table"});
  assert(gatewayDiff.accessChanged == std::vector<std::string>{"EXPORT:table"});
  auto redisTable = gateway;
  redisTable.pvaGateway = {};
  PVConfig valueTable;
  valueTable.name = "table";
  redisTable.pvs.push_back(valueTable);
  for (const auto& report : {diffConfigs(gateway, redisTable), diffConfigs(redisTable, gateway)}) {
    assert(report.replaced == std::vector<std::string>{"EXPORT:table"});
    assert(report.added.empty() && report.removed.empty());
  }

  if (argc > 1) {
    const auto fileConfig = loadConfigFile(argv[1]);
    assert(fileConfig.access.enabled);
    assert(!fileConfig.access.file.empty());
  }
  std::cout << "config tests passed\n";
  return 0;
}
