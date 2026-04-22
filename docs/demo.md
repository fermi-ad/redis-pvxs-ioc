# Demo Guide

## Pull the images

```sh
docker compose pull
```

## Start the demo stack

```sh
docker compose up -d
docker compose logs -f
```

This launches:

- one standalone Redis container
- one `redis-pvxs-ioc` container

The runtime container mounts [`demo/config.yaml`](/Users/derekste/Dev/epics/redis-pvxs-ioc/demo/config.yaml) into `/etc/redis-pvxs-ioc/config.yaml`.
The compose stack uses a local bridge network for service-to-service traffic and does not publish Redis or PVA ports onto the host.
Use `docker exec` for validation.
Stop the demo stack with `docker compose down`.

## Manual validation

Run the smoke script:

```sh
./scripts/smoke-test.sh
```

The smoke script only pulls and runs published registry images. To validate a specific release digest or alternate config, override the compose variables:

```sh
REDIS_PVXS_IOC_IMAGE=adregistry.fnal.gov/instrumentation/redis-pvxs-ioc@sha256:<digest> \
REDIS_PVXS_IOC_CONFIG=/absolute/path/to/config.yaml \
./scripts/smoke-test.sh
```

Or validate by hand:

```sh
IOC_CONTAINER=redis-pvxs-ioc-demo
REDIS_CONTAINER=redis-pvxs-ioc-redis
PV_ENV='EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1'
PVX_BIN_DIR="/opt/redis-pvxs-ioc/bin/pvxs"

docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:backend:health"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:waveform"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput DEMO:magnet:current 9.0"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:magnet:current"
docker exec "$REDIS_CONTAINER" redis-cli XRANGE acorn:alarms - + COUNT 10
docker exec "$REDIS_CONTAINER" /bin/sh -lc "redis-cli --raw XRANGE '{demo}:magnet:current' - + COUNT 1 | tail -n 1 | xxd -p -c 256"
docker logs -f "$IOC_CONTAINER"
```

The writable PV demo stores its raw Redis payload in the `{demo}:magnet:current` stream as a packed binary double.
For the sample transform, a served value of `9.0` corresponds to the raw hex payload `0000000000805640`, which is `90.0` in little-endian IEEE754.

For reload validation, change [`demo/config.yaml`](/Users/derekste/Dev/epics/redis-pvxs-ioc/demo/config.yaml) on the host and then signal the running process:

```sh
docker exec "$IOC_CONTAINER" /bin/sh -lc 'kill -HUP 1'
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:generation"
```
