FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    ca-certificates \
    cmake \
    iproute2 \
    iputils-ping \
    libreadline-dev \
    libtirpc-dev \
    perl \
    re2c \
  && rm -rf /var/lib/apt/lists/*

WORKDIR /opt/redis-pvxs-ioc
COPY third_party ./third_party
COPY legacy-sidecar ./legacy-sidecar

RUN test -d third_party/epics-base/modules/database
RUN test -d third_party/pvxs/ioc

RUN make -C third_party/epics-base configure.install src.install -j"$(nproc)" && \
    make -C third_party/epics-base/modules libcom.install -j"$(nproc)" && \
    make -C third_party/epics-base/modules/ca install TESTPROD_HOST= TESTSCRIPTS_HOST= -j"$(nproc)" && \
    make -C third_party/epics-base/modules/database/src install -j"$(nproc)"
RUN printf 'EPICS_BASE=%s/third_party/epics-base\n' "$(pwd)" > third_party/pvxs/configure/RELEASE.local
RUN make -C third_party/pvxs/bundle libevent
RUN make -C third_party/pvxs configure.install setup.install src.install ioc.install tools.install -j"$(nproc)"
RUN printf 'EPICS_BASE=%s/third_party/epics-base\n' "$(pwd)" > third_party/recsync/client/configure/RELEASE.local
RUN make -C third_party/recsync/client configure.install castApp.install \
    TESTPROD_HOST= TESTSCRIPTS_HOST= -j"$(nproc)"
RUN legacy-sidecar/build-controls-support.sh
RUN make -C legacy-sidecar/app -j"$(nproc)"

RUN EPICS_HOST_ARCH="$(perl third_party/epics-base/lib/perl/EpicsHostArch.pl)" && \
    mkdir -p /opt/legacy-runtime/bin /opt/legacy-runtime/db /opt/legacy-runtime/dbd \
      /opt/legacy-runtime/lib/epics-base /opt/legacy-runtime/lib/pvxs /opt/legacy-runtime/lib/reccaster /opt/legacy-runtime/lib/libevent \
      /opt/legacy-runtime/lib/controls /opt/legacy-runtime/support \
      /etc/legacy-ioc && \
    cp "legacy-sidecar/app/bin/${EPICS_HOST_ARCH}/legacyIoc" /opt/legacy-runtime/bin/legacyIoc && \
    cp "legacy-sidecar/app/dbd/legacyIoc.dbd" /opt/legacy-runtime/dbd/legacyIoc.dbd && \
    cp "legacy-sidecar/app/db/legacy-demo.db" /opt/legacy-runtime/db/legacy-demo.db && \
    cp "third_party/recsync/client/db/reccaster.db" /opt/legacy-runtime/db/reccaster.db && \
    cp "legacy-sidecar/iocBoot/st.cmd" /etc/legacy-ioc/st.cmd && \
    cp "legacy-sidecar/entrypoint.sh" /opt/legacy-runtime/bin/entrypoint.sh && \
    cp -R "third_party/epics-base/lib/${EPICS_HOST_ARCH}/." /opt/legacy-runtime/lib/epics-base/ && \
    cp -R "third_party/pvxs/lib/${EPICS_HOST_ARCH}/." /opt/legacy-runtime/lib/pvxs/ && \
    cp -R "third_party/recsync/client/lib/${EPICS_HOST_ARCH}/." /opt/legacy-runtime/lib/reccaster/ && \
    for module in \
      third_party/support/seq \
      third_party/support/sscan \
      third_party/support/calc \
      third_party/support/asyn \
      third_party/support/std \
      third_party/support/pcre \
      third_party/support/StreamDevice \
      third_party/support/lua \
      third_party/support/iocStats \
      third_party/support/alive \
      third_party/support/autosave \
      third_party/support/busy \
      third_party/support/caPutLog \
      third_party/fnal/tcast \
      third_party/fnal/acnetPV; do \
      name="$(basename "$module")"; \
      mkdir -p "/opt/legacy-runtime/support/${name}/db" "/opt/legacy-runtime/support/${name}/dbd"; \
      if [ -d "${module}/lib/${EPICS_HOST_ARCH}" ]; then \
        cp -R "${module}/lib/${EPICS_HOST_ARCH}/." /opt/legacy-runtime/lib/controls/; \
      fi; \
      if [ -d "${module}/db" ]; then \
        cp -R "${module}/db/." "/opt/legacy-runtime/support/${name}/db/"; \
      fi; \
      if [ -d "${module}/dbd" ]; then \
        cp -R "${module}/dbd/." "/opt/legacy-runtime/support/${name}/dbd/"; \
      fi; \
    done && \
    if [ -d "third_party/pvxs/bundle/usr/${EPICS_HOST_ARCH}/lib" ]; then \
      cp -R "third_party/pvxs/bundle/usr/${EPICS_HOST_ARCH}/lib/." /opt/legacy-runtime/lib/libevent/; \
    fi && \
    chmod +x /opt/legacy-runtime/bin/entrypoint.sh

FROM ubuntu:24.04

RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    iproute2 \
    iputils-ping \
    libgcc-s1 \
    libreadline8t64 \
    libstdc++6 \
    libtirpc3 \
  && rm -rf /var/lib/apt/lists/*

COPY --from=builder /opt/legacy-runtime /opt/legacy-ioc
COPY --from=builder /etc/legacy-ioc /etc/legacy-ioc

WORKDIR /opt/legacy-ioc

ENV PATH=/opt/legacy-ioc/bin:$PATH
ENV LD_LIBRARY_PATH=/opt/legacy-ioc/lib/epics-base:/opt/legacy-ioc/lib/pvxs:/opt/legacy-ioc/lib/reccaster:/opt/legacy-ioc/lib/controls:/opt/legacy-ioc/lib/libevent

ENTRYPOINT ["/opt/legacy-ioc/bin/entrypoint.sh"]
