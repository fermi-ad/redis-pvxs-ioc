# Third-Party Notices

Project-authored `redis-pvxs-ioc` code is distributed under the BSD 3-Clause
License in [`LICENSE`](LICENSE). Third-party components retain their own copyright
and license terms. This file is an inventory; the referenced license files are
authoritative.

## Git submodules

| Component | Source | License notice in this checkout |
| --- | --- | --- |
| EPICS Base | <https://github.com/derekste/epics-base> | `third_party/epics-base/LICENSE` |
| PVXS | <https://github.com/derekste/pvxs> | `third_party/pvxs/LICENSE` |
| redis-adapter and its bundled dependencies | <https://github.com/fermi-ad/redis-adapter> | `third_party/redis-adapter/LICENSE`, plus nested notices |
| yaml-cpp | <https://github.com/jbeder/yaml-cpp> | `third_party/yaml-cpp/LICENSE` |
| RecSync/RecCaster | <https://github.com/ChannelFinder/recsync> | `third_party/recsync/LICENSE` |
| sequencer | <https://github.com/epics-modules/sequencer> | `third_party/support/seq/LICENSE` |
| sscan | <https://github.com/epics-modules/sscan> | `third_party/support/sscan/LICENSE` |
| calc | <https://github.com/epics-modules/calc> | `third_party/support/calc/LICENSE` |
| asyn | <https://github.com/epics-modules/asyn> | `third_party/support/asyn/LICENSE` |
| std | <https://github.com/epics-modules/std> | `third_party/support/std/LICENSE` |
| StreamDevice | <https://github.com/paulscherrerinstitute/StreamDevice> | `third_party/support/StreamDevice/LICENSE` and `LICENSE.LESSER` |
| lua | <https://github.com/epics-modules/lua> | upstream notices in `third_party/support/lua` |
| iocStats | <https://github.com/epics-modules/iocStats> | `third_party/support/iocStats/LICENSE` |
| alive | <https://github.com/epics-modules/alive> | `third_party/support/alive/LICENSE` |
| autosave | <https://github.com/epics-modules/autosave> | `third_party/support/autosave/LICENSE` |
| busy | <https://github.com/epics-modules/busy> | upstream notices in `third_party/support/busy` |
| caPutLog | <https://github.com/epics-modules/caPutLog> | upstream notices in `third_party/support/caPutLog` |
| linStat | <https://github.com/mdavidsaver/linStat> | `third_party/support/linStat/LICENSE` |

The runtime container also includes packages supplied by Ubuntu, including
gRPC, Protocol Buffers, libcurl, libevent, readline, and their runtime
dependencies. Their package metadata and upstream source distributions contain
the applicable notices.

## Vendored source

- PCRE is vendored under `third_party/support/pcre`; its license is
  `third_party/support/pcre/pcre/LICENCE`.
