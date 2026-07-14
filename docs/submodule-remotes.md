# Published Submodule Remotes

`.gitmodules` points at public HTTPS remotes so anonymous recursive clones and
fork pull-request workflows do not need GitHub SSH credentials. Before changing a
gitlink, verify that its public remote contains the exact pinned commit.

## Required remotes

### `third_party/epics-base`

- Published `.gitmodules` URL: `https://github.com/derekste/epics-base.git`
- Pinned commit: `c5333b5c88dc05bbb5a6bc641527f11f949953b8`
- Publish status: ready
- Current known branch containing the commit: `dev/as-hag-dns-ttl`
- Verified remote branch: `https://github.com/derekste/epics-base.git` `refs/heads/dev/as-hag-dns-ttl`
- Upstream relink plan: once these fork changes are merged upstream, point this submodule back to the main `epics-base/epics-base` repo at the merged upstream commit

### `third_party/pvxs`

- Published `.gitmodules` URL: `https://github.com/derekste/pvxs.git`
- Pinned commit: `3c83f4bde9a36a3d06d65389b4f043de244464ac`
- Publish status: ready
- Current known branch containing the commit: `dev/client-stateful-dns`
- Verified remote branch: `https://github.com/derekste/pvxs.git` `refs/heads/dev/client-stateful-dns`
- Upstream relink plan: once these fork changes are merged upstream, point this submodule back to the main `epics-base/pvxs` repo at the merged upstream commit

### `third_party/redis-adapter`

- Published `.gitmodules` URL: `https://github.com/fermi-ad/redis-adapter.git`
- Pinned commit: `fd26d57dd9146ae4898c8623c3994776101e91c5`
- Publish status: ready
- Current known branch containing the commit: `origin/main`

### `third_party/yaml-cpp`

- Published `.gitmodules` URL: `https://github.com/jbeder/yaml-cpp.git`
- Pinned commit: `4861d049534ed6f2c51c45b01d7c2926022e5f3f`
- Publish status: ready
- Source of the local checkout: `https://github.com/jbeder/yaml-cpp.git`

### `third_party/recsync`

- Published `.gitmodules` URL: `https://github.com/ChannelFinder/recsync.git`
- Pinned commit: `864b162cb05fa140e67e056bda86ed750447c3d4`
- Pinned upstream tag: `1.9.2`
- Publish status: ready
- Use: legacy IOC sidecar RecCaster support only

## Legacy sidecar support-module remotes

These public support modules are pinned for the registry-built legacy IOC sidecar:

| Path | Remote | Pinned commit | Tag/describe |
| --- | --- | --- | --- |
| `third_party/support/seq` | `https://github.com/epics-modules/sequencer.git` | `7544ad3acd18d0b4d072c5d3b089b87b073097c0` | `R2-2-5` |
| `third_party/support/sscan` | `https://github.com/epics-modules/sscan.git` | `a67107fd9ca533d3056b157ad29a7e0b395f3147` | `R2-12` |
| `third_party/support/calc` | `https://github.com/epics-modules/calc.git` | `2f5b175f260bc3fe35bc25a3f6c204e9d6f628c9` | `R3-7-5` |
| `third_party/support/asyn` | `https://github.com/epics-modules/asyn.git` | `0e6d2edfcefd2e53580f07ba159a0ef0f8a4bc55` | `R4-44` |
| `third_party/support/std` | `https://github.com/epics-modules/std.git` | `1b416dbb74fb54060395347433c30361bd276424` | `R3-6-4` |
| `third_party/support/StreamDevice` | `https://github.com/paulscherrerinstitute/StreamDevice.git` | `211f689cdf8acaf45a53fe035a1b1d0fac8a40d7` | `2.8.24` |
| `third_party/support/lua` | `https://github.com/epics-modules/lua.git` | `0091a141a9f708a47c87554a72da79da039f210b` | `R3-0-2` |
| `third_party/support/iocStats` | `https://github.com/epics-modules/iocStats.git` | `4df9e87815f6a9432955a3ddb45fafa9fe4a4d40` | `3.1.16` |
| `third_party/support/alive` | `https://github.com/epics-modules/alive.git` | `fd1fd81286cc1ace7f06b9f7c91a8a0c8fecd044` | `R1-4-1` |
| `third_party/support/autosave` | `https://github.com/epics-modules/autosave.git` | `800c2d600908cf300c9b1c8beedd090991e8d554` | `R5-10-2` |
| `third_party/support/busy` | `https://github.com/epics-modules/busy.git` | `569a6b6fb1288c067ac2b22a998aa7de5375ddc4` | `R1-7-4` |
| `third_party/support/caPutLog` | `https://github.com/epics-modules/caPutLog.git` | `062998a1d4b9c4ccacf3277e9097a63c5db4c28e` | `R4.0` |
| `third_party/support/linStat` | `https://github.com/mdavidsaver/linStat.git` | `a470a5954ff7be81bb35e10fa09597e8bf8d6722` | `1.2.0` |

Vendored non-submodule trees:

- `third_party/support/pcre`: copied from the local GHE support-module monorepo `pcre-8.44` tree because the EPICS build wrapper is local.

## Update checklist

1. Run `git submodule sync --recursive`.
2. Re-run `git submodule update --init --recursive` from an anonymous clean clone
   to verify the repo is self-bootstrapable.
