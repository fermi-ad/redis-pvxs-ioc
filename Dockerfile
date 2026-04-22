FROM debian:bookworm-slim AS builder

ARG REDIS_PVXS_IOC_VERSION=dev
ARG REDIS_PVXS_IOC_REVISION=unknown
ARG REDIS_PVXS_IOC_SOURCE=https://github.com/fermi-ad/redis-pvxs-ioc

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    libreadline-dev \
    perl \
    pkg-config \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/redis-pvxs-ioc
COPY . .

RUN test -d third_party/epics-base/modules/pvData
RUN test -d third_party/pvxs/bundle/libevent
RUN test -d third_party/redis-adapter/redis-plus-plus

RUN make -C third_party/epics-base configure.install src.install -j"$(nproc)" && \
    make -C third_party/epics-base/modules libcom.install -j"$(nproc)"
RUN printf 'EPICS_BASE=%s/third_party/epics-base\n' "$(pwd)" > third_party/pvxs/configure/RELEASE.local
RUN make -C third_party/pvxs/bundle libevent
RUN make -C third_party/pvxs configure.install setup.install src.install tools.install -j"$(nproc)"
RUN cmake -S . -B build \
    -D REDIS_PVXS_IOC_BUILD_TESTS=ON \
    -D REDIS_PVXS_IOC_GIT_REVISION_OVERRIDE="${REDIS_PVXS_IOC_REVISION}"
RUN cmake --build build -j"$(nproc)"
RUN ctest --test-dir build --output-on-failure
RUN EPICS_HOST_ARCH="$(perl third_party/epics-base/lib/perl/EpicsHostArch.pl)" && \
    mkdir -p /opt/runtime/bin /opt/runtime/lib/epics-base /opt/runtime/lib/pvxs /opt/runtime/lib/libevent && \
    cp build/redis-pvxs-ioc /opt/runtime/bin/redis-pvxs-ioc && \
    cp -R "third_party/epics-base/lib/${EPICS_HOST_ARCH}/." /opt/runtime/lib/epics-base/ && \
    cp -R "third_party/pvxs/lib/${EPICS_HOST_ARCH}/." /opt/runtime/lib/pvxs/ && \
    if [ -d "third_party/pvxs/bundle/usr/${EPICS_HOST_ARCH}/lib" ]; then \
      cp -R "third_party/pvxs/bundle/usr/${EPICS_HOST_ARCH}/lib/." /opt/runtime/lib/libevent/; \
    fi && \
    cp -R "third_party/epics-base/bin/${EPICS_HOST_ARCH}" /opt/runtime/bin/epics-base && \
    cp -R "third_party/pvxs/bin/${EPICS_HOST_ARCH}" /opt/runtime/bin/pvxs

FROM debian:bookworm-slim

ARG REDIS_PVXS_IOC_VERSION=dev
ARG REDIS_PVXS_IOC_REVISION=unknown
ARG REDIS_PVXS_IOC_SOURCE=https://github.com/fermi-ad/redis-pvxs-ioc

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    libgcc-s1 \
    libreadline8 \
    libstdc++6 \
    perl \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/redis-pvxs-ioc

LABEL org.opencontainers.image.title="redis-pvxs-ioc" \
      org.opencontainers.image.version="${REDIS_PVXS_IOC_VERSION}" \
      org.opencontainers.image.revision="${REDIS_PVXS_IOC_REVISION}" \
      org.opencontainers.image.source="${REDIS_PVXS_IOC_SOURCE}"

COPY --from=builder /opt/runtime /opt/redis-pvxs-ioc
COPY demo/config.yaml /etc/redis-pvxs-ioc/config.yaml

ENV PATH=/opt/redis-pvxs-ioc/bin/pvxs:/opt/redis-pvxs-ioc/bin/epics-base:$PATH
ENV LD_LIBRARY_PATH=/opt/redis-pvxs-ioc/lib/epics-base:/opt/redis-pvxs-ioc/lib/pvxs:/opt/redis-pvxs-ioc/lib/libevent

ENTRYPOINT ["/opt/redis-pvxs-ioc/bin/redis-pvxs-ioc"]
CMD ["--config", "/etc/redis-pvxs-ioc/config.yaml"]
