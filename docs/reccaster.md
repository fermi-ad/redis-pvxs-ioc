# Discovery

Discovery is part of the core service. PVA clients find and connect to PVs using
normal PVXS discovery. A native RecCaster-compatible client also registers the
active catalog with RecCeiver, whose ChannelFinder processor maintains the
searchable catalog. No sidecar, EPICS database, `iocInit()`, CA facade, manual
upload, or IOC-side ChannelFinder credentials are needed.

This implementation targets v0.9.0. Published v0.8 images retain their previous
behavior until upgraded to a validated release.

## Automatic registration

Registration is enabled by default and listens for RecCeiver announcements on
UDP 5049. RecCeiver supplies its TCP endpoint and server key. The service uploads
canonical PVs, their aliases, built-in diagnostics, and successfully reflected
RPC endpoints. The `IOCNAME` is `server.instance`, and `PVAS_SERVER_PORT` is the
actual bound PVA TCP port, including when an ephemeral port was requested.
`HOSTNAME` defaults to the system hostname; the environment can override it.
`ENGINEER`, `LOCATION`, `CONTACT`, `BUILDING`, and `SECTOR` are included when set.
No CA port is advertised.

On each successful configuration reload, a new immutable catalog replaces the
previous session. RecCeiver reconciles additions, removals, aliases, and changed
metadata. Failed staging preserves the active catalog. A receiver restart
triggers another full upload automatically. The direct
[ChannelFinder tool](channelfinder-sync.md) remains available for explicit
one-shot publication; it is not needed for automatic discovery.

## Network setup

Routed or host-network containers must receive UDP 5049 and reach the TCP
endpoint advertised by RecCeiver. A bridge-network container needs a UDP mapping:

The supplied Compose stack already publishes UDP 5049 for the core service;
`RECCASTER_UDP_HOST_PORT` overrides the host port. Custom bridge deployments use:

```yaml
services:
  ioc:
    ports:
      - "5049:5049/udp"
```

Use a routed container address or a distinct configured port/announcement target
for multiple services on one host. PVA clients also need the PVA routes described
in [PVAccess networking](pva-networking.md); a catalog entry alone does not make
a private container address reachable.

Existing RecCeiver installations use the same protocol. A typical receiver
configuration is:

```ini
[recceiver]
addrlist = <service-broadcast-or-routed-address>:5049
bind = <receiver-address>:<stable-tcp-port>
procs = cf

[cf]
baseUrl = https://<catalog-host>/ChannelFinder
cfUsername = <service-user>
cfPassword = <secret>
verifySSL = True
alias = True
iocConnectionInfo = True
recordType = True
recordDesc = True
infotags = protocol units type shape
environment_vars = PVXS_PROTOCOL:protocol
```

Retain existing site-specific receiver properties in `environment_vars` and
`infotags`. RecCeiver owns catalog authorization and downstream delivery.
RecCaster announcements and uploads use the existing unauthenticated protocol
and belong on the trusted controls network. ACF still controls PVA operations;
PV names remain discoverable when their values require authorization.

## Status and failure behavior

Read `SYS:<instance>:discovery:status` for structured fields:

- `state`: idle, listening, connecting, uploading, uploaded, synchronized, error,
  or disabled;
- `peer`, `port`, and bounded `lastError`;
- desired and synchronized config generations;
- catalog records, aliases and encoded bytes;
- uploads, failures, coalesced replacements, and invalid announcements.

`uploaded` means the complete catalog was sent. `synchronized` additionally
means the RecCeiver heartbeat exchange is working. Neither is a downstream
ChannelFinder transaction acknowledgement; use the receiver's logs and catalog
for that delivery status. Counters are process-local.

Network operations run on one worker with a total connect/greeting/upload
budget. Heartbeat receive waits are bounded at four times that budget. Reload
and shutdown interrupt socket waits immediately. A stalled receiver never
blocks a PVA callback. The newest catalog supersedes older queued replacements;
there is no unbounded queue of generations. Catalog length/count limits and
protocol field limits are checked before activation. See the
[configuration reference](configuration.md#discovery) for overrides.

## Acceptance

`discovery_e2e` exercises the real IOC with a protocol receiver and verifies PVA
lookups, aliases, rejected reloads, replacement, reconnect, malformed control
messages, and bounded shutdown. `discovery_tests` covers catalog count/byte
limits, invalid names, and coalesced replacements.

`tests/recceiver_acceptance.py` uses upstream RecCeiver commit
`864b162cb05fa140e67e056bda86ed750447c3d4` and its unmodified ChannelFinder
processor with an in-memory client. It verifies actual protocol parsing,
property translation, alias retirement, and receiver restart. Hosted native CI
runs this acceptance without a production catalog. Release qualification also
requires the isolated deployed ChannelFinder/PVA discovery exercise before the
legacy sidecar retirement is promoted.

For the HTTP catalog exercise, start the pinned, resource-limited fixture in
`tests/discovery-catalog.compose.yml`. Its only published port binds to loopback,
and its demo credentials apply only to the fixture. With a built native service,
the pinned receiver source above, and a virtual environment containing
`tests/recceiver-requirements.txt`:

```sh
docker compose -f tests/discovery-catalog.compose.yml up -d --wait
CATALOG_ADDRESS=$(docker compose -f tests/discovery-catalog.compose.yml port channelfinder 8080)
EPICS_HOST_ARCH=$(perl third_party/epics-base/lib/perl/EpicsHostArch.pl)
build/recceiver-venv/bin/python scripts/run-with-redis.py --redis-server "$(command -v redis-server)" -- \
  build/recceiver-venv/bin/python tests/recceiver_acceptance.py \
  --source build/recceiver-source/server --ioc build/redis-pvxs-ioc \
  --pvxget "third_party/pvxs/bin/$EPICS_HOST_ARCH/pvxget" \
  --cf-url "http://$CATALOG_ADDRESS/ChannelFinder"
docker compose -f tests/discovery-catalog.compose.yml down -v
```

The test reads the registered `iocIP` and `pvaPort` back from the catalog and
uses those fields to resolve and GET aliases with UDP search disabled. It
repeats this check after reload and receiver restart.
