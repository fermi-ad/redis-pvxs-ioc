# Changelog

## v0.5.1 - 2026-06-26

- publish an always-on read-only `<server.instance>:version` PVA string with the running `redis-pvxs-ioc` release version

## v0.5.0 - 2026-05-13

- keep the legacy sidecar IOC alive in non-interactive Docker/Compose mode while preserving an interactive IOC shell for TTY-attached runs
- make Redis alarm stream events schema-compatible with `fermi-ad/epics-alarm-push` commit `cfbee1e110cf8b08c79c5faf604e0a859bcffbfe`
- add `linStat` to the prelinked legacy sidecar support-module bundle as an opt-in Linux statistics capability

## v0.4.0 - 2026-05-12

- major legacy sidecar compatibility release for controls EPICS ecosystem adoption
- add a prelinked support-module bundle to the conventional IOC sidecar: `seq`, `sscan`, `calc`, `asyn`, `std`, `pcre`, `StreamDevice`, `lua`, `iocStats`, `alive`, `autosave`, `busy`, and `caPutLog`
- include Fermilab `tcast` and `acnetPV` in the sidecar image as opt-in capabilities while keeping them inactive by default
- keep support-module behavior isolated in the sidecar lane so the Redis/PVXS runtime remains PVA-first and hot-reload focused
- document the support-module adoption path, pinned public remotes, and vendored FNAL compatibility sources

## v0.3.0 - 2026-05-06

- add direct ChannelFinder sync for Redis-sourced PV definitions with dry-run JSON and HTTP publish modes
- add optional `channelfinder` config metadata for owner, tags, and properties
- include upstream RecSync/RecCaster in the legacy sidecar image for conventional IOC record cataloging
- expose RecCaster status records under `LEGACY:RecCaster:` and publish UDP `5049` in the legacy compose overlay
- document the split catalog model: RecCaster for sidecar records, direct ChannelFinder sync for Redis-backed PVs

## v0.2.0 - 2026-04-28

- add an optional conventional IOC sidecar image/demo for base-record `.db` workflows with PVA exposure through `pvxsIoc`
- keep the main `redis-pvxs-ioc` runtime PVA-first with no `.dbd`, `.db`, or `iocInit()` behavior
- make the legacy sidecar compose path registry-first while preserving an optional maintainer build overlay
- disable CA server behavior in the sidecar by default and require `LEGACY_IOC_ENABLE_CA=YES` for opt-in CA compatibility

## v0.1.2 - 2026-04-22

- make the default PVA multicast configuration host-agnostic instead of assuming a specific interface name
- keep `EPICS_HOST_INTERFACE` as an explicit opt-in override when interface pinning is required
- harden version metadata generation and smoke-test cleanup based on packaging review follow-up
- make the default demo/smoke image reference track the published `v0.1.2` release tag

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
