# redis-pvxs-ioc

[![Validate image](https://github.com/fermi-ad/redis-pvxs-ioc/actions/workflows/ci-image.yml/badge.svg)](https://github.com/fermi-ad/redis-pvxs-ioc/actions/workflows/ci-image.yml)
[![Latest release](https://img.shields.io/github/v/release/fermi-ad/redis-pvxs-ioc)](https://github.com/fermi-ad/redis-pvxs-ioc/releases/latest)
[![License](https://img.shields.io/badge/license-BSD--3--Clause-blue.svg)](LICENSE)

`redis-pvxs-ioc` is a standalone PVAccess service for hot-reloadable,
Redis-backed EPICS process variables defined by YAML.

## Key features

- **Generation-based hot reload:** reload through `SIGHUP` or a PVA command;
  invalid replacements are rejected while the active generation keeps serving.
- **Built-in diagnostics:** always-on PVA endpoints report version, source
  revision, config generation/status/error, configured PV count, and Redis
  backend health.
- **Typed Redis routes:** scalar and array reads, writes, and confirmed readback
  across one or more standalone Redis backends.
- **Operator-ready PVA values:** `NTScalar` and `NTScalarArray` values include
  alarm, timestamp, display, control, units, precision, and limit metadata.
- **Alarms and transforms:** scalar threshold evaluation, Redis alarm-stream
  publication, and linear floating-point scale/offset transforms.
- **Integrations:** one-shot ChannelFinder publishing and reflection-based PVA
  RPC to gRPC forwarding.
- **Container tooling:** released images include `pvxget`, `pvxput`, and other
  PVXS tools for validation and troubleshooting.

The core runtime is PVA-only. It does not load EPICS databases, call `iocInit()`,
or serve Channel Access.

## Quick start

The registry images allow anonymous pulls:

```sh
git clone https://github.com/fermi-ad/redis-pvxs-ioc.git
cd redis-pvxs-ioc
docker compose pull
docker compose up -d
```

Validate the demo from inside the IOC container:

```sh
PV_ENV='EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1'
PVX=/opt/redis-pvxs-ioc/bin/pvxs

docker exec redis-pvxs-ioc-demo sh -lc \
  "$PV_ENV $PVX/pvxget SYS:demo:backend:health"
docker exec redis-pvxs-ioc-demo sh -lc \
  "$PV_ENV $PVX/pvxget DEMO:source:temperature"
docker exec redis-pvxs-ioc-demo sh -lc \
  "$PV_ENV $PVX/pvxput DEMO:magnet:current 9.0"
```

Stop the demo with `docker compose down`.

The default stack uses a private Docker bridge and is intended for local
validation. See [PVAccess networking](docs/pva-networking.md) before deploying
on a controls network.

## Built-in operational PVs

These PVs are always created from `server.instance`; they do not use
`server.namespace`.

| PV | Access | Purpose |
| --- | --- | --- |
| `<instance>:version` | read | Release version |
| `<instance>:revision` | read | Embedded Git revision |
| `SYS:<instance>:version` | read | Version alias |
| `SYS:<instance>:revision` | read | Revision alias |
| `SYS:<instance>:config:reload` | write | Request a config reload |
| `SYS:<instance>:config:generation` | read | Active config generation |
| `SYS:<instance>:config:lastStatus` | read | Last config/app status |
| `SYS:<instance>:config:lastError` | read | Last config/app error |
| `SYS:<instance>:stats:pvCount` | read | Configured runtime and RPC PV count |
| `SYS:<instance>:backend:health` | read | Connected Redis backend summary |

See [Operations and diagnostics](docs/operations.md) for reload guarantees and
verification commands.

## Configure and deploy

The released image reads `/etc/redis-pvxs-ioc/config.yaml`. Start with
[`demo/config.yaml`](demo/config.yaml), validate it before serving, and mount the
deployment-owned file read-only:

```sh
REDIS_PVXS_IOC_CONFIG=/absolute/path/to/config.yaml \
  docker compose run --rm --no-deps ioc \
  --check-config /etc/redis-pvxs-ioc/config.yaml

REDIS_PVXS_IOC_CONFIG=/absolute/path/to/config.yaml docker compose up -d
```

Production deployments should consume the published image by semver tag and
immutable digest. `:latest` is only a development convenience. See the
[Redis-only adoption guide](docs/redis-only-quickstart.md) and complete
[configuration reference](docs/configuration.md).

## Documentation

- [Documentation index](docs/README.md)
- [Configuration reference](docs/configuration.md)
- [Operations and diagnostics](docs/operations.md)
- [Architecture](docs/design.md)
- [Current feature state and roadmap](docs/feature-state-roadmap.md)
- [Building from source](docs/building-from-source.md)
- [Contributor development guide](docs/development.md)
- [Release process](docs/releasing.md)

## Optional legacy sidecar

An independently versioned conventional IOC sidecar is available for `.db`
records, RecCaster, and selected support modules. It is experimental and is not
part of the core quick start while its long-term product boundary is decided in
[issue #68](https://github.com/fermi-ad/redis-pvxs-ioc/issues/68). See the
[legacy sidecar guide](docs/legacy-sidecar.md) for the current image and limits.

## License

Project-authored code is available under the [BSD 3-Clause License](LICENSE).
The contract and government-rights terms are retained in [NOTICE](NOTICE). See
[Third-Party Notices](THIRD_PARTY_NOTICES.md) for bundled dependencies and their
license locations.
