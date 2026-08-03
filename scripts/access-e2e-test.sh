#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FIXTURE_DIR="$ROOT_DIR/tests/fixtures/access-e2e"
IMAGE="${REDIS_PVXS_IOC_IMAGE:-redis-pvxs-ioc:acf-local}"
REDIS_IMAGE="${REDIS_IMAGE:-redis:7-alpine}"
mkdir -p "$ROOT_DIR/build"
RUN_DIR="$(mktemp -d "$ROOT_DIR/build/access-e2e-run.XXXXXX")"
RUN_ID="$(basename "$RUN_DIR" | tr '.[:upper:]' '-[:lower:]')"
NETWORK="${RUN_ID}-network"
REDIS_CONTAINER="${RUN_ID}-redis"
IOC_CONTAINER="${RUN_ID}-ioc"
MONITOR_PID=""
PVX_DIR=/opt/redis-pvxs-ioc/bin/pvxs

cleanup() {
  if [ -n "$MONITOR_PID" ]; then
    kill "$MONITOR_PID" >/dev/null 2>&1 || true
  fi
  docker logs "$IOC_CONTAINER" >"$RUN_DIR/ioc.log" 2>&1 || true
  docker logs "$REDIS_CONTAINER" >"$RUN_DIR/redis.log" 2>&1 || true
  docker rm -f "$IOC_CONTAINER" "$REDIS_CONTAINER" >/dev/null 2>&1 || true
  docker network rm "$NETWORK" >/dev/null 2>&1 || true
}
trap cleanup EXIT

pvxget() {
  docker exec "$IOC_CONTAINER" env \
    EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1 \
    "$PVX_DIR/pvxget" -w 3 "$1"
}

pvxput() {
  docker exec "$IOC_CONTAINER" env \
    EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1 \
    "$PVX_DIR/pvxput" -w 3 "$1" "$2"
}

pvxinfo() {
  docker exec "$IOC_CONTAINER" env \
    EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1 \
    "$PVX_DIR/pvxinfo" -w 3 "$1"
}

assert_pv_contains() {
  local pv="$1"
  local expected="$2"
  local output
  output="$(pvxget "$pv" 2>&1)"
  if ! printf '%s\n' "$output" | grep -Fq "$expected"; then
    printf 'PV %s did not contain %s:\n%s\n' "$pv" "$expected" "$output" >&2
    return 1
  fi
}

wait_pv_contains() {
  local pv="$1"
  local expected="$2"
  local output=""
  local attempt
  for attempt in $(seq 1 60); do
    if output="$(pvxget "$pv" 2>&1)" && printf '%s\n' "$output" | grep -Fq "$expected"; then
      return 0
    fi
    sleep 0.1
  done
  printf 'timed out waiting for %s to contain %s:\n%s\n' "$pv" "$expected" "$output" >&2
  return 1
}

wait_file_contains() {
  local path="$1"
  local expected="$2"
  local attempt
  for attempt in $(seq 1 60); do
    if grep -Fq "$expected" "$path" 2>/dev/null; then
      return 0
    fi
    sleep 0.1
  done
  printf 'timed out waiting for %s to contain %s\n' "$path" "$expected" >&2
  return 1
}

assert_int_pv_at_least() {
  local pv="$1"
  local minimum="$2"
  local output
  local value
  output="$(pvxget "$pv" 2>&1)"
  value="$(printf '%s\n' "$output" | sed -n 's/^[[:space:]]*value int64_t = \([0-9][0-9]*\)$/\1/p')"
  if [ -z "$value" ] || [ "$value" -lt "$minimum" ]; then
    printf 'PV %s expected an integer at least %s:\n%s\n' "$pv" "$minimum" "$output" >&2
    return 1
  fi
}

expect_pv_failure() {
  local operation="$1"
  shift
  local output
  output="$($operation "$@" 2>&1)" || true
  if ! printf '%s\n' "$output" | grep -Fq 'access denied'; then
    printf '%s completed without access-denied evidence:\n%s\n' "$operation" "$output" >&2
    return 1
  fi
}

replace_in_place() {
  local source="$1"
  local destination="$2"
  LC_ALL=C perl -e '
    use strict;
    use warnings;
    my ($source, $destination) = @ARGV;
    open my $input, "<", $source or die "open $source: $!\n";
    local $/;
    my $content = <$input>;
    close $input or die "close $source: $!\n";
    open my $output, "+<", $destination or die "open $destination: $!\n";
    seek $output, 0, 0 or die "seek $destination: $!\n";
    truncate $output, 0 or die "truncate $destination: $!\n";
    print {$output} $content or die "write $destination: $!\n";
    close $output or die "close $destination: $!\n";
  ' "$source" "$destination"
}

replace_config_text() {
  local from="$1"
  local to="$2"
  LC_ALL=C perl -0 -e '
    use strict;
    use warnings;
    my ($path, $from, $to) = @ARGV;
    open my $config, "+<", $path or die "open $path: $!\n";
    local $/;
    my $text = <$config>;
    $text =~ s/\Q$from\E/$to/ or die "text not found in $path: $from\n";
    seek $config, 0, 0 or die "seek $path: $!\n";
    truncate $config, 0 or die "truncate $path: $!\n";
    print {$config} $text or die "write $path: $!\n";
    close $config or die "close $path: $!\n";
  ' "$RUN_DIR/config.yaml" "$from" "$to"
}

cp "$FIXTURE_DIR/config.yaml" "$RUN_DIR/config.yaml"
cp "$FIXTURE_DIR/allow.acf" "$RUN_DIR/access.acf"

docker image inspect "$IMAGE" >/dev/null
docker image inspect "$REDIS_IMAGE" >/dev/null
docker network create "$NETWORK" >/dev/null
docker run -d --name "$REDIS_CONTAINER" --network "$NETWORK" \
  --network-alias acf-e2e-redis "$REDIS_IMAGE" >/dev/null

for _ in $(seq 1 40); do
  if docker exec "$REDIS_CONTAINER" redis-cli ping 2>/dev/null | grep -Fq PONG; then
    break
  fi
  sleep 0.1
done
docker exec "$REDIS_CONTAINER" redis-cli ping | grep -Fq PONG

docker run -d --name "$IOC_CONTAINER" --network "$NETWORK" \
  -v "$RUN_DIR:/config" "$IMAGE" --config /config/config.yaml >/dev/null

wait_pv_contains SYS:e2e:backend:health connected
assert_pv_contains SYS:e2e:access:enabled 'value bool = true'
assert_pv_contains SYS:e2e:access:generation 'value int64_t = 1'
assert_pv_contains SYS:e2e:access:lastError 'value string = ""'
assert_pv_contains SYS:e2e:access:watchStatus 'watching /config/access.acf'

assert_pv_contains E2E:value 'value double = 1'
pvxput E2E:value 42 >"$RUN_DIR/allowed-put.txt"
assert_pv_contains E2E:value 'value double = 42'
STREAM_KEY="$(docker exec "$REDIS_CONTAINER" redis-cli --scan | grep -F 'value' | head -n 1)"
if [ -z "$STREAM_KEY" ]; then
  echo 'no Redis stream was created by the allowed PUT' >&2
  exit 1
fi
STREAM_LENGTH="$(docker exec "$REDIS_CONTAINER" redis-cli XLEN "$STREAM_KEY")"
if [ "$STREAM_LENGTH" -ne 1 ]; then
  printf 'expected Redis stream length 1, got %s for %s\n' "$STREAM_LENGTH" "$STREAM_KEY" >&2
  exit 1
fi

docker exec "$IOC_CONTAINER" env \
  EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1 \
  "$PVX_DIR/pvxmonitor" E2E:value >"$RUN_DIR/monitor.log" 2>&1 &
MONITOR_PID=$!
wait_file_contains "$RUN_DIR/monitor.log" Connected

# In-place watcher update: active monitor must disconnect and future operations
# must be rejected without reaching Redis.
replace_in_place "$FIXTURE_DIR/deny.acf" "$RUN_DIR/access.acf"
wait_pv_contains SYS:e2e:access:generation 'value int64_t = 2'
wait_file_contains "$RUN_DIR/monitor.log" Disconnected
expect_pv_failure pvxget E2E:value
expect_pv_failure pvxput E2E:value 43
AFTER_DENY_LENGTH="$(docker exec "$REDIS_CONTAINER" redis-cli XLEN "$STREAM_KEY")"
if [ "$AFTER_DENY_LENGTH" -ne "$STREAM_LENGTH" ]; then
  printf 'denied PUT changed Redis stream length from %s to %s\n' "$STREAM_LENGTH" "$AFTER_DENY_LENGTH" >&2
  exit 1
fi

# Names remain discoverable while GET_FIELD is denied.
docker exec "$IOC_CONTAINER" env \
  EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1 \
  "$PVX_DIR/pvxlist" -w 3 127.0.0.1:5075 | grep -Fq E2E:value
expect_pv_failure pvxinfo E2E:value

# Invalid watcher input retains the last good deny policy and reports a
# line-aware unsupported-construct error.
replace_in_place "$FIXTURE_DIR/invalid.acf" "$RUN_DIR/access.acf"
wait_pv_contains SYS:e2e:access:lastStatus 'watch reload failed'
assert_pv_contains SYS:e2e:access:generation 'value int64_t = 2'
assert_pv_contains SYS:e2e:access:lastError 'ACF 6:19:'
assert_pv_contains SYS:e2e:access:lastError 'CALC'
expect_pv_failure pvxget E2E:value

# Atomic path replacement restores access.
cp "$FIXTURE_DIR/allow.acf" "$RUN_DIR/access.acf.next"
mv "$RUN_DIR/access.acf.next" "$RUN_DIR/access.acf"
wait_pv_contains SYS:e2e:access:generation 'value int64_t = 3'
wait_pv_contains E2E:value 'value double = 42'

# Dedicated reload must activate even when content and fingerprint are unchanged.
pvxput SYS:e2e:access:reload 1 >"$RUN_DIR/access-reload.txt"
wait_pv_contains SYS:e2e:access:generation 'value int64_t = 4'

# Whole-config reload and SIGHUP each reread the ACF and advance both generations.
pvxput SYS:e2e:config:reload 1 >"$RUN_DIR/config-reload.txt"
wait_pv_contains SYS:e2e:config:generation 'value int64_t = 2'
wait_pv_contains SYS:e2e:access:generation 'value int64_t = 5'
docker exec "$IOC_CONTAINER" kill -HUP 1
wait_pv_contains SYS:e2e:config:generation 'value int64_t = 3'
wait_pv_contains SYS:e2e:access:generation 'value int64_t = 6'

# A disabled block may not retain watcher or protection settings that could
# misleadingly imply active enforcement.
replace_config_text 'enabled: true' 'enabled: false'
pvxput SYS:e2e:config:reload 1 >"$RUN_DIR/immutable-reload.txt"
wait_pv_contains SYS:e2e:config:lastStatus 'reload failed'
assert_pv_contains SYS:e2e:config:lastError 'access settings require enabled: true'
assert_pv_contains SYS:e2e:config:generation 'value int64_t = 3'
assert_pv_contains SYS:e2e:access:generation 'value int64_t = 6'
assert_pv_contains E2E:value 'value double = 42'
replace_config_text 'enabled: false' 'enabled: true'

# A well-formed disabled configuration reaches the startup-immutability check;
# its failed reload preserves both active generations and the allow policy.
replace_in_place "$FIXTURE_DIR/config-disabled.yaml" "$RUN_DIR/config.yaml"
pvxput SYS:e2e:config:reload 1 >"$RUN_DIR/immutable-clean-reload.txt"
wait_pv_contains SYS:e2e:config:lastStatus 'reload rejected'
assert_pv_contains SYS:e2e:config:lastError 'access.enabled is immutable after startup'
assert_pv_contains SYS:e2e:config:generation 'value int64_t = 3'
assert_pv_contains SYS:e2e:access:generation 'value int64_t = 6'
assert_pv_contains E2E:value 'value double = 42'
replace_in_place "$FIXTURE_DIR/config.yaml" "$RUN_DIR/config.yaml"

# Disabling the watcher is hot-reloadable. A changed ACF remains inactive until
# an explicit whole-config reload, which always rereads it.
replace_config_text $'watch:\n    enabled: true' $'watch:\n    enabled: false'
pvxput SYS:e2e:config:reload 1 >"$RUN_DIR/disable-watch.txt"
wait_pv_contains SYS:e2e:config:generation 'value int64_t = 4'
wait_pv_contains SYS:e2e:access:generation 'value int64_t = 7'
assert_pv_contains SYS:e2e:access:watchStatus 'value string = "disabled"'
replace_in_place "$FIXTURE_DIR/deny.acf" "$RUN_DIR/access.acf"
sleep 1
assert_pv_contains SYS:e2e:access:generation 'value int64_t = 7'
assert_pv_contains E2E:value 'value double = 42'
pvxput SYS:e2e:config:reload 1 >"$RUN_DIR/disabled-watch-config-reload.txt"
wait_pv_contains SYS:e2e:config:generation 'value int64_t = 5'
wait_pv_contains SYS:e2e:access:generation 'value int64_t = 8'
expect_pv_failure pvxget E2E:value

# Restore the allow policy through whole-config reload while the watcher remains
# disabled, then re-enable monitoring.
replace_in_place "$FIXTURE_DIR/allow.acf" "$RUN_DIR/access.acf"
pvxput SYS:e2e:config:reload 1 >"$RUN_DIR/restore-allow.txt"
wait_pv_contains SYS:e2e:config:generation 'value int64_t = 6'
wait_pv_contains SYS:e2e:access:generation 'value int64_t = 9'
wait_pv_contains E2E:value 'value double = 42'
replace_config_text $'watch:\n    enabled: false' $'watch:\n    enabled: true'
pvxput SYS:e2e:config:reload 1 >"$RUN_DIR/enable-watch.txt"
wait_pv_contains SYS:e2e:config:generation 'value int64_t = 7'
wait_pv_contains SYS:e2e:access:generation 'value int64_t = 10'

# File deletion retains the last good policy. Restoring identical content clears
# the watcher error without spuriously activating a duplicate generation.
mv "$RUN_DIR/access.acf" "$RUN_DIR/access.acf.missing"
wait_pv_contains SYS:e2e:access:lastStatus 'watch reload failed'
assert_pv_contains SYS:e2e:access:lastError 'cannot open ACF file'
assert_pv_contains SYS:e2e:access:generation 'value int64_t = 10'
assert_pv_contains E2E:value 'value double = 42'
mv "$RUN_DIR/access.acf.missing" "$RUN_DIR/access.acf"
wait_pv_contains SYS:e2e:access:lastStatus 'watch active'
assert_pv_contains SYS:e2e:access:lastError 'value string = ""'
assert_pv_contains SYS:e2e:access:generation 'value int64_t = 10'

assert_int_pv_at_least SYS:e2e:access:activeClients 1
assert_int_pv_at_least SYS:e2e:access:deniedReads 1
assert_int_pv_at_least SYS:e2e:access:deniedWrites 1
assert_int_pv_at_least SYS:e2e:access:rightsChanges 1
assert_pv_contains SYS:e2e:access:policyFingerprint 'value string = "'

for status_pv in \
  enabled generation lastStatus lastError policyFingerprint watchStatus \
  activeClients deniedReads deniedWrites rightsChanges; do
  pvxget "SYS:e2e:access:$status_pv" >>"$RUN_DIR/access-status-pvs.txt"
done

docker logs "$IOC_CONTAINER" >"$RUN_DIR/ioc.log" 2>&1
grep -F 'access audit' "$RUN_DIR/ioc.log" | grep -Fq 'result=allowed'
grep -F 'access audit' "$RUN_DIR/ioc.log" | grep -Fq 'result=denied'
grep -Fq 'access policy active trigger=watch' "$RUN_DIR/ioc.log"
grep -Fq 'access policy reload failed trigger=watch' "$RUN_DIR/ioc.log"

printf '%s\n' \
  "image=$IMAGE" \
  "redis_image=$REDIS_IMAGE" \
  "stream_key=$STREAM_KEY" \
  "stream_length_after_denied_write=$AFTER_DENY_LENGTH" \
  'final_config_generation=7' \
  'final_access_generation=10' \
  'result=pass' >"$RUN_DIR/summary.txt"

printf 'ACF end-to-end acceptance passed; artifacts: %s\n' "$RUN_DIR"
