# redis-pvxs-ioc

`redis-pvxs-ioc` is a standalone PVAccess-only service that serves Redis-backed PVs from structured YAML config and reloads configuration without restarting the process.

For the latest released version, see the repository's tags/releases.

The published container image starts through [`scripts/container-entrypoint.sh`](scripts/container-entrypoint.sh), which exports the standard EPICS CA/PVA multicast network defaults before launching the IOC. Every setting can still be overridden with an explicit container environment variable.

## Quick start

Registry-first startup:

```sh
git clone https://github.com/fermi-ad/redis-pvxs-ioc.git
cd redis-pvxs-ioc
docker compose pull
docker compose up -d
```

Add the legacy IOC sidecar:

```sh
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy pull
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy up -d
```

Validate:

```sh
docker exec redis-pvxs-ioc-demo sh -lc \
  'EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1 /opt/redis-pvxs-ioc/bin/pvxs/pvxget DEMO:source:temperature'
docker exec redis-pvxs-ioc-demo sh -lc \
  'EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=239.128.1.6 /opt/redis-pvxs-ioc/bin/pvxs/pvxget LEGACY:readback'
```

See [`docs/redis-only-quickstart.md`](docs/redis-only-quickstart.md) for the fastest Redis-only project adoption path, [`docs/demo.md`](docs/demo.md) for the complete validation flow, and [`docs/legacy-sidecar.md`](docs/legacy-sidecar.md) for the support-module sidecar adoption path.

## What works today

`redis-pvxs-ioc` gives a fresh clone a working PVA testbed from registry images only:

- a Redis-backed PVA IOC serving configured PVs
- a Redis container for the demo value plane
- packaged `pvxget` / `pvxput` tools inside the IOC image
- an optional conventional IOC sidecar for base-record `.db` examples and support-module adoption
- optional ChannelFinder catalog sync for Redis-backed PV definitions

The main runtime serves `NTScalar` and `NTScalarArray` PVs with the Phoebus-facing fields operators expect: `value`, `alarm`, `timeStamp`, `display`, `control`, and `valueAlarm`. PVs can read from Redis, write to Redis, confirm readback, publish alarm transitions to a Redis stream, and reload YAML config without restarting the process.

## Runtime capabilities

- Redis-backed reads, writes, and confirm/readback routes
- one or more standalone Redis backends in the same config
- scalar and array PVs over PVAccess
- units, description, precision, display form, display/control limits, and alarm limits
- scalar alarm evaluation with `epics-alarm-push`-compatible Redis alarm-stream publishing
- linear transforms for floating-point PVs and arrays
- manual hot reload via `SIGHUP` and admin PVs
- always-on read-only `<server.instance>:version` PV with the running release version
- standard EPICS/PVA network defaults at container startup, with environment overrides
- one-shot ChannelFinder dry-run/publish tooling for Redis PV definitions

## Legacy compatibility

Legacy IOC support is handled as a sidecar, not by loading `.dbd` files into the Redis-first runtime. The included sidecar image runs a small conventional IOC with `pvxsIoc` and RecCaster, so users can see base records over PVA and advertise conventional records through RecCeiver/ChannelFinder.

Source-built sidecar images and release images `v0.5.0` and later ship a prelinked controls compatibility bundle: `seq`, `sscan`, `calc`, `asyn`, `std`, `pcre`, `StreamDevice`, `lua`, `iocStats`, `alive`, `autosave`, `busy`, `caPutLog`, `linStat`, RecCaster, `tcast`, and `acnetPV`. `linStat`, `tcast`, `acnetPV`, and CA behavior are inactive by default; enable them only through an explicit sidecar startup script/environment.

Teams that need modules outside this bundle should derive their own sidecar image, link the required module code there, publish it to the registry, and run it next to `redis-pvxs-ioc` on the same PVA network. That keeps support-module behavior available without compromising the hot-reload Redis runtime.

## Current boundaries

- The core product is PVA-first; CA is not served by the main runtime.
- ACF enforcement and Redis-native PV definitions/settings are planned, not implemented yet.
- Redis Cluster and in-process EPICS database hosting are intentionally out of scope.

The detailed product/roadmap state is tracked in [`docs/feature-state-roadmap.md`](docs/feature-state-roadmap.md). The normative-types expansion target is tracked separately in [`docs/normative-types-roadmap.md`](docs/normative-types-roadmap.md).

## Repository layout

- [`docs/design.md`](docs/design.md) is the baseline design artifact.
- [`docs/feature-state-roadmap.md`](docs/feature-state-roadmap.md) captures the current feature state and the post-MVP capability tracks.
- [`docs/mvp-spec.md`](docs/mvp-spec.md) is the implementation contract.
- [`docs/normative-types-roadmap.md`](docs/normative-types-roadmap.md) breaks down the long-term EPICS normative-types coverage goal.
- [`docs/channelfinder-sync.md`](docs/channelfinder-sync.md) documents Redis PV catalog publishing.
- [`docs/redis-only-quickstart.md`](docs/redis-only-quickstart.md) is the short project adoption path for Redis-backed PVAccess-only deployments.
- [`docs/pva-networking.md`](docs/pva-networking.md) documents PVAccess discovery expectations for Docker bridge demos and routable deployments.
- [`docs/reccaster.md`](docs/reccaster.md) documents RecCaster in the legacy sidecar.
- [`docs/submodule-remotes.md`](docs/submodule-remotes.md) lists the submodules that still need published remotes before the repo is pushed outside this workspace.
- [`demo/config.yaml`](demo/config.yaml) is the legacy single-backend sample runtime configuration.
- [`demo/config.multi.yaml`](demo/config.multi.yaml) is the sample multi-backend runtime configuration.
- [`docs/legacy-sidecar.md`](docs/legacy-sidecar.md) explains the optional conventional IOC sidecar for legacy `.dbd` / `.db` workflows.
- [`scripts/smoke-test.sh`](scripts/smoke-test.sh) exercises the container demo.
- [`docs/releasing.md`](docs/releasing.md) is the manual semver release procedure.

## Config model

- Legacy single-backend configs still use top-level `redis`.
- Multi-backend configs use top-level `redis_backends` keyed by backend alias.
- `read.backend`, `write.backend`, `confirm.backend`, and `alarms.backend` select which Redis backend each operation uses.
- When multiple backends are configured, every PV route must resolve to an explicit backend alias.
- When multiple backends are configured, set `alarms.backend` explicitly as well. The runtime does not choose a default alarm-stream backend when more than one backend is present.

## Dependency bootstrap

Populate the pinned submodules first:

```sh
git submodule update --init --recursive
```

`.gitmodules` points at published remotes for `epics-base`, `pvxs`, `redis-adapter`, `yaml-cpp`, `recsync`, and the public support-module bundle used by the legacy sidecar.
See [`docs/submodule-remotes.md`](docs/submodule-remotes.md) for the pinned SHAs, fork branch details, and the plan to relink `epics-base` and `pvxs` back to upstream after their fork changes are merged.

Build the EPICS stack in-place:

```sh
make -C third_party/epics-base configure.install src.install -j"$(nproc)"
make -C third_party/epics-base/modules libcom.install -j"$(nproc)"
printf 'EPICS_BASE=%s/third_party/epics-base\n' "$(pwd)" > third_party/pvxs/configure/RELEASE.local
make -C third_party/pvxs/bundle libevent
make -C third_party/pvxs configure.install setup.install src.install tools.install -j"$(nproc)"
```

This service only needs `libCom` from `epics-base` and the standalone `pvxs` library/tools build; it does not need the full CA/database or `pvxsIoc` build.

Then build the service:

```sh
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/redis-pvxs-ioc --version
```

## Local run

Validate config only:

```sh
./build/redis-pvxs-ioc --check-config demo/config.yaml
```

Run the service:

```sh
./build/redis-pvxs-ioc --config demo/config.yaml
```

Reload the active config:

```sh
kill -HUP "$(pgrep -f redis-pvxs-ioc)"
```

## Container demo

```sh
docker compose pull
docker compose up -d
docker compose logs -f
```

The runtime container reads `/etc/redis-pvxs-ioc/config.yaml` by default.
This default compose stack is intentionally self-contained on a local bridge network for smoke/demo testing. Host-side `pvxhosts`/`pvxlist` discovery is not expected from that private bridge network; use routable `ipvlan`/`macvlan`/host networking for controls-network visibility.
For `docker exec` validation, use the packaged PVXS tools under `/opt/redis-pvxs-ioc/bin/pvxs/`.
See [`docs/pva-networking.md`](docs/pva-networking.md) for the deployment pattern and troubleshooting commands.
Stop the demo stack with `docker compose down`.

To test a specific published image or alternate config without editing tracked files:

```sh
REDIS_PVXS_IOC_IMAGE=adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:vX.Y.Z@sha256:<digest> \
REDIS_PVXS_IOC_CONFIG=/absolute/path/to/config.yaml \
docker compose up -d
```

If you intentionally want the moving convenience tag instead of the default release digest, override `REDIS_PVXS_IOC_IMAGE` with `adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:latest`.

The runtime image is built on `ubuntu:24.04` and includes `iproute2` and `iputils-ping` for basic network diagnostics.

Run the registry-only smoke test with:

```sh
./scripts/smoke-test.sh
```

Transforms are internal to the server. YAML `transform` settings describe how raw Redis values are mapped to the served PVA `value`; display, control, units, and alarm thresholds are configured in served units.

## ChannelFinder sync

Preview ChannelFinder entries for Redis-backed PVs:

```sh
docker compose \
  -f docker-compose.yml \
  -f docker-compose.channelfinder-sync.yml \
  --profile channelfinder \
  run --rm channelfinder-sync
```

See [`docs/channelfinder-sync.md`](docs/channelfinder-sync.md) for publish commands and config.

## Legacy IOC sidecar

The optional legacy sidecar is a conventional EPICS IOC container that can run beside `redis-pvxs-ioc` for base-record `.db` files and user-owned support-module images. It does not add `.dbd`, `.db`, or `iocInit()` behavior to the main `redis-pvxs-ioc` process.

The sample sidecar is also registry-backed, so a fresh clone can start the complete testbed without local image builds:

```sh
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy pull
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy up -d
```

The sidecar overlay publishes UDP `5049` by default so RecCaster can receive RecCeiver announcements on a bridge-network demo stack. Override `RECCASTER_UDP_HOST_PORT` if another local stack already owns that port.

See [`docs/legacy-sidecar.md`](docs/legacy-sidecar.md) for the sample sidecar image, compose overlay, and instructions for deriving a real support-module sidecar. See [`docs/reccaster.md`](docs/reccaster.md) for RecCaster/RecCeiver details.
