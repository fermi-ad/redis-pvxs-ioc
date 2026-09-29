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

Credentials come from `CHANNELFINDER_USERNAME` and `CHANNELFINDER_PASSWORD`, or
their `_FILE` alternatives. For example, set `CHANNELFINDER_PASSWORD_FILE` to a
mounted secret's path inside the container. Nonempty inline and file inputs for
the same credential are mutually exclusive; an empty inline environment variable
counts as unset for Compose compatibility. Username and password must still be
supplied together. File inputs use the same bounded text-file rules as Redis
credentials. They are not loaded during `--dry-run`, since it does not authenticate.
See [Container runtime and secret files](container-runtime.md) for mounts and UID
permissions. Credential files retain the same prohibition on authenticated redirects.

The publisher uses a 3-second connection timeout, a 10-second total request
deadline, and a 1 MiB response limit. Override them with
`--connect-timeout-ms`, `--timeout-ms`, and `--max-response-bytes`. Timeouts must
be positive and at most 300000 ms; the response cap must be positive and at most
64 MiB. A shorter total deadline also bounds connection establishment.

Redirects are disabled by default. `--allow-redirects` permits at most three
HTTPS redirects for unauthenticated publication and preserves the POST method
and JSON payload. Authenticated publication must use the final URL; combining
credentials and redirects is rejected before sending a request. URL-embedded
credentials and non-HTTP(S) protocols are rejected. TLS verification remains
enabled. Non-2xx responses report their status without echoing server bodies.

`--dry-run` remains offline and does not contact either ChannelFinder or Redis.

## Published Fields

Each canonical Redis PV and each of its configured aliases becomes one
ChannelFinder channel. Alias channels carry the same metadata and routes plus
`aliasOf=<canonical resolved PVA name>`.

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
