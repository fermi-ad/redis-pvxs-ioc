# ChannelFinder Sync

`redis-pvxs-channelfinder-sync` publishes Redis-backed PV definitions to ChannelFinder. It does not serve values and it is not part of the hot IOC runtime path.

## Dry Run

```sh
docker compose \
  -f docker-compose.yml \
  -f docker-compose.channelfinder-sync.yml \
  --profile channelfinder \
  run --rm channelfinder-sync
```

The default compose command is `--dry-run`, so it prints the ChannelFinder JSON and does not publish.

## Publish

Add ChannelFinder settings to your IOC config:

```yaml
channelfinder:
  url: https://channelfinder.example.com/ChannelFinder
  owner: redis-pvxs-ioc
  tags:
    - pva
  properties:
    facility: fnal
```

Run the one-shot publisher:

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

Credentials come only from `CHANNELFINDER_USERNAME` and `CHANNELFINDER_PASSWORD`.

## Published Fields

Each Redis PV becomes one ChannelFinder channel.

Properties include:

```text
source=redis-pvxs-ioc
protocol=pva
iocName=<server.instance>
namespace=<server.namespace>
pvStatus=Active
time=<sync timestamp>
pvaPort=<server.tcp_port or 5075>
type=<pv type>
shape=<scalar or array>
description=<metadata.description>
units=<metadata.units>
precision=<metadata.precision, when configured>
redisBackend=<read backend>
redisReadKey=<read key>
redisWriteKey=<write key, when configured>
redisConfirmKey=<confirm key, when configured>
```

Configured `channelfinder.properties` are added to each channel and can override default property values by name.

`recordType` is intentionally not published for Redis-backed PVs because they are not EPICS database records.
