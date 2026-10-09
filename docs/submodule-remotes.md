# Published Submodule Remotes

`.gitmodules` points at public HTTPS remotes so anonymous recursive clones and
fork pull-request workflows do not need GitHub SSH credentials. Before changing a
gitlink, verify that its public remote contains the exact pinned commit.

## Required remotes

### `third_party/epics-base`

- Published `.gitmodules` URL: `https://github.com/derekste/epics-base.git`
- Pinned commit: `64fe4ba7c0aeb384ee68accc80e630a52a7bcd27`
- Publish status: published correction; final candidate qualification pending
- Verified remote branch: `https://github.com/derekste/epics-base.git`
  `refs/heads/dev/hag-dns-staging`
- Review: [derekste/epics-base #1](https://github.com/derekste/epics-base/pull/1),
  based directly on the previous IOC pin
- Upstream relink plan: once these fork changes are merged upstream, point this
  submodule back to the main `epics-base/epics-base` repo at the merged upstream commit
- This correction stages HAG DNS work outside the client access lock while
  retaining the documented synchronous-callback restriction. Independent
  Instrumentation review and final candidate access/recovery qualification
  remain required.

### `third_party/pvxs`

- Published `.gitmodules` URL: `https://github.com/derekste/pvxs.git`
- Pinned commit: `749a8933b7982cb4dd9c16660a53f93bcdd3d67e`
- Publish status: published bounded-backlog correction; final candidate
  qualification pending
- Verified remote branch: `refs/heads/dev/listener-backlog-v0.9`
- The pin is one commit and one line beyond the previous `1.5.2`-based IOC pin,
  changing the listener backlog from 4 to 32. [Issue #122](https://github.com/fermi-ad/redis-pvxs-ioc/issues/122)
  retains the burst comparison. Upstream
  [PVXS #229](https://github.com/epics-base/pvxs/pull/229) remains a follow-up
  and is not a release gate for this fork pin.
- Release gate: repeat the final candidate burst, outage and reconnect
  qualification with strict CI and independent Instrumentation review.

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
