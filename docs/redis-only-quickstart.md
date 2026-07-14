# Redis-Only Quick Start

Use this path when you only want Redis-backed PVAccess PVs. No legacy sidecar,
no Channel Access, no local image build.

## Run The Demo

```sh
git clone https://github.com/fermi-ad/redis-pvxs-ioc.git
cd redis-pvxs-ioc
docker compose pull
docker compose up -d
```

Validate from inside the IOC container:

```sh
IOC_CONTAINER=redis-pvxs-ioc-demo
PV_ENV='EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1'
PVX='/opt/redis-pvxs-ioc/bin/pvxs'

docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX/pvxget SYS:demo:backend:health"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX/pvxget DEMO:source:temperature"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX/pvxput DEMO:magnet:current 9.0"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX/pvxget DEMO:magnet:current"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX/pvxget DEMO:waveform"
```

Stop it:

```sh
docker compose down
```

## Run Your Config

Start from the demo config:

```sh
mkdir -p config
cp demo/config.yaml config/redis-pvxs-ioc.yaml
```

Edit at least:

```yaml
server:
  instance: my-ioc
  namespace: MYIOC

redis:
  base_key: my-ioc
  host: redis
  port: 6379
```

Check the config before serving it:

```sh
REDIS_PVXS_IOC_CONFIG="$PWD/config/redis-pvxs-ioc.yaml" \
  docker compose run --rm --no-deps ioc \
  --check-config /etc/redis-pvxs-ioc/config.yaml
```

Then run with your config mounted:

```sh
REDIS_PVXS_IOC_CONFIG="$PWD/config/redis-pvxs-ioc.yaml" docker compose up -d
```

Reload after edits:

```sh
docker exec redis-pvxs-ioc-demo sh -lc 'kill -HUP 1'
```

Or reload through the admin PV:

```sh
PV_ENV='EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1'
PVX='/opt/redis-pvxs-ioc/bin/pvxs'
docker exec redis-pvxs-ioc-demo sh -lc "$PV_ENV $PVX/pvxput SYS:my-ioc:config:reload 1"
docker exec redis-pvxs-ioc-demo sh -lc "$PV_ENV $PVX/pvxget SYS:my-ioc:config:lastStatus"
```

See [`operations.md`](operations.md) for the complete built-in PV table and
rejected-reload behavior.

## Put It In Your Project

Do not submodule this repo for normal deployment. Consume the published image by
digest and mount your project-owned config.

Copy this compose shape into your project:

```yaml
services:
  redis:
    image: adregistry.fnal.gov/instrumentation/redis@sha256:365ee40e627b9faf0775c31c7d6eb9e88a2de201e981b28bd5206029375977b6
    networks:
      - controls
    healthcheck:
      test: ["CMD", "redis-cli", "ping"]
      interval: 2s
      timeout: 2s
      retries: 15

  redis-pvxs-ioc:
    image: adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:v0.6.1@sha256:73ef6e1ca9e8e6c344e2663f841186050629b49a3f1ebe3c3ffa1e73ce4bfad5
    networks:
      - controls
    depends_on:
      redis:
        condition: service_healthy
    volumes:
      - ./config/redis-pvxs-ioc.yaml:/etc/redis-pvxs-ioc/config.yaml:ro
    environment:
      EPICS_HOST_INTERFACE: ${EPICS_HOST_INTERFACE:-eth0}

networks:
  controls:
    driver: bridge
```

For a real controls-network deployment, replace the bridge network with your
site pattern, usually `ipvlan`, `macvlan`, or host networking. See
[`pva-networking.md`](pva-networking.md).

If Redis already exists elsewhere, remove the `redis` service and set
`redis.host` in the IOC config to that reachable hostname.

## Minimal PV Shape

```yaml
pvs:
  - name: device:readback
    type: float64
    shape: scalar
    read:
      backend: redis
      key: device:readback
    metadata:
      description: Device readback
      units: A
      precision: 2
    initial: 0.0

  - name: device:setpoint
    type: float64
    shape: scalar
    read:
      backend: redis
      key: device:setpoint
    write:
      backend: redis
      key: device:setpoint
    confirm:
      backend: redis
      key: device:setpoint
      timeout_ms: 500
    metadata:
      description: Device setpoint
      units: A
      precision: 2
    alarm:
      high_warning: 7
      high_alarm: 9
      hysteresis: 0.1
    initial: 0.0
```

Full example: [`../demo/config.yaml`](../demo/config.yaml).

## Upgrade

1. Open the [latest GitHub Release](https://github.com/fermi-ad/redis-pvxs-ioc/releases/latest).
2. Copy the new `redis-pvxs-ioc:vX.Y.Z@sha256:<digest>` image reference.
3. Update your project compose file.
4. Run `docker compose pull`.
5. Run `docker compose up -d`.
6. Validate `SYS:<instance>:version`, `SYS:<instance>:revision`,
   `SYS:<instance>:backend:health`, and representative PVs.
