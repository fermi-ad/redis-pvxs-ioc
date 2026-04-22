# Changelog

## v0.1.1 - 2026-04-22

- switch the container build/runtime base image to Ubuntu 24.04
- add a container entrypoint that exports the standard EPICS CA/PVA multicast network defaults before launching the IOC
- install `iproute2` and `iputils-ping` in the runtime image for basic network diagnostics
- make the default demo/smoke image reference track the published `v0.1.1` release tag

## v0.1.0 - 2026-04-22

- first formal `redis-pvxs-ioc` release
- standalone PVA-only Redis-backed IOC runtime with hot config reload
- registry-only compose demo and smoke validation path
- semver-aware CMake project version and binary `--version` output
