FROM ubuntu:24.04 AS builder

ARG REDIS_PVXS_IOC_VERSION=dev
ARG REDIS_PVXS_IOC_REVISION=unknown
ARG REDIS_PVXS_IOC_SOURCE=https://github.com/fermi-ad/redis-pvxs-ioc

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    libcurl4-openssl-dev \
    libreadline-dev \
    perl \
    pkg-config \
    # gRPC + protobuf for the RPC->gRPC forwarding feature (find_package CONFIG)
    libgrpc++-dev \
    libprotobuf-dev \
    protobuf-compiler \
    protobuf-compiler-grpc \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/redis-pvxs-ioc

# Build the heavy third-party deps (EPICS base + pvxs) from the submodule tree
# FIRST, so that editing the IOC sources (src/, include/, proto/, CMakeLists)
# does not invalidate these layers. Only VERSION + third_party are needed here.
COPY VERSION ./
COPY third_party/ third_party/

RUN VERSION_FROM_FILE="$(tr -d '\n' < VERSION)" && \
    if [ "${REDIS_PVXS_IOC_VERSION}" != "dev" ] && [ "${REDIS_PVXS_IOC_VERSION}" != "${VERSION_FROM_FILE}" ]; then \
      echo "REDIS_PVXS_IOC_VERSION (${REDIS_PVXS_IOC_VERSION}) must match VERSION (${VERSION_FROM_FILE})" >&2; \
      exit 1; \
    fi

RUN test -d third_party/epics-base/modules/pvData
RUN test -d third_party/pvxs/bundle/libevent
RUN test -d third_party/redis-adapter/redis-plus-plus

RUN make -C third_party/epics-base configure.install src.install -j"$(nproc)" && \
    make -C third_party/epics-base/modules libcom.install -j"$(nproc)"
RUN printf 'EPICS_BASE=%s/third_party/epics-base\n' "$(pwd)" > third_party/pvxs/configure/RELEASE.local
RUN make -C third_party/pvxs/bundle libevent
RUN make -C third_party/pvxs configure.install setup.install src.install tools.install -j"$(nproc)"

# Now the IOC sources (this COPY merges over third_party without disturbing the
# built artifacts created by the RUN layers above).
COPY . .

RUN cmake -S . -B build \
    -D REDIS_PVXS_IOC_BUILD_TESTS=ON \
    -D REDIS_PVXS_IOC_GIT_REVISION_OVERRIDE="${REDIS_PVXS_IOC_REVISION}"
RUN cmake --build build -j"$(nproc)"
RUN ctest --test-dir build --output-on-failure
RUN EPICS_HOST_ARCH="$(perl third_party/epics-base/lib/perl/EpicsHostArch.pl)" && \
    mkdir -p /opt/runtime/bin /opt/runtime/lib/epics-base /opt/runtime/lib/pvxs /opt/runtime/lib/libevent && \
    cp build/redis-pvxs-ioc /opt/runtime/bin/redis-pvxs-ioc && \
    cp build/redis-pvxs-channelfinder-sync /opt/runtime/bin/redis-pvxs-channelfinder-sync && \
    cp -R "third_party/epics-base/lib/${EPICS_HOST_ARCH}/." /opt/runtime/lib/epics-base/ && \
    cp -R "third_party/pvxs/lib/${EPICS_HOST_ARCH}/." /opt/runtime/lib/pvxs/ && \
    if [ -d "third_party/pvxs/bundle/usr/${EPICS_HOST_ARCH}/lib" ]; then \
      cp -R "third_party/pvxs/bundle/usr/${EPICS_HOST_ARCH}/lib/." /opt/runtime/lib/libevent/; \
    fi && \
    cp -R "third_party/epics-base/bin/${EPICS_HOST_ARCH}" /opt/runtime/bin/epics-base && \
    cp -R "third_party/pvxs/bin/${EPICS_HOST_ARCH}" /opt/runtime/bin/pvxs

FROM ubuntu:24.04

ARG REDIS_PVXS_IOC_VERSION=dev
ARG REDIS_PVXS_IOC_REVISION=unknown
ARG REDIS_PVXS_IOC_SOURCE=https://github.com/fermi-ad/redis-pvxs-ioc

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    iproute2 \
    iputils-ping \
    libcurl4t64 \
    libgcc-s1 \
    libreadline8 \
    libstdc++6 \
    perl \
    # gRPC + protobuf runtime shared libs for the RPC->gRPC forwarding feature
    libgrpc++1.51t64 \
    libprotobuf32t64 \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/redis-pvxs-ioc

LABEL org.opencontainers.image.title="redis-pvxs-ioc" \
      org.opencontainers.image.version="${REDIS_PVXS_IOC_VERSION}" \
      org.opencontainers.image.revision="${REDIS_PVXS_IOC_REVISION}" \
      org.opencontainers.image.source="${REDIS_PVXS_IOC_SOURCE}" \
      org.opencontainers.image.licenses="BSD-3-Clause"

COPY --from=builder /opt/runtime /opt/redis-pvxs-ioc
COPY demo/config.yaml /etc/redis-pvxs-ioc/config.yaml
COPY scripts/container-entrypoint.sh /opt/redis-pvxs-ioc/bin/container-entrypoint.sh
COPY LICENSE NOTICE THIRD_PARTY_NOTICES.md /usr/share/doc/redis-pvxs-ioc/

RUN chmod +x /opt/redis-pvxs-ioc/bin/container-entrypoint.sh

ENV PATH=/opt/redis-pvxs-ioc/bin/pvxs:/opt/redis-pvxs-ioc/bin/epics-base:$PATH
ENV LD_LIBRARY_PATH=/opt/redis-pvxs-ioc/lib/epics-base:/opt/redis-pvxs-ioc/lib/pvxs:/opt/redis-pvxs-ioc/lib/libevent

ENTRYPOINT ["/opt/redis-pvxs-ioc/bin/container-entrypoint.sh"]
CMD ["--config", "/etc/redis-pvxs-ioc/config.yaml"]
