# This local-only helper image is not a release image or a digest promotion.
# The collector appends this stage to the exact candidate Dockerfile, then
# builds only this target with a private resource-bounded BuildKit builder.
FROM builder AS qualification-tools
ARG QUALIFICATION_SCOPE
LABEL org.fermi-ad.redis-pvxs-ioc.qualification="${QUALIFICATION_SCOPE}"
WORKDIR /opt/redis-pvxs-ioc
COPY scripts/qualification-timings.patch /tmp/qualification-timings.patch
RUN git apply --check /tmp/qualification-timings.patch && \
    git apply /tmp/qualification-timings.patch && \
    cmake --build build --target ndarray_transfer stream_capacity qualification_probe -j4 && \
    test -x /usr/bin/python3
