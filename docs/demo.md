# Demo Guide

## Pull the images

```sh
docker compose pull
```

## Start the demo stack

```sh
docker compose up
```

This launches:

- one standalone Redis container
- one `redis-pvxs-ioc` container

The runtime container mounts [`demo/config.yaml`](/Users/derekste/Dev/epics/redis-pvxs-ioc/demo/config.yaml) into `/etc/redis-pvxs-ioc/config.yaml`.
The compose stack uses a local bridge network for service-to-service traffic and does not publish Redis or PVA ports onto the host.
Use `docker exec` for validation.

## Manual validation

Run the smoke script:

```sh
./scripts/smoke-test.sh
```

For local source validation instead of the published IOC image, the smoke script layers [`docker-compose.dev.yml`](/Users/derekste/Dev/epics/redis-pvxs-ioc/docker-compose.dev.yml) on top of the default compose stack and rebuilds the `ioc` service locally.

Or validate by hand:

```sh
IOC_CONTAINER="$(docker compose ps -q ioc)"
REDIS_CONTAINER="$(docker compose ps -q redis)"
PV_ENV='EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1'
PVX_BIN_DIR="/opt/redis-pvxs-ioc/bin/pvxs"

docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:source:temperature"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:waveform"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput DEMO:magnet:current 9.0"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget DEMO:magnet:current"
docker exec "$REDIS_CONTAINER" redis-cli XRANGE acorn:alarms - + COUNT 10
docker exec "$REDIS_CONTAINER" /bin/sh -lc "redis-cli --raw XRANGE '{demo}:magnet:current' - + COUNT 1 | tail -n 1 | xxd -p -c 256"
```

The writable PV demo stores its raw Redis payload in the `{demo}:magnet:current` stream as a packed binary double.
For the sample transform, a served value of `9.0` corresponds to the raw hex payload `0000000000805640`, which is `90.0` in little-endian IEEE754.

For reload validation, change [`demo/config.yaml`](/Users/derekste/Dev/epics/redis-pvxs-ioc/demo/config.yaml) on the host and then signal the running process:

```sh
docker exec "$IOC_CONTAINER" /bin/sh -lc 'kill -HUP 1'
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:generation"
```
