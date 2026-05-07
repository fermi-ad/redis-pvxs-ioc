# Demo Guide

## Start

```sh
docker compose pull
docker compose up -d
```

Full testbed with legacy sidecar:

```sh
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy pull
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy up -d
```

## Validate Redis-Backed PVs

```sh
IOC_CONTAINER=redis-pvxs-ioc-demo
REDIS_CONTAINER=redis-pvxs-ioc-redis
PV_ENV='EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1'
PVX_BIN_DIR='/opt/redis-pvxs-ioc/bin/pvxs'

# backend health and admin PVs
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:backend:health"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:generation"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:stats:pvCount"

# read-only scalar, writable scalar, transformed Redis writeback, and array
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput DEMO:magnet:current 9.0"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:magnet:current"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:waveform"

# Redis alarm stream and raw writeback payload
docker exec "$REDIS_CONTAINER" redis-cli XRANGE acorn:alarms - + COUNT 10
docker exec "$REDIS_CONTAINER" /bin/sh -lc "redis-cli --raw XRANGE '{demo}:magnet:current' - + COUNT 1 | tail -n 1 | xxd -p -c 256"
```

## Reload

Edit [`demo/config.yaml`](../demo/config.yaml), then reload:

```sh
docker exec "$IOC_CONTAINER" /bin/sh -lc 'kill -HUP 1'
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:generation"
```

Or reload through the admin PV:

```sh
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput SYS:demo:config:reload 1"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:lastStatus"
```

## Preview ChannelFinder Catalog Entries

```sh
docker compose \
  -f docker-compose.yml \
  -f docker-compose.channelfinder-sync.yml \
  --profile channelfinder \
  run --rm channelfinder-sync
```

Publish with a config that sets `channelfinder.url`:

```sh
CHANNELFINDER_USERNAME=<user> \
CHANNELFINDER_PASSWORD=<password> \
docker compose \
  -f docker-compose.yml \
  -f docker-compose.channelfinder-sync.yml \
  --profile channelfinder \
  run --rm channelfinder-sync \
  --config /etc/redis-pvxs-ioc/config.yaml
```

## Validate Legacy Sidecar

```sh
SIDE_CAR_PV_ENV='EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=239.128.1.6'

docker exec "$IOC_CONTAINER" sh -lc "$SIDE_CAR_PV_ENV $PVX_BIN_DIR/pvxget LEGACY:readback"
docker exec "$IOC_CONTAINER" sh -lc "$SIDE_CAR_PV_ENV $PVX_BIN_DIR/pvxput LEGACY:setpoint 2.5"
docker exec "$IOC_CONTAINER" sh -lc "$SIDE_CAR_PV_ENV $PVX_BIN_DIR/pvxget LEGACY:setpoint"
docker exec "$IOC_CONTAINER" sh -lc "$SIDE_CAR_PV_ENV $PVX_BIN_DIR/pvxget LEGACY:counter"
docker exec "$IOC_CONTAINER" sh -lc "$SIDE_CAR_PV_ENV $PVX_BIN_DIR/pvxget LEGACY:RecCaster:State-Sts"
docker exec "$IOC_CONTAINER" sh -lc "$SIDE_CAR_PV_ENV $PVX_BIN_DIR/pvxget LEGACY:RecCaster:Msg-I"
```

## Use Your Config

```sh
cp demo/config.yaml /path/to/my-config.yaml
REDIS_PVXS_IOC_CONFIG=/path/to/my-config.yaml docker compose up -d
```

Use a pinned release image:

```sh
REDIS_PVXS_IOC_IMAGE=adregistry.fnal.gov/instrumentation/redis-pvxs-ioc@sha256:<digest> \
REDIS_PVXS_IOC_CONFIG=/path/to/my-config.yaml \
docker compose up -d
```

## Stop

```sh
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy down
```
