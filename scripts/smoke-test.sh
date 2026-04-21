#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PVX_BIN_DIR="/opt/redis-pvxs-ioc/bin/pvxs"
PV_ENV='LANG=C LC_ALL=C EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1'

docker build -t redis-pvxs-ioc:dev .
docker compose up -d --build

cleanup() {
  docker compose down -v
}

sleep 5

IOC_CONTAINER="$(docker compose ps -q ioc)"
REDIS_CONTAINER="$(docker compose ps -q redis)"
TMP_CONFIG="$(mktemp)"
cp demo/config.yaml "$TMP_CONFIG"

restore_config() {
  cp "$TMP_CONFIG" demo/config.yaml
  rm -f "$TMP_CONFIG"
}

trap 'restore_config; cleanup' EXIT

for _ in {1..30}; do
  if docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:backend:health" | grep -q 'value string = "connected"'; then
    break
  fi
  sleep 2
done

docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:backend:health" | grep 'value string = "connected"'

docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature" | grep 'display.description string = "Source temperature"'
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:waveform" | grep 'value float\[] = {5}\[0, 1.5, 3, 1.5, 0\]'
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput DEMO:magnet:current 9.0"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:magnet:current" | grep 'value double = 9'
docker exec "$REDIS_CONTAINER" /bin/sh -lc "redis-cli --raw XRANGE '{demo}:magnet:current' - + COUNT 1 | tail -n 1 | xxd -p -c 256" | grep '^0000000000805640'
docker exec "$REDIS_CONTAINER" redis-cli XRANGE acorn:alarms - + COUNT 10 | grep 'DEMO:magnet:current'

LC_ALL=C LANG=C perl -0pi -e 's/description: Source temperature/description: Source temperature reloaded/' demo/config.yaml
docker exec "$IOC_CONTAINER" /bin/sh -lc 'kill -HUP 1'
sleep 2
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:generation" | grep 'value int64_t = 2'
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature" | grep 'display.description string = "Source temperature reloaded"'
