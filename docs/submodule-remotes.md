# Published Submodule Remotes

`.gitmodules` now points at published remotes.
Before pushing `redis-pvxs-ioc`, verify that each remote contains the exact pinned commit recorded by the superproject.

## Required remotes

### `third_party/epics-base`

- Published `.gitmodules` URL: `git@github.com:derekste/epics-base.git`
- Pinned commit: `c5333b5c88dc05bbb5a6bc641527f11f949953b8`
- Publish status: ready
- Current known branch containing the commit: `origin/dev/as-hag-dns-ttl`

### `third_party/pvxs`

- Published `.gitmodules` URL: `git@github.com:derekste/pvxs.git`
- Pinned commit: `3c83f4bde9a36a3d06d65389b4f043de244464ac`
- Publish status: still needs push to the published remote
- Current known branch containing the commit: local `dev/client-stateful-dns`
- Note: the local checkout currently has `origin` set to `git@github.com:epics-base/pvxs.git`, so the fork URL in `.gitmodules` assumes you will publish this commit to `git@github.com:derekste/pvxs.git`

### `third_party/redis-adapter`

- Published `.gitmodules` URL: `git@github.com:fermi-ad/redis-adapter.git`
- Pinned commit: `4a40fe8e6871a42bd225d405af9d8fdd859a591d`
- Publish status: still needs push to the published remote
- Current local branch carrying the patch: `redis-pvxs-ioc-mvp`
- Patch purpose: force direct single-node Redis connections instead of probing cluster mode first

### `third_party/yaml-cpp`

- Published `.gitmodules` URL: `https://github.com/jbeder/yaml-cpp.git`
- Pinned commit: `4861d049534ed6f2c51c45b01d7c2926022e5f3f`
- Publish status: ready
- Source of the local checkout: `https://github.com/jbeder/yaml-cpp.git`

## Push checklist

1. Push `pvxs` commit `3c83f4bde9a36a3d06d65389b4f043de244464ac` to `git@github.com:derekste/pvxs.git`.
2. Push `redis-adapter` branch `redis-pvxs-ioc-mvp` containing commit `4a40fe8e6871a42bd225d405af9d8fdd859a591d` to `git@github.com:fermi-ad/redis-adapter.git`.
3. Run `git submodule sync --recursive`.
4. Re-run `git submodule update --init --recursive` from a clean clone to verify the repo is self-bootstrapable.
