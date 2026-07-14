# PVAccess Networking

`redis-pvxs-ioc` serves PVAccess only. `pvxget` can work when pointed at a
known reachable address while `pvxhosts` or `pvxlist` still fail, because host
visibility depends on UDP search/beacon traffic and on the IOC advertising a
TCP endpoint that the client can route back to.

## Container Entrypoint Defaults

The published container starts through `scripts/container-entrypoint.sh`. The
entrypoint sets explicit EPICS CA/PVA discovery defaults unless an environment
variable already exists.

For PVAccess, the important defaults are:

```sh
EPICS_PVA_AUTO_ADDR_LIST=NO
EPICS_PVA_ADDR_LIST=239.128.1.6
EPICS_PVAS_AUTO_BEACON_ADDR_LIST=NO
EPICS_PVAS_BEACON_ADDR_LIST=239.128.1.7,10
```

When `EPICS_HOST_INTERFACE` is set, the entrypoint also adds an
interface-qualified PVA search address:

```sh
EPICS_PVA_ADDR_LIST="239.128.1.6,8@${EPICS_HOST_INTERFACE} 239.128.1.6"
```

Deployment compose files should usually set `EPICS_HOST_INTERFACE` to the
container interface on the controls network and avoid overriding
`EPICS_PVA_AUTO_ADDR_LIST` back to `YES`.

## Bring an existing EPICS environment into Docker

A native `redis-pvxs-ioc` process inherits every `EPICS_*` variable exported by
its calling shell. Docker containers only receive variables that are passed
explicitly. The container entrypoint preserves any supplied `EPICS_*` value and
only fills in a project default when that variable is absent.

For an ad hoc `docker run`, forward every currently exported `EPICS_*` variable
by name. Passing `--env NAME` tells Docker to copy the value without embedding
it in shell history:

```bash
epics_env=()
while IFS= read -r name; do
  case "$name" in
    EPICS_*) epics_env+=(--env "$name") ;;
  esac
done < <(compgen -e | LC_ALL=C sort)

docker run --rm --network host \
  "${epics_env[@]}" \
  --volume "$PWD/config.yaml:/etc/redis-pvxs-ioc/config.yaml:ro" \
  adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:v0.6.0@sha256:208002466ec3cc7db31ed5061938029ed2df90242ae2ccb5b0ab7de8792fbefb
```

For a long-running Compose deployment, capture the same variables in a
temporary environment file and reference it from a deployment-owned override:

```sh
EPICS_ENV_FILE=$(mktemp)
export EPICS_ENV_FILE
env | awk -F= '$1 ~ /^EPICS_/ { print }' > "$EPICS_ENV_FILE"
docker compose -f docker-compose.yml -f compose.epics-env.yml up -d
rm -f "$EPICS_ENV_FILE"
```

```yaml
# compose.epics-env.yml
services:
  ioc:
    env_file:
      - ${EPICS_ENV_FILE}
```

Review the captured variables before deployment. In particular, client-side
`EPICS_PVA_*` search settings and server-side `EPICS_PVAS_*` interface/beacon
settings have different roles. Forwarding the host environment also does not
make a private bridge routable; use host networking or a routed container
identity when the supplied addresses refer to the controls network. The
`--network host` example is for Linux hosts.

## Default Docker Demo

The default compose stack is intentionally self-contained on a private Docker
bridge network. It is meant for smoke testing, not for discovery by tools
running on the Docker host or on another controls-network machine.

For that demo, validate from inside the IOC container:

```sh
docker exec redis-pvxs-ioc-demo sh -lc \
  'EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1 /opt/redis-pvxs-ioc/bin/pvxs/pvxget DEMO:source:temperature'
```

Host-side `pvxhosts`/`pvxlist` should not be treated as a pass/fail criterion
for the private bridge demo unless the tool is attached to the same Docker
network or the compose file explicitly publishes and advertises reachable PVA
endpoints.

## Routable Deployment Pattern

For `pvxhosts`/`pvxlist` visibility from the broader controls network, run the
IOC on a routable network identity. Known-good deployment shapes include:

- host networking, when acceptable for the host and security model
- `macvlan` or `ipvlan` with a real controls-network address
- another routed container network where clients can reach the advertised IOC
  TCP endpoint directly

For `macvlan`/`ipvlan`, use a stable hostname/IP assignment and set
`EPICS_HOST_INTERFACE` to the interface inside the container's network
namespace:

```yaml
services:
  ioc:
    image: adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:v0.6.0@sha256:208002466ec3cc7db31ed5061938029ed2df90242ae2ccb5b0ab7de8792fbefb
    hostname: redis-pvxs-ioc.example.fnal.gov
    environment:
      - EPICS_HOST_INTERFACE=eth0
    networks:
      controls:
        ipv4_address: 10.0.0.50

networks:
  controls:
    external: true
```

If the IOC shares another service's network namespace with
`network_mode: "service:<name>"`, `EPICS_HOST_INTERFACE` still refers to the
interface inside that shared namespace. In the Booster DCCT deployment, the IOC
shares the Redis service namespace on an `ipvlan` network, sets
`EPICS_HOST_INTERFACE=eth0`, and relies on the release entrypoint defaults. With
the container reachable by its controls-network hostname, a remote controls host
can see the IOC in `pvxhosts`.

Do not carry over deployment overrides such as:

```yaml
environment:
  - EPICS_PVA_AUTO_ADDR_LIST=YES
```

That bypasses the explicit search list built by the container entrypoint and can
make behavior depend on Docker's view of local interfaces instead of the
intended controls-network interface.

## Troubleshooting

Capture the effective compose and container network state first:

```sh
docker compose config
docker exec redis-pvxs-ioc-demo sh -lc 'env | sort | grep -E "^(EPICS|PVXS)_"'
docker exec redis-pvxs-ioc-demo sh -lc 'ip addr && ip route'
docker exec redis-pvxs-ioc-demo sh -lc 'ss -lunpt || netstat -lunpt'
```

Validate the data path independently of discovery:

```sh
docker exec redis-pvxs-ioc-demo sh -lc \
  'EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=127.0.0.1 /opt/redis-pvxs-ioc/bin/pvxs/pvxget DEMO:source:temperature'
```

Then validate from a separate machine on the target network:

```sh
EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=239.128.1.6 pvxhosts
EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=239.128.1.6 pvxlist -w 10 -v
EPICS_PVA_AUTO_ADDR_LIST=NO EPICS_PVA_ADDR_LIST=<ioc-hostname-or-ip> pvxget DEMO:source:temperature
```

If direct unicast reads work but host discovery does not, compare the advertised
or known server endpoint:

```sh
pvxlist -i <ioc-hostname-or-ip>:5075
```

For verbose server-side networking diagnostics, run the IOC with PVXS logging:

```sh
PVXS_LOG='pvxs.server*=DEBUG pvxs.tcp*=DEBUG pvxs.udp*=DEBUG' docker compose up ioc
```

RecCaster/RecSync and ChannelFinder are separate cataloging mechanisms. They
can publish record or PV metadata, but they do not replace PVAccess UDP
search/beacon visibility for `pvxhosts`/`pvxlist`.
