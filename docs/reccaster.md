# RecCaster Sidecar

RecCaster is included in the legacy IOC sidecar so conventional EPICS records can register with RecCeiver/ChannelFinder.

## Run

```sh
docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy up -d
```

Check the RecCaster status records:

```sh
IOC_CONTAINER=redis-pvxs-ioc-demo
PV_ENV='EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=239.128.1.6'
PVX_BIN_DIR='/opt/redis-pvxs-ioc/bin/pvxs'

docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget LEGACY:RecCaster:State-Sts"
docker exec "$IOC_CONTAINER" sh -lc "$PV_ENV $PVX_BIN_DIR/pvxget LEGACY:RecCaster:Msg-I"
```

## How It Connects

RecCaster does not know the ChannelFinder URL.

- RecCaster listens for RecCeiver UDP announcements on port `5049`.
- RecCeiver advertises its TCP host, TCP port, and server key.
- RecCaster connects to that advertised TCP endpoint and uploads records.
- RecCeiver writes to ChannelFinder when its `cf` processor is configured.

The sample compose overlay publishes UDP `5049` from the host to the legacy sidecar so it can receive RecCeiver announcements on a bridge network:

```sh
RECCASTER_UDP_HOST_PORT=5049 \
  docker compose -f docker-compose.yml -f docker-compose.legacy-sidecar.yml --profile legacy up -d
```

Use a different `RECCASTER_UDP_HOST_PORT` only when running multiple local test stacks on the same host. Production `ipvlan` or directly routed container networking may not need host port publishing.

RecCeiver-side settings are the important network/catalog settings:

```ini
[recceiver]
addrlist = <ioc-broadcast-or-multicast-target>:5049
bind = <recceiver-listen-ip>:<stable-tcp-port>

[cf]
baseUrl = https://<channel-finder-host>/ChannelFinder
cfUsername = <service-user>
cfPassword = <secret>
verifySSL = True
```

## IOC-Side Settings

The sample sidecar sets these defaults before startup:

```sh
IOCNAME=legacy-ioc
ENGINEER=redis-pvxs-ioc
LOCATION=redis-pvxs-ioc-demo
CONTACT=redis-pvxs-ioc
BUILDING=demo
SECTOR=demo
RECCAST_TIMEOUT=20
RECCAST_MAX_HOLDOFF=10
```

Override them as normal compose environment variables. `CONTACT`, `BUILDING`, and `SECTOR` are sent with each RecCaster upload.

## Upstream Source and Fermilab Delta

RecCaster is pinned from the standalone public
[`ChannelFinder/reccaster`](https://github.com/ChannelFinder/reccaster)
repository. Upstream moved it from `ChannelFinder/recsync/client` in June 2026
and subsequently removed that old source tree.

The local Fermilab `RecCaster-1.8` tree was originally compared with the public
`ChannelFinder/recsync` client tag `1.9.2`. The pinned `1.9.2` client source was
then compared with standalone RecCaster commit
`254723063a2b7a7c80d8e50f05efa8d748f429dd` during the repository migration.

Observed differences are build/layout only:

- Fermilab's top-level `Makefile` disables upstream demo/iocBoot dirs.
- Fermilab's `configure/RELEASE` adjusts local include depth for
  `RELEASE.local`.
- The standalone upstream commit updates documentation and
  `configure/RELEASE` include paths for its new repository root.

No C source, DBD, database, or protocol differences were observed. This project
tracks public RecCaster directly and should only add a patch layer if a real
source delta appears later.
