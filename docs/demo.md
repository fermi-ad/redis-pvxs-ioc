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

The runtime container mounts [`demo/config.yaml`](../demo/config.yaml) into `/etc/redis-pvxs-ioc/config.yaml`.
The compose stack uses a local bridge network for service-to-service traffic and does not publish Redis or PVA ports onto the host.
Use `docker exec` for validation.
Stop the demo stack with `docker compose down`.

## Start the complete testbed

The optional legacy sidecar is also registry-backed. A fresh clone can start Redis, `redis-pvxs-ioc`, and the sample conventional IOC sidecar without local image builds:

```sh
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy pull
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy up -d
```

This adds:

- one `legacy-ioc` container serving base records through `pvxsIoc`

The sidecar is on the same local bridge network as the Redis-backed IOC and does not publish host ports by default.
Use [`docs/legacy-sidecar.md`](legacy-sidecar.md) for sidecar-specific validation, optional CA enablement, and the maintainer-only build/push path.

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

The default compose and smoke-test path tracks the published `v0.2.0` release tag. If you need a stricter pin, override `REDIS_PVXS_IOC_IMAGE` with an immutable digest. If you intentionally want the moving convenience tag instead, override `REDIS_PVXS_IOC_IMAGE` with `adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:latest`.

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

The container starts through [`scripts/container-entrypoint.sh`](../scripts/container-entrypoint.sh), which exports the default EPICS multicast network settings before launching the IOC. Override any of those variables with explicit container environment settings if your deployment needs different interfaces or multicast groups.

The writable PV demo stores its raw Redis payload in the `{demo}:magnet:current` stream as a packed binary double.
For the sample transform, a served value of `9.0` corresponds to the raw hex payload `0000000000805640`, which is `90.0` in little-endian IEEE754.

For reload validation, change [`demo/config.yaml`](../demo/config.yaml) on the host and then signal the running process:

```sh
docker exec "$IOC_CONTAINER" /bin/sh -lc 'kill -HUP 1'
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget SYS:demo:config:generation"
```
