# Development Guide

For a complete native setup, including package prerequisites, pinned dependency
bootstrap, existing EPICS/PVXS installations, and loader paths, see
[Building from source](building-from-source.md).

## Recommended container build

The Docker build is the most reproducible contributor path. It builds all pinned
dependencies, the service, tools, and tests:

```sh
git submodule update --init --recursive
docker build --platform linux/amd64 -t redis-pvxs-ioc:local .
docker run --rm redis-pvxs-ioc:local --version
docker run --rm redis-pvxs-ioc:local \
  --check-config /etc/redis-pvxs-ioc/config.yaml
REDIS_PVXS_IOC_IMAGE=redis-pvxs-ioc:local ./scripts/smoke-test.sh
```

## Native contributor loop

After completing the source-build bootstrap, the normal edit/test cycle is:

```sh
cmake --build build -j"${JOBS:-4}"
ctest --test-dir build --output-on-failure
./build/redis-pvxs-ioc --check-config demo/config.yaml
```

Core checkout, builds and validation use only EPICS Base, PVXS, redis-adapter
and yaml-cpp. Native RecCeiver discovery is implemented in this service.
