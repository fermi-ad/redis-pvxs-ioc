# Access control

Native EPICS access security is optional and disabled by default. Enable it
explicitly at process startup:

```yaml
access:
  enabled: true
  file: access.acf
  macros:
    IOC: demo
  watch:
    enabled: false
    interval_ms: 1000
    settle_ms: 250
  defaults:
    pv: {asg: DEFAULT, asl: 0}
    rpc: {asg: DEFAULT, asl: 0}
    admin_read: {asg: DEFAULT, asl: 0}
    admin_write: {asg: ACCESS_ADMINS, asl: 0}
```

`access.enabled` is startup-immutable. A reload which changes it is rejected;
restart the process to enable or disable access control. When disabled,
endpoint `access` assignments and an enabled watcher are rejected so a file
cannot accidentally imply protection which is not active. The disabled data
path continues to use PVXS's built-in static source directly.

When enabled, `file` is required. Relative paths resolve against the main YAML
file. Macros are expanded before validation, fingerprinting, and activation.
`asl` is limited to `0` or `1`. Per-endpoint assignments override the defaults:

```yaml
pvs:
  - name: magnet:current
    # ...
    access: {asg: OPERATORS, asl: 0}

rpc_services:
  - endpoint: query-server:50051
    service: example.Query
    access: {asg: QUERY_USERS, asl: 0}
```

The ACF parser is the pinned EPICS `asLib` implementation. `ASG`, `RULE`,
`UAG`, `HAG`, ASL 0/1, and `TRAPWRITE` are supported. Deferred `CALC()` and
`INPA` through `INPU` constructs are rejected with line and column information.
Every assigned ASG must be present before a policy can activate. HAG hostname
resolution and refresh follow the pinned EPICS branch and match IPv4 client
addresses.

## Authorization behavior

PVA GET, GET_FIELD, and monitor setup require EPICS `READ`; PUT and RPC require
`WRITE`. PV names remain discoverable, but a denied operation returns
`access denied`. The client account and each `role/<role>` credential are
evaluated independently, and any granting identity allows the operation.

Rights are cached per connected channel. An allowed operation performs one
atomic rights load; Redis update handling and `SharedPV::post()` monitor fan-out
do not evaluate policy. When EPICS reports a rights change, cached rights are
cleared immediately and the channel is closed after recomputation. This stops
active monitors and makes long-lived clients reconnect under the new policy.

Permitted `TRAPWRITE` operations and all denied writes are audited with the PV,
account, peer, authentication method, result, and a bounded value preview.
Ordinary permitted writes do not format their value. Denial logs are
rate-limited per operation, PV, and client while counters remain exact.

## Reload and file watching

A whole YAML reload always rereads and reapplies the ACF. To reload only the
policy, write any value to:

```sh
pvxput SYS:<instance>:access:reload 1
```

SIGHUP remains the recovery mechanism if policy denies the reload command.
Invalid, missing, or partially written policies retain the last good policy and
do not increment the access generation.

The optional watcher polls the configured path rather than an inode, so it sees
in-place edits, atomic replacements, and bind-mount updates. It waits until the
content fingerprint remains stable for `settle_ms`, and does not repeatedly
attempt the same unchanged invalid content.

After building the local `redis-pvxs-ioc:acf-local` image, run the isolated
Redis/PVXS acceptance sequence with:

```sh
./scripts/access-e2e-test.sh
```

The script uses uniquely named temporary containers and a network, removes
them on exit, and leaves its logs and status-PV evidence under
`build/access-e2e-run.*`.

See [`../demo/config.access.yaml`](../demo/config.access.yaml) and
[`../demo/access.acf`](../demo/access.acf) for a complete opt-in example.
