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
| libevent (bundled by PVXS) | <https://github.com/libevent/libevent> | `third_party/pvxs/bundle/libevent/LICENSE` |
| hiredis (bundled by redis-adapter) | <https://github.com/redis/hiredis> | `third_party/redis-adapter/hiredis/COPYING` |
| redis-plus-plus (bundled by redis-adapter) | <https://github.com/sewenew/redis-plus-plus> | `third_party/redis-adapter/redis-plus-plus/LICENSE` |

The runtime image carries source-tree license, copying, copyright, and notice
files under `/usr/share/doc/redis-pvxs-ioc/third-party/`, preserving their
repository-relative paths. This includes nested source dependencies and their
test/build dependencies; inclusion in this directory does not mean that every
component is linked into the runtime executable.

The runtime container also includes packages supplied by Ubuntu, including
gRPC, Protocol Buffers, libcurl, readline, and their runtime dependencies.
Their notices remain under `/usr/share/doc/<package>/copyright`. The exact
builder and runtime package versions are recorded in `builder-packages.tsv`
and `runtime-packages.tsv` under `/usr/share/doc/redis-pvxs-ioc/`. The base image
digest is pinned in `Dockerfile`; the Ubuntu package archive snapshot is pinned
in `packaging/ubuntu.sources`.

Native discovery implements the RecCaster wire protocol and does not compile
RecCaster or conventional IOC support-module sources. The protocol references
are upstream `ChannelFinder/reccaster` and `ChannelFinder/recsync`.

The optional discovery acceptance tests download pinned RecCeiver and pyCFClient
sources; their upstream license notices remain with those test dependencies.
The historical RecCaster notice remains in `licenses/RecCaster-LICENSE` for
historical source distributions.
