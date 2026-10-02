# Published Submodule Remotes

`.gitmodules` points at public HTTPS remotes so anonymous recursive clones and
fork pull-request workflows do not need GitHub SSH credentials. Before changing a
gitlink, verify that its public remote contains the exact pinned commit.

## Required remotes

### `third_party/epics-base`

- Published `.gitmodules` URL: `https://github.com/derekste/epics-base.git`
- Pinned commit: `c8ecd7c29d5e8cdd3ee15da2aaf761bda4f37e37`
- Publish status: ready
- Current known branch containing the commit: `dev/as-hag-refresh-api-v7.0.10`
- Verified remote branch: `https://github.com/derekste/epics-base.git` `refs/heads/dev/as-hag-refresh-api-v7.0.10`
- Upstream relink plan: once these fork changes are merged upstream, point this submodule back to the main `epics-base/epics-base` repo at the merged upstream commit

### `third_party/pvxs`

- Published `.gitmodules` URL: `https://github.com/epics-base/pvxs.git`
- Pinned commit: `8e00eaecdee5ce8a474704e70d820e6f92693fa1`
- Publish status: ready
- Release tag: `1.5.2`
- No redis-pvxs-ioc-specific PVXS changes are required.

### `third_party/redis-adapter`

- Published `.gitmodules` URL: `https://github.com/fermi-ad/redis-adapter.git`
- Pinned commit: `b6c442a04746d420742ca0640111a65286fd22e9`
- Publish status: published development commit; merge and release qualification pending
- Verified remote branch: `dev/subscription-lifecycle`
- Upstream changes: [redis-adapter #108](https://github.com/fermi-ad/redis-adapter/pull/108),
  dependent on [redis-adapter #127](https://github.com/fermi-ad/redis-adapter/pull/127)
- Release gate: keep the correctness preparation in draft until both upstream
  changes are merged, update this pin to the exact reviewed merged revision,
  and complete strict CI with independent Instrumentation review.

### `third_party/yaml-cpp`

- Published `.gitmodules` URL: `https://github.com/jbeder/yaml-cpp.git`
- Pinned commit: `4861d049534ed6f2c51c45b01d7c2926022e5f3f`
- Publish status: ready
- Source of the local checkout: `https://github.com/jbeder/yaml-cpp.git`

## Update checklist

1. Run `git submodule sync --recursive`.
2. Re-run `git submodule update --init --recursive` from an anonymous clean clone
   to verify the repo is self-bootstrapable.
