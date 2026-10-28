# Published Submodule Remotes

`.gitmodules` points at public HTTPS remotes so anonymous recursive clones and
fork pull-request workflows do not need GitHub SSH credentials. Before changing a
gitlink, verify that its public remote contains the exact pinned commit.

## Required remotes

### `third_party/epics-base`

- Published `.gitmodules` URL: `https://github.com/derekste/epics-base.git`
- Pinned commit: `91c1884e6dc259a9b8b5f3eacc22db83b516c2f4`
- Publish status: merged fork correction; final candidate qualification pending
- Verified remote branch: `https://github.com/derekste/epics-base.git`
  `refs/heads/dev/as-hag-refresh-api-v7.0.10`
- Review: [derekste/epics-base #1](https://github.com/derekste/epics-base/pull/1),
  merged into the fork release branch from the previous IOC pin
- Upstream relink plan: once these fork changes are merged upstream, point this
  submodule back to the main `epics-base/epics-base` repo at the merged upstream commit
- This correction stages HAG DNS work outside the client access lock while
  retaining the documented synchronous-callback restriction. It also declares
  the existing resolver dependency for static Linux `aslibtest` links.
  Final candidate access/recovery qualification remains required.

### `third_party/pvxs`

- Published `.gitmodules` URL: `https://github.com/epics-base/pvxs.git`
- Pinned commit: `96345ddbbad50144b9f4464b9a24a2e490bbbf45`
- Publish status: official upstream revision; integration validation and final
  candidate qualification remain separate acceptance steps
- Verified upstream branch: `refs/heads/master`
- [PVXS #229](https://github.com/epics-base/pvxs/pull/229) was closed as applied
  with changes on 28 October 2026. The official revision uses listener backlog
  32 on Linux and other non-RTOS platforms, retaining 4 on RTEMS and VxWorks.
  It replaces the personal fork's backlog-only correction. Tag `1.5.3` predates
  this revision and does not include the correction.
- [Issue #122](https://github.com/fermi-ad/redis-pvxs-ioc/issues/122) retains
  startup acceptance; [#138](https://github.com/fermi-ad/redis-pvxs-ioc/issues/138)
  still requires the separate EPICS Base fork exit. Neither is closed merely by
  adopting this pin. Final candidate burst, outage and reconnect qualification,
  strict CI and independent Instrumentation review remain required.

### `third_party/redis-adapter`

- Published `.gitmodules` URL: `https://github.com/fermi-ad/redis-adapter.git`
- Pinned commit: `69bf18ec403c21ce396759172b24a91506283155`
- Publish status: merged source; final candidate release qualification pending
- Upstream merge: [redis-adapter #132](https://github.com/fermi-ad/redis-adapter/pull/132)
- Upstream change: [redis-adapter #111](https://github.com/fermi-ad/redis-adapter/pull/111),
  following #127 → #108 → #109. Source-health callbacks use its immutable batch
  epoch/rejection metadata; the earlier callback API remains compatible.
- Release gate: qualify the final IOC candidate against this merged revision,
  with strict CI and independent Instrumentation review. Merged source alone is
  not release qualification evidence.

### `third_party/yaml-cpp`

- Published `.gitmodules` URL: `https://github.com/jbeder/yaml-cpp.git`
- Pinned commit: `4861d049534ed6f2c51c45b01d7c2926022e5f3f`
- Publish status: ready
- Source of the local checkout: `https://github.com/jbeder/yaml-cpp.git`

## Update checklist

1. Run `git submodule sync --recursive`.
2. Re-run `git submodule update --init --recursive` from an anonymous clean clone
   to verify the repo is self-bootstrapable.
