#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PVX_BIN_DIR="/opt/redis-pvxs-ioc/bin/pvxs"
PV_ENV='LANG=C LC_ALL=C EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1'
DEFAULT_REDIS_PVXS_IOC_IMAGE="adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:v0.6.1@sha256:73ef6e1ca9e8e6c344e2663f841186050629b49a3f1ebe3c3ffa1e73ce4bfad5"
USER_SUPPLIED_REDIS_PVXS_IOC_IMAGE="${REDIS_PVXS_IOC_IMAGE+x}"
SOURCE_VERSION="$(tr -d '\n' < VERSION)"

export REDIS_PVXS_IOC_IMAGE="${REDIS_PVXS_IOC_IMAGE:-$DEFAULT_REDIS_PVXS_IOC_IMAGE}"
SOURCE_CONFIG="${REDIS_PVXS_IOC_CONFIG:-$ROOT_DIR/demo/config.yaml}"
TEMP_CONFIG="$(mktemp)"
export REDIS_PVXS_IOC_CONFIG="$TEMP_CONFIG"
cp "$SOURCE_CONFIG" "$REDIS_PVXS_IOC_CONFIG"

cleanup() {
  docker compose down -v >/dev/null 2>&1 || true
}

restore_config() {
  rm -f "${REDIS_PVXS_IOC_CONFIG:-}"
}

trap 'restore_config; cleanup' EXIT

run_with_timeout() {
  if command -v timeout >/dev/null 2>&1; then
    timeout 20 "$@"
  else
    "$@"
  fi
}

# Rewrite the existing inode so native Linux bind mounts observe config edits.
# Tools such as `sed -i` and `perl -pi` replace the file by rename, leaving a
# running container attached to the old inode.
replace_config_text() {
  local from="$1"
  local to="$2"

  perl -0 -e '
    my ($path, $from, $to) = @ARGV;
    open my $config, "+<", $path or die "open $path: $!\n";
    local $/;
    my $text = <$config>;
    $text =~ s/\Q$from\E/$to/ or die "text not found in $path: $from\n";
    seek $config, 0, 0 or die "seek $path: $!\n";
    truncate $config, 0 or die "truncate $path: $!\n";
    print {$config} $text or die "write $path: $!\n";
    close $config or die "close $path: $!\n";
  ' "$REDIS_PVXS_IOC_CONFIG" "$from" "$to"
}

if ! command -v xxd >/dev/null 2>&1; then
  echo "xxd is required on the Docker host for Redis payload validation" >&2
  exit 1
fi

docker compose pull redis
if [ -n "$USER_SUPPLIED_REDIS_PVXS_IOC_IMAGE" ]; then
  docker image inspect "$REDIS_PVXS_IOC_IMAGE" >/dev/null 2>&1 || docker pull "$REDIS_PVXS_IOC_IMAGE"
else
  docker compose pull ioc
fi
docker compose up -d

sleep 5

IOC_CONTAINER="$(docker compose ps -q ioc)"
REDIS_CONTAINER="$(docker compose ps -q redis)"

for _ in {1..30}; do
  if run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:backend:health" | grep -Eq 'value string = "[0-9]+/[0-9]+ connected'; then
    break
  fi
  sleep 2
done

run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:backend:health" | grep -E 'value string = "[0-9]+/[0-9]+ connected'

VERSION_OUTPUT="$(docker exec "$IOC_CONTAINER" /opt/redis-pvxs-ioc/bin/redis-pvxs-ioc --version)"
EXPECTED_VERSION="$(printf '%s\n' "$VERSION_OUTPUT" | sed -n 's/^redis-pvxs-ioc \([^ ]*\).*$/\1/p')"
EXPECTED_REVISION="$(printf '%s\n' "$VERSION_OUTPUT" | sed -n 's/^redis-pvxs-ioc [^ ]* (\([^)]*\))$/\1/p')"
if [ -z "$EXPECTED_VERSION" ] || [ -z "$EXPECTED_REVISION" ]; then
  echo "could not determine redis-pvxs-ioc version and revision from: $VERSION_OUTPUT" >&2
  exit 1
fi
if [ -n "$USER_SUPPLIED_REDIS_PVXS_IOC_IMAGE" ] && [ "$EXPECTED_VERSION" != "$SOURCE_VERSION" ]; then
  echo "user-supplied image version $EXPECTED_VERSION does not match VERSION=$SOURCE_VERSION" >&2
  exit 1
fi

run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget demo:version" | grep "value string = \"redis-pvxs-ioc v${EXPECTED_VERSION}\""
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:version" | grep "value string = \"redis-pvxs-ioc v${EXPECTED_VERSION}\""
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget demo:revision" | grep "value string = \"redis-pvxs-ioc ${EXPECTED_REVISION}\""
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:revision" | grep "value string = \"redis-pvxs-ioc ${EXPECTED_REVISION}\""
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:generation" | grep 'value int64_t = 1'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:lastStatus" | grep 'value string = "generation 1 active"'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:lastError" | grep 'value string = ""'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:stats:pvCount" | grep -E 'value int64_t = [1-9][0-9]*'

if run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput demo:version invalid"; then
  echo "version PV accepted a write unexpectedly" >&2
  exit 1
fi
if run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput demo:revision invalid"; then
  echo "revision PV accepted a write unexpectedly" >&2
  exit 1
fi
if run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput SYS:demo:version invalid"; then
  echo "SYS version PV accepted a write unexpectedly" >&2
  exit 1
fi
if run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput SYS:demo:revision invalid"; then
  echo "SYS revision PV accepted a write unexpectedly" >&2
  exit 1
fi

run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature" | grep 'display.description string = "Source temperature"'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:waveform" | grep 'value float\[] = {5}\[0, 1.5, 3, 1.5, 0\]'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput DEMO:magnet:current 9.0"
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:magnet:current" | grep 'value double = 9'
run_with_timeout docker exec "$REDIS_CONTAINER" /bin/sh -lc "redis-cli --raw XRANGE '{demo}:magnet:current' - + COUNT 1 | tail -n 1" | xxd -p -c 256 | grep '^0000000000805640'
run_with_timeout docker exec "$REDIS_CONTAINER" redis-cli XRANGE acorn:alarms - + COUNT 10 | grep 'DEMO:magnet:current'

replace_config_text 'description: Source temperature' 'description: Source temperature reloaded'
run_with_timeout docker exec "$IOC_CONTAINER" /bin/sh -lc 'kill -HUP 1'
sleep 2
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:generation" | grep 'value int64_t = 2'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:lastStatus" | grep 'value string = "generation 2 active"'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:lastError" | grep 'value string = ""'
for _ in {1..10}; do
  if run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature" | grep -q 'display.description string = "Source temperature reloaded"'; then
    break
  fi
  sleep 1
done
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature" | grep 'display.description string = "Source temperature reloaded"'

# A bad replacement must report the error without replacing generation 2.
replace_config_text 'type: float64' 'type: unsupported'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput SYS:demo:config:reload 1"
for _ in {1..10}; do
  if run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:lastStatus" | grep -q 'value string = "reload failed"'; then
    break
  fi
  sleep 1
done
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:generation" | grep 'value int64_t = 2'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:lastStatus" | grep 'value string = "reload failed"'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:lastError" | grep 'unsupported primitive type'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature" | grep 'display.description string = "Source temperature reloaded"'
