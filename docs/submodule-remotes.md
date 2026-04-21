# Required Published Submodule Remotes

All four submodules in this repo currently point at local sibling paths in this workspace via `.gitmodules`.
Before pushing `redis-pvxs-ioc` to a shared remote, each of these needs a real published remote URL that contains the exact pinned commit recorded by the superproject.

## Required remotes

### `third_party/epics-base`

- Current `.gitmodules` URL: `../epics-base`
- Pinned commit: `c5333b5c88dc05bbb5a6bc641527f11f949953b8`
- Why it needs a published remote: the superproject currently assumes a local checkout exists next to `redis-pvxs-ioc`
- Remote requirement: publish a remote that contains this exact SHA
- Recommendation: if this SHA comes from a local feature branch needed for access-security behavior, publish that branch or mirror before changing `.gitmodules`

### `third_party/pvxs`

- Current `.gitmodules` URL: `../pvxs`
- Pinned commit: `3c83f4bde9a36a3d06d65389b4f043de244464ac`
- Why it needs a published remote: the superproject currently assumes a local checkout exists next to `redis-pvxs-ioc`
- Remote requirement: publish a remote that contains this exact SHA
- Recommendation: use a remote/branch that preserves the PVXS behavior expected by this MVP and by the design notes around access-security integration

### `third_party/redis-adapter`

- Current `.gitmodules` URL: `../redis-adapter`
- Pinned commit: `4a40fe8e6871a42bd225d405af9d8fdd859a591d`
- Why it needs a published remote: the pinned revision includes an MVP-local patch and does not exist on a public remote by default
- Remote requirement: publish a remote that contains this exact SHA
- Current local branch carrying the patch: `redis-pvxs-ioc-mvp`
- Patch purpose: force direct single-node Redis connections instead of probing cluster mode first

### `third_party/yaml-cpp`

- Current `.gitmodules` URL: `../yaml-cpp`
- Pinned commit: `4861d049534ed6f2c51c45b01d7c2926022e5f3f`
- Why it needs a published remote: the superproject currently assumes a local checkout exists next to `redis-pvxs-ioc`
- Remote requirement: point `.gitmodules` at a published remote that already contains this SHA
- Recommendation: the upstream `yaml-cpp` remote is probably sufficient if this exact commit exists there

## Push checklist

1. Publish or mirror each dependency repo so the pinned SHA exists on an accessible remote.
2. Update `.gitmodules` to use those published URLs instead of `../...` paths.
3. Run `git submodule sync --recursive`.
4. Re-run `git submodule update --init --recursive` from a clean clone to verify the repo is self-bootstrapable.
