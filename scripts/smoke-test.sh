#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PVX_BIN_DIR="/opt/redis-pvxs-ioc/bin/pvxs"
PV_ENV='LANG=C LC_ALL=C EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1'
DEFAULT_REDIS_PVXS_IOC_IMAGE="adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:v0.5.1@sha256:6c473e996d0091994868ac00f63b1b80f5b4ede067165c3a66182ef5dd5efc69"
USER_SUPPLIED_REDIS_PVXS_IOC_IMAGE="${REDIS_PVXS_IOC_IMAGE+x}"
EXPECTED_VERSION="$(tr -d '\n' < VERSION)"

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

run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget demo:version" | grep "value string = \"redis-pvxs-ioc v${EXPECTED_VERSION}\""
if run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput demo:version invalid"; then
  echo "version PV accepted a write unexpectedly" >&2
  exit 1
fi
if [ -n "$USER_SUPPLIED_REDIS_PVXS_IOC_IMAGE" ]; then
  EXPECTED_REVISION="$(docker exec "$IOC_CONTAINER" /opt/redis-pvxs-ioc/bin/redis-pvxs-ioc --version | sed -n 's/^redis-pvxs-ioc [^ ]* (\([^)]*\))$/\1/p')"
  if [ -z "$EXPECTED_REVISION" ]; then
    echo "could not determine redis-pvxs-ioc revision from --version" >&2
    exit 1
  fi
  run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:version" | grep "value string = \"redis-pvxs-ioc v${EXPECTED_VERSION}\""
  run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget demo:revision" | grep "value string = \"redis-pvxs-ioc ${EXPECTED_REVISION}\""
  run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:revision" | grep "value string = \"redis-pvxs-ioc ${EXPECTED_REVISION}\""
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
fi

run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature" | grep 'display.description string = "Source temperature"'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:waveform" | grep 'value float\[] = {5}\[0, 1.5, 3, 1.5, 0\]'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput DEMO:magnet:current 9.0"
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:magnet:current" | grep 'value double = 9'
run_with_timeout docker exec "$REDIS_CONTAINER" /bin/sh -lc "redis-cli --raw XRANGE '{demo}:magnet:current' - + COUNT 1 | tail -n 1" | xxd -p -c 256 | grep '^0000000000805640'
run_with_timeout docker exec "$REDIS_CONTAINER" redis-cli XRANGE acorn:alarms - + COUNT 10 | grep 'DEMO:magnet:current'

LC_ALL=C LANG=C perl -0pi -e 's/description: Source temperature/description: Source temperature reloaded/' "$REDIS_PVXS_IOC_CONFIG"
run_with_timeout docker exec "$IOC_CONTAINER" /bin/sh -lc 'kill -HUP 1'
sleep 2
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:generation" | grep 'value int64_t = 2'
for _ in {1..10}; do
  if run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature" | grep -q 'display.description string = "Source temperature reloaded"'; then
    break
  fi
  sleep 1
done
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature" | grep 'display.description string = "Source temperature reloaded"'
