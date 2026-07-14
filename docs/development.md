# Development Guide

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

## Native prerequisites

Native builds support Linux and macOS. They require:

- CMake 3.20 or newer and a C++17 compiler;
- GNU Make, Perl, Git, and pkg-config;
- libcurl and readline development files;
- gRPC C++ and Protocol Buffers development libraries plus `protoc` and the
  gRPC C++ plugin;
- the pinned EPICS Base, PVXS, redis-adapter, and yaml-cpp submodules.

On Ubuntu 24.04, the direct package set is:

```sh
sudo apt-get install \
  build-essential ca-certificates cmake git libcurl4-openssl-dev \
  libreadline-dev libgrpc++-dev libprotobuf-dev perl pkg-config \
  protobuf-compiler protobuf-compiler-grpc
```

gRPC and Protocol Buffers are mandatory build dependencies even when a deployment
does not configure `rpc_services`.

## Build pinned EPICS dependencies

```sh
git submodule update --init --recursive
make -C third_party/epics-base configure.install src.install -j"$(nproc)"
make -C third_party/epics-base/modules libcom.install -j"$(nproc)"
printf 'EPICS_BASE=%s/third_party/epics-base\n' "$(pwd)" \
  > third_party/pvxs/configure/RELEASE.local
make -C third_party/pvxs/bundle libevent
make -C third_party/pvxs configure.install setup.install src.install tools.install \
  -j"$(nproc)"
```

The service needs EPICS `libCom` plus the standalone PVXS library/tools; the main
runtime does not require an EPICS database or `pvxsIoc`.

## Build and test

```sh
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/redis-pvxs-ioc --version
./build/redis-pvxs-ioc --check-config demo/config.yaml
```

If an existing build directory predates a `VERSION` change, rerun CMake before
checking `--version`.

The legacy sidecar has a separate build overlay and dependency surface. See
[`legacy-sidecar.md`](legacy-sidecar.md); it is not required for core changes.
