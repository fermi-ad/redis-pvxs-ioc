# Published Submodule Remotes

`.gitmodules` now points at published remotes.
Before pushing `redis-pvxs-ioc`, verify that each remote contains the exact pinned commit recorded by the superproject.

## Required remotes

### `third_party/epics-base`

- Published `.gitmodules` URL: `git@github.com:derekste/epics-base.git`
- Pinned commit: `c5333b5c88dc05bbb5a6bc641527f11f949953b8`
- Publish status: ready
- Current known branch containing the commit: `dev/as-hag-dns-ttl`
- Verified remote branch head: `git@github.com:derekste/epics-base.git` `refs/heads/dev/as-hag-dns-ttl`
- Upstream relink plan: once these fork changes are merged upstream, point this submodule back to the main `epics-base/epics-base` repo at the merged upstream commit

### `third_party/pvxs`

- Published `.gitmodules` URL: `git@github.com:derekste/pvxs.git`
- Pinned commit: `3c83f4bde9a36a3d06d65389b4f043de244464ac`
- Publish status: ready
- Current known branch containing the commit: `dev/client-stateful-dns`
- Verified remote branch head: `git@github.com:derekste/pvxs.git` `refs/heads/dev/client-stateful-dns`
- Upstream relink plan: once these fork changes are merged upstream, point this submodule back to the main `epics-base/pvxs` repo at the merged upstream commit

### `third_party/redis-adapter`

- Published `.gitmodules` URL: `git@github.com:fermi-ad/redis-adapter.git`
- Pinned commit: `31832be0ab6d38ed1296f39ab73334a8663c7e6a`
- Publish status: ready
- Current known branch containing the commit: `origin/main`

### `third_party/yaml-cpp`

- Published `.gitmodules` URL: `https://github.com/jbeder/yaml-cpp.git`
- Pinned commit: `4861d049534ed6f2c51c45b01d7c2926022e5f3f`
- Publish status: ready
- Source of the local checkout: `https://github.com/jbeder/yaml-cpp.git`

## Push checklist

1. Run `git submodule sync --recursive`.
2. Re-run `git submodule update --init --recursive` from a clean clone to verify the repo is self-bootstrapable.
