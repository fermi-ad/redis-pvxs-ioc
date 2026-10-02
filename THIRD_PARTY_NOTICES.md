# Third-Party Notices

Project-authored `redis-pvxs-ioc` code is distributed under the BSD 3-Clause
License in [`LICENSE`](LICENSE). Third-party components retain their own copyright
and license terms. This file is an inventory; the referenced license files are
authoritative.

## Git submodules

| Component | Source | License notice in this checkout |
| --- | --- | --- |
| EPICS Base | <https://github.com/derekste/epics-base> | `third_party/epics-base/LICENSE` |
| PVXS | <https://github.com/epics-base/pvxs> | `third_party/pvxs/LICENSE` |
| redis-adapter and its bundled dependencies | <https://github.com/fermi-ad/redis-adapter> | `third_party/redis-adapter/LICENSE`, plus nested notices |
| yaml-cpp | <https://github.com/jbeder/yaml-cpp> | `third_party/yaml-cpp/LICENSE` |

The runtime container also includes packages supplied by Ubuntu, including
gRPC, Protocol Buffers, libcurl, libevent, readline, and their runtime
dependencies. Their package metadata and upstream source distributions contain
the applicable notices.

Native discovery implements the RecCaster wire protocol and does not compile
RecCaster or conventional IOC support-module sources. The protocol references
are upstream `ChannelFinder/reccaster` and `ChannelFinder/recsync`.

The optional discovery acceptance tests download pinned RecCeiver and pyCFClient
sources; their upstream license notices remain with those test dependencies.
The historical RecCaster notice remains in `licenses/RecCaster-LICENSE` for
historical source distributions.
