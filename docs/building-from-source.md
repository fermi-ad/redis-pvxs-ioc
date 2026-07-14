# Building from Source

The release image is the shortest production path. A native source build is
useful for development, debugging, and installations that already maintain
EPICS Base and PVXS. Linux and macOS are supported.

## Clone the complete source tree

The project pins its build dependencies as Git submodules. Clone recursively;
all configured remotes are public HTTPS URLs.

```sh
git clone --recurse-submodules \
  https://github.com/fermi-ad/redis-pvxs-ioc.git
cd redis-pvxs-ioc
```

For an existing checkout:

```sh
git submodule sync --recursive
git submodule update --init --recursive
```

## Prerequisites

Required tools and libraries are:

- CMake 3.20 or newer and a C++17 compiler;
- GNU Make, Perl, Git, and pkg-config;
- libcurl and readline development files;
- gRPC C++ and Protocol Buffers development libraries;
- `protoc` and the gRPC C++ code-generation plugin.

gRPC and Protocol Buffers are mandatory build dependencies even when the
runtime configuration has no `rpc_services`.

Ubuntu 24.04:

```sh
sudo apt-get update
sudo apt-get install \
  build-essential ca-certificates cmake git libcurl4-openssl-dev \
  libreadline-dev libgrpc++-dev libprotobuf-dev perl pkg-config \
  protobuf-compiler protobuf-compiler-grpc
```

macOS with Homebrew:

```sh
brew install cmake curl grpc protobuf readline pkg-config
```

## Build the pinned EPICS dependencies

Choose a parallel job count appropriate for the machine:

```sh
JOBS=${JOBS:-4}

make -C third_party/epics-base configure.install src.install -j"$JOBS"
make -C third_party/epics-base/modules libcom.install -j"$JOBS"

printf 'EPICS_BASE=%s/third_party/epics-base\n' "$(pwd)" \
  > third_party/pvxs/configure/RELEASE.local
make -C third_party/pvxs/bundle libevent
make -C third_party/pvxs configure.install setup.install src.install \
  tools.install -j"$JOBS"
```

The main service links EPICS `libCom` and standalone PVXS. It does not require
an EPICS database, `pvxsIoc`, or `iocInit()`.

## Build and test the service

```sh
JOBS=${JOBS:-4}
cmake -S . -B build
cmake --build build -j"$JOBS"
ctest --test-dir build --output-on-failure

./build/redis-pvxs-ioc --version
./build/redis-pvxs-ioc --check-config demo/config.yaml
```

On Homebrew installations where CMake cannot locate gRPC or Protobuf, provide
their package prefixes explicitly:

```sh
cmake -S . -B build \
  -DCMAKE_PREFIX_PATH="$(brew --prefix grpc);$(brew --prefix protobuf)"
```

Rerun CMake after changing `VERSION`; it regenerates the embedded version
header. Use `-DREDIS_PVXS_IOC_BUILD_TESTS=OFF` only for a build where the test
binaries are deliberately not wanted.

## Use existing EPICS Base and PVXS builds

The in-tree revisions are the reproducible default, but CMake can link an
already-built EPICS Base and PVXS tree. The two installations must use a
compatible compiler, target architecture, and shared-library ABI.

```sh
export EPICS_BASE=/opt/epics/base
export PVXS_ROOT=/opt/epics/pvxs

cmake -S . -B build \
  -DEPICS_BASE="$EPICS_BASE" \
  -DPVXS_ROOT="$PVXS_ROOT"
cmake --build build -j"$JOBS"
ctest --test-dir build --output-on-failure
```

`PVXS_ROOT` is the PVXS source/install tree layout used by its EPICS Make
build: it must contain `include/`, `lib/<EPICS_HOST_ARCH>/`, and, on Linux,
`bundle/usr/<EPICS_HOST_ARCH>/lib/libevent`. The redis-adapter and yaml-cpp
submodules are still built from this repository.

If the loader cannot find the native shared libraries, add the corresponding
architecture directories before running the binaries:

```sh
EPICS_HOST_ARCH="$(perl "$EPICS_BASE/lib/perl/EpicsHostArch.pl")"
export LD_LIBRARY_PATH="$EPICS_BASE/lib/$EPICS_HOST_ARCH:$PVXS_ROOT/lib/$EPICS_HOST_ARCH:$PVXS_ROOT/bundle/usr/$EPICS_HOST_ARCH/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

On macOS, set `DYLD_LIBRARY_PATH` to the same colon-separated directories
instead.

## Build the container locally

The multi-stage container build compiles the pinned dependencies, service,
tools, and tests in the same Ubuntu environment used by public CI:

```sh
docker build --platform linux/amd64 -t redis-pvxs-ioc:local .
docker run --rm redis-pvxs-ioc:local --version
docker run --rm redis-pvxs-ioc:local \
  --check-config /etc/redis-pvxs-ioc/config.yaml
REDIS_PVXS_IOC_IMAGE=redis-pvxs-ioc:local ./scripts/smoke-test.sh
```

The optional legacy sidecar has a separate dependency surface and build
overlay. See [Legacy IOC sidecar](legacy-sidecar.md).
