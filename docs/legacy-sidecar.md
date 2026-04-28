# Legacy IOC Sidecar

The legacy sidecar is an optional conventional EPICS IOC container that can run beside `redis-pvxs-ioc`.

If you already have a working `.db` / `.dbd` IOC or a support module people depend on, start here. The sidecar lets that IOC join the PVA testbed without changing the Redis-first runtime.

The rule is simple: keep `redis-pvxs-ioc` clean, put legacy EPICS record/device-support behavior in a separate sidecar image, publish that image to the registry, and run both containers on the same PVA network.

## What This Supports

- a conventional IOC startup script
- base records from `.db` files
- PVA exposure through `pvxsIoc` / QSRV2
- an optional compose service on the same PVA network as `redis-pvxs-ioc`
- a template that teams can derive from for real support-module images

## What This Does Not Support

- loading arbitrary support modules into `redis-pvxs-ioc`
- mounting an unknown `.dbd` file and expecting missing C/C++ registrar or device-support code to appear
- re-exporting sidecar PVs through `redis-pvxs-ioc`
- Redis shadowing of sidecar values
- CA compatibility by default
- hot reload of legacy `.dbd` or `.db` structure after `iocInit()`

## Why A `.dbd` Is Not Enough

A `.dbd` file describes record types, device support, menus, functions, and registrars. If it references a support module, the corresponding compiled code must be linked into the IOC application image.

The sample sidecar image links only:

- EPICS Base IOC libraries
- `pvxs`
- `pvxsIoc`
- base record/device support

For a real support module, derive a custom sidecar image and link that module into the IOC app. Do not treat the sample image as a universal `.dbd` loader.

## Run The Sample

```sh
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy pull
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy up -d
```

Validate:

```sh
IOC_CONTAINER=redis-pvxs-ioc-demo
PV_ENV='EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=239.128.1.6'
PVX_BIN_DIR='/opt/redis-pvxs-ioc/bin/pvxs'

docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget LEGACY:readback"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxput LEGACY:setpoint 2.5"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget LEGACY:setpoint"
```

Use a different startup script:

```sh
LEGACY_IOC_STARTUP_HOST=/path/to/st.cmd \
  docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy up -d
```

## Optional Channel Access

CA is disabled by default. Enable it only for a sidecar that must serve legacy CA clients:

```sh
LEGACY_IOC_ENABLE_CA=YES \
  docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy up -d
```

## Deriving A Real Support-Module Sidecar

Fast path:

```sh
cp -R legacy-sidecar /path/to/my-project/legacy-sidecar
```

Then edit:

- `legacy-sidecar/app/legacyIocApp/src/Makefile`
- `legacy-sidecar/iocBoot/st.cmd`
- `legacy-sidecar/app/legacyIocApp/Db/`
- `legacy-sidecar/Dockerfile` if the support module needs extra source packages or build steps

A user-owned sidecar image should contain:

- the conventional IOC executable
- its generated `.dbd`
- all linked support-module libraries needed by that `.dbd`
- `.db` files
- startup script
- EPICS Base runtime libraries
- `pvxs` and `pvxsIoc` when PVA exposure is required

The important application pattern is:

```makefile
PROD_IOC = legacy

DBD += legacy.dbd
legacy_DBD += base.dbd
legacy_DBD += pvxsIoc.dbd
legacy_DBD += mySupportModule.dbd

legacy_SRCS += legacy_registerRecordDeviceDriver.cpp
legacy_SRCS_DEFAULT += legacyMain.cpp

legacy_LIBS += mySupportModule
legacy_LIBS += pvxsIoc
legacy_LIBS += pvxs
legacy_LIBS += $(EPICS_BASE_IOC_LIBS)
```

And the startup pattern is:

```iocsh
epicsEnvSet("PVXS_QSRV_ENABLE", "YES")

dbLoadDatabase("/opt/legacy-ioc/dbd/legacy.dbd")
legacy_registerRecordDeviceDriver(pdbbase)

dbLoadRecords("/opt/legacy-ioc/db/records.db", "P=LEGACY:")

iocInit()
```

The sidecar and `redis-pvxs-ioc` share a PVA network, but they are separate processes with separate failure domains.

Common mistakes:

- Do not mount only a `.dbd` and expect missing support code to appear.
- Do not make normal users build images locally.
- Do not enable CA unless the sidecar must serve legacy CA clients.

## Use Your Sidecar Image

Push the project sidecar image to the registry, then override the image:

```yaml
services:
  legacy-ioc:
    image: adregistry.fnal.gov/instrumentation/my-device-legacy-ioc@sha256:<digest>
    environment:
      LEGACY_IOC_ENABLE_CA: "NO"
    volumes:
      - ./iocBoot/st.cmd:/etc/legacy-ioc/st.cmd:ro
```

## Publishing The Sample Sidecar Image

Maintainers only:

```sh
LEGACY_IOC_IMAGE=adregistry.fnal.gov/instrumentation/redis-pvxs-ioc-legacy-sidecar:v0.2.0 \
  docker compose \
    -f docker-compose.yml \
    -f docker-compose.legacy-sidecar.yml \
    -f docker-compose.legacy-sidecar.build.yml \
    --profile legacy \
    build legacy-ioc

LEGACY_IOC_IMAGE=adregistry.fnal.gov/instrumentation/redis-pvxs-ioc-legacy-sidecar:v0.2.0 \
  docker compose \
    -f docker-compose.yml \
    -f docker-compose.legacy-sidecar.yml \
    -f docker-compose.legacy-sidecar.build.yml \
    --profile legacy \
    push legacy-ioc
```

Project-specific sidecars should follow the same pattern: build a derived image, push it to the registry, then set `LEGACY_IOC_IMAGE` in that project's compose override. Do not make normal project startup depend on local image builds.
