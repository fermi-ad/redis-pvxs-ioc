# Bundled PVA proxy extensions

The patch applies to the existing PVXS revision recorded in `manifest.json`.
The gitlink remains unchanged. The file hashes distinguish a pristine source
tree from the fully patched tree and reject source drift. Known mixtures of
before/after files can be completed safely after a Docker source overlay.
Both native and container builds must apply this patch before building PVXS.

The public API advertises `PVXS_XRM_PROXY_API == 1`; linking and calling
`pvxs::xrmProxyApiVersion()` must also return `1`. Checking both prevents using
patched headers with an unpatched library.

The extension adds manual native PROCESS, PUT_GET and ARRAY operations through
`ProxyBuilder` and `ProxySetupOp`; GET_FIELD subfields; manual RPC execution;
execution admission callbacks; native status carriers; and monitor overrun,
termination and reservation-lifetime forwarding. Existing APIs remain available.

`beforeSend` runs on the client connection worker after payload preparation and
before execution serialization. It must not block or perform logging. Throwing
rejects that execution without enqueuing execution bytes. A successful callback
is the logical commitment boundary: later cancellation, timeout, disconnection
or policy changes cannot establish whether a device acted. Guarded executions
are not replayed internally. The protocol supplies no durable identity with
which to deduplicate a downstream client's separate retry.

Cross-loop handlers use `Subscription::pauseAsync()` and
`ChannelControl::closeAsync()`. Explicit client cancellation remains synchronous;
release handles created with `syncCancel(false)` when cancellation must not wait
for the client loop. Queuing channel closure after a reply or error preserves
their order on the server loop.

Ordinary monitor value messages contain no PVA Status field. Setup status is
available through status-aware initialization callbacks; final status is carried
by `Finished` or `RemoteError`. Value events preserve overrun field indexes and
may retain a local reservation lease while queued. The lease is never serialized.

The bundled regressions `testproxyclient` and `testproxyserver` exercise these
interfaces over isolated loopback connections. They are added to the normal
PVXS test targets by the patch.

For the container's minimal Base build, only libCom is required. After building
PVXS's `configure.install`, `setup.install` and `src.install` targets, build and
run the isolated test directory:

```sh
make -C third_party/pvxs/test/proxy -j4
make -C third_party/pvxs/test/proxy runtests
```

This directory uses the same test sources as the normal test targets and links
the configured libevent dependencies through PVXS's existing build rules. It
does not build IOC support or require `dbCore` or `dbRecStd`. The commands apply
unchanged on native hosts and in Linux containers.
