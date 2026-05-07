#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PVX_BIN_DIR="/opt/redis-pvxs-ioc/bin/pvxs"
PV_ENV='LANG=C LC_ALL=C EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1'
DEFAULT_REDIS_PVXS_IOC_IMAGE="adregistry.fnal.gov/instrumentation/redis-pvxs-ioc@sha256:6b7198592219ec258dc90faf17bd9d8c2178648ac1fe0f996641429e8e81a1d3"

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

docker compose pull
docker compose up -d

sleep 5

IOC_CONTAINER="$(docker compose ps -q ioc)"
REDIS_CONTAINER="$(docker compose ps -q redis)"

for _ in {1..30}; do
  if run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:backend:health" | grep -q 'value string = "connected"'; then
    break
  fi
  sleep 2
done

run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:backend:health" | grep 'value string = "connected"'

run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature" | grep 'display.description string = "Source temperature"'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:waveform" | grep 'value float\[] = {5}\[0, 1.5, 3, 1.5, 0\]'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput DEMO:magnet:current 9.0"
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:magnet:current" | grep 'value double = 9'
run_with_timeout docker exec "$REDIS_CONTAINER" /bin/sh -lc "redis-cli --raw XRANGE '{demo}:magnet:current' - + COUNT 1 | tail -n 1 | xxd -p -c 256" | grep '^0000000000805640'
run_with_timeout docker exec "$REDIS_CONTAINER" redis-cli XRANGE acorn:alarms - + COUNT 10 | grep 'DEMO:magnet:current'

LC_ALL=C LANG=C perl -0pi -e 's/description: Source temperature/description: Source temperature reloaded/' "$REDIS_PVXS_IOC_CONFIG"
run_with_timeout docker exec "$IOC_CONTAINER" /bin/sh -lc 'kill -HUP 1'
sleep 2
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:generation" | grep 'value int64_t = 2'
run_with_timeout docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature" | grep 'display.description string = "Source temperature reloaded"'
