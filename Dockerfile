ARG BUILDKIT_SBOM_SCAN_STAGE=builder
# Update the base digest and package snapshot together in a reviewed PR.
FROM ubuntu:26.04@sha256:da6fc2be547864451aa253836dd926da33623312df4a9a243e35dc877c378a78 AS pinned-base
# The minimal base lacks HTTPS trust roots. BuildKit fetches this Ubuntu
# package with TLS and checks its pinned hash before APT contacts the snapshot.
ADD --checksum=sha256:f7025ab9b24cd73215510931037b02d6960d89584d0d00afba81851abdbe6ef1 \
    https://archive.ubuntu.com/ubuntu/pool/main/c/ca-certificates/ca-certificates_20260223_all.deb /tmp/ca-certificates.deb
RUN dpkg-deb --extract /tmp/ca-certificates.deb /tmp/apt-trust && \
    mkdir -p /etc/ssl/certs && \
    cat /tmp/apt-trust/usr/share/ca-certificates/mozilla/*.crt > /etc/ssl/certs/ca-certificates.crt && \
    printf 'Acquire::https::CaInfo "/etc/ssl/certs/ca-certificates.crt";\n' > /etc/apt/apt.conf.d/50snapshot-tls && \
    rm -rf /tmp/apt-trust /tmp/ca-certificates.deb
COPY packaging/ubuntu.sources /etc/apt/sources.list.d/ubuntu.sources

FROM pinned-base AS dependencies

ARG REDIS_PVXS_IOC_VERSION=dev
ARG REDIS_PVXS_IOC_REVISION=unknown
ARG REDIS_PVXS_IOC_SOURCE=https://github.com/fermi-ad/redis-pvxs-ioc

RUN apt-get update --error-on=any && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    git \
    libcurl4-openssl-dev \
    libreadline-dev \
    perl \
    pkg-config \
    python3 \
    redis-server \
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
COPY patches/pvxs/ patches/pvxs/
COPY scripts/apply-pvxs-patches.py scripts/apply-pvxs-patches.py
RUN python3 scripts/apply-pvxs-patches.py third_party/pvxs
RUN printf 'EPICS_BASE=%s/third_party/epics-base\n' "$(pwd)" > third_party/pvxs/configure/RELEASE.local
RUN make -C third_party/pvxs/bundle libevent
RUN make -C third_party/pvxs configure.install setup.install src.install tools.install -j"$(nproc)"

FROM dependencies AS builder

# Now the IOC sources (this COPY merges over third_party without disturbing the
# built artifacts created by the RUN layers above).
COPY . .
# The source overlay includes the pristine pinned submodule files. Restore the
# exact project extension while preserving the compiled dependency artifacts.
RUN python3 scripts/apply-pvxs-patches.py third_party/pvxs

# These extension tests require only Base libCom, matching the production
# dependency build. They exercise cancellation, status and native wire state.
RUN make -C third_party/pvxs/test/proxy -j4 && \
    make -C third_party/pvxs/test/proxy runtests
# Qualify the wire extensions against the stock implementations already pinned
# in Base. The reference peer remains in the builder and is not a runtime dep.
RUN python3 tests/build_pva_native_reference.py \
    --source-base third_party/epics-base --built-base third_party/epics-base \
    --build-dir /tmp/pva-native-reference --jobs 4
RUN cmake -S . -B build \
    -D REDIS_PVXS_IOC_BUILD_TESTS=ON \
    -D PVA_NATIVE_REFERENCE_EXECUTABLE=/tmp/pva-native-reference/pva_native_reference \
    -D REDIS_PVXS_IOC_GIT_REVISION_OVERRIDE="${REDIS_PVXS_IOC_REVISION}"
RUN cmake --build build -j"$(nproc)"
RUN ctest --test-dir build --output-on-failure
RUN EPICS_HOST_ARCH="$(perl third_party/epics-base/lib/perl/EpicsHostArch.pl)" && \
    mkdir -p /opt/runtime/bin /opt/runtime/lib/epics-base /opt/runtime/lib/pvxs /opt/runtime/lib/libevent && \
    cp build/redis-pvxs-ioc /opt/runtime/bin/redis-pvxs-ioc && \
    cp build/xrm-proxy /opt/runtime/bin/xrm-proxy && \
    cp build/redis-pvxs-channelfinder-sync /opt/runtime/bin/redis-pvxs-channelfinder-sync && \
    cp -R "third_party/epics-base/lib/${EPICS_HOST_ARCH}/." /opt/runtime/lib/epics-base/ && \
    cp -R "third_party/pvxs/lib/${EPICS_HOST_ARCH}/." /opt/runtime/lib/pvxs/ && \
    if [ -d "third_party/pvxs/bundle/usr/${EPICS_HOST_ARCH}/lib" ]; then \
      cp -R "third_party/pvxs/bundle/usr/${EPICS_HOST_ARCH}/lib/." /opt/runtime/lib/libevent/; \
    fi && \
    cp -R "third_party/epics-base/bin/${EPICS_HOST_ARCH}" /opt/runtime/bin/epics-base && \
    cp -R "third_party/pvxs/bin/${EPICS_HOST_ARCH}" /opt/runtime/bin/pvxs

# Retain notices for the actual source tree, including nested dependencies.
# Keep their relative paths so identical filenames do not overwrite each other.
RUN mkdir -p /opt/notices/third-party && \
    find third_party -type f \( -iname 'LICENSE*' -o -iname 'LICENCE*' -o \
      -iname 'COPYING*' -o -iname 'COPYRIGHT*' -o -iname 'NOTICE*' \) \
      -exec cp --parents -t /opt/notices/third-party/ {} + && \
    dpkg-query -W -f '${binary:Package}\t${Version}\t${Architecture}\n' | \
      LC_ALL=C sort > /opt/notices/builder-packages.tsv

FROM pinned-base

ARG REDIS_PVXS_IOC_VERSION=dev
ARG REDIS_PVXS_IOC_REVISION=unknown
ARG REDIS_PVXS_IOC_SOURCE=https://github.com/fermi-ad/redis-pvxs-ioc

RUN apt-get update --error-on=any && apt-get install -y --no-install-recommends \
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
COPY --from=builder /opt/notices/ /usr/share/doc/redis-pvxs-ioc/

RUN dpkg-query -W -f '${binary:Package}\t${Version}\t${Architecture}\n' | \
      LC_ALL=C sort > /usr/share/doc/redis-pvxs-ioc/runtime-packages.tsv

RUN chmod +x /opt/redis-pvxs-ioc/bin/container-entrypoint.sh

RUN groupadd --gid 10001 ioc && \
    useradd --uid 10001 --gid 10001 --no-create-home --home-dir /nonexistent --shell /usr/sbin/nologin ioc && \
    install -d -o 10001 -g 10001 -m 0755 /var/lib/redis-pvxs-ioc /run/xrm-proxy

ENV PATH=/opt/redis-pvxs-ioc/bin/pvxs:/opt/redis-pvxs-ioc/bin/epics-base:$PATH
ENV LD_LIBRARY_PATH=/opt/redis-pvxs-ioc/lib/epics-base:/opt/redis-pvxs-ioc/lib/pvxs:/opt/redis-pvxs-ioc/lib/libevent

USER 10001:10001

ENTRYPOINT ["/opt/redis-pvxs-ioc/bin/container-entrypoint.sh"]
CMD ["--config", "/etc/redis-pvxs-ioc/config.yaml"]
