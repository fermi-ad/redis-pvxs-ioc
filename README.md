# redis-pvxs-ioc

`redis-pvxs-ioc` is a standalone PVAccess-only service that serves Redis-backed PVs from structured YAML config and reloads configuration without restarting the process.

## MVP scope

- PVA only
- Redis-backed reads and writes against a standalone single-node Redis instance
- `NTScalar` / `NTScalarArray` payloads with standard Phoebus-facing metadata
- manual hot reload via `SIGHUP` and admin PV
- Redis alarm stream publishing
- linear transforms for floating-point PVs
- no CA, no embedded record/database host, no Redis Cluster support, and no ACF in the first cut

## Repository layout

- [`docs/design.md`](/Users/derekste/Dev/epics/redis-pvxs-ioc/docs/design.md) is the baseline design artifact.
- [`docs/mvp-spec.md`](/Users/derekste/Dev/epics/redis-pvxs-ioc/docs/mvp-spec.md) is the implementation contract.
- [`docs/submodule-remotes.md`](/Users/derekste/Dev/epics/redis-pvxs-ioc/docs/submodule-remotes.md) lists the submodules that still need published remotes before the repo is pushed outside this workspace.
- [`demo/config.yaml`](/Users/derekste/Dev/epics/redis-pvxs-ioc/demo/config.yaml) is the sample runtime configuration.
- [`scripts/smoke-test.sh`](/Users/derekste/Dev/epics/redis-pvxs-ioc/scripts/smoke-test.sh) exercises the container demo.

## Dependency bootstrap

Populate the pinned submodules first:

```sh
git submodule update --init --recursive
```

`.gitmodules` points at published remotes for `epics-base`, `pvxs`, `redis-adapter`, and `yaml-cpp`.
See [`docs/submodule-remotes.md`](/Users/derekste/Dev/epics/redis-pvxs-ioc/docs/submodule-remotes.md) for the pinned SHAs, fork branch details, and the plan to relink `epics-base` and `pvxs` back to upstream after their fork changes are merged.

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
docker compose up
```

The runtime container reads `/etc/redis-pvxs-ioc/config.yaml` by default.
This default compose stack is intentionally self-contained on a local bridge network for smoke/demo testing. Deployment networking can move to dedicated IPs or `ipvlan` later.
For `docker exec` validation, use the packaged PVXS tools under `/opt/redis-pvxs-ioc/bin/pvxs/`.

Transforms are internal to the server. YAML `transform` settings describe how raw Redis values are mapped to the served PVA `value`; display, control, units, and alarm thresholds are configured in served units.
