# XRM inventory sidecar

`xrm-proxy` discovers the stock XRM PVA servers from a numeric IPv4 address and
publishes the channel map consumed by `pva_gateway.file`. The sidecar handles
inventory and naming; `redis-pvxs-ioc` handles downstream PVA operations and ACF.
An optional independent worker also demultiplexes configured Redis raw streams;
stock PVA mirroring does not copy values through Redis.

```sh
xrm-proxy --ip 10.200.8.253 --prefix SL:INST:ICM:XRM \
  --manifest /var/lib/xrm-proxy/gateway.yaml \
  --compat-aliases /etc/xrm-proxy/aliases.yaml
```

The default prefix is empty. A supplied prefix gains a trailing colon if needed.
Every canonical name includes the logical role and the complete, case-sensitive
stock name:

```text
BASE:acq2206-097:0:MODEL
APP:acq2206_597:FMT:TAB:Tbl
```

With the example prefix, these become
`SL:INST:ICM:XRM:BASE:acq2206-097:0:MODEL` and
`SL:INST:ICM:XRM:APP:acq2206_597:FMT:TAB:Tbl`. Role qualification keeps two
channels with the same native name on different stock IOCs distinct. Neither
the discovered TCP port nor the server GUID appears in a public PV name.

## Discovery and recovery

The PVXS client configuration is constructed explicitly. Automatic address lists
are off, TCP name servers are empty, and active discovery targets only the named
IP on UDP 5076. A PVA empty SEARCH with MustReply discovers the current TCP
listeners. Only TCP endpoints and packet origins on the named IP are accepted.
The sidecar queries the built-in `server` RPC with `op=channels` at those
listeners. It does not use SSH, scan TCP ports, open an acquisition socket,
issue a PUT, or call any device-control RPC. Port 4210 is explicitly forbidden.

PVA loopback multicast fanout on the device must deliver directed discovery to
both stock IOCs, and both must support the `server` channel enumeration RPC.
Discovery cannot promise a complete catalog if the stock firmware or network
does not provide those capabilities. Exactly one BASE inventory and one APP
inventory must be present. BASE is identified by the `:0:MODEL` and `:0:SERIAL`
suffixes; APP by `:FMT:TAB:Tbl` and `:SOE_HLD:TAB:Tbl`. A missing, ambiguous,
unclassified, duplicate, or failed inventory rejects the refresh. These anchors
reflect the stock XRM application; firmware with different anchors needs an
explicit implementation update rather than a guessed classification.

Successful inventories refresh every 60 seconds by default. Failed attempts retry
every 5 seconds. Each discovery window and each server RPC has a 3 second default
timeout. `--refresh-ms`, `--retry-ms`, `--discovery-timeout-ms` and
`--request-timeout-ms` set bounded intervals. `--discovery-port` permits a known
nondefault PVA UDP listener, including an isolated fixture; its default remains
5076. No alternative TCP port is guessed when the application restarts.

The latest successful manifest persists on disk and remains untouched after a
failed refresh or sidecar restart. Complete successful inventories may remove
stock names that the device no longer lists; missing compatibility sources
reject the update to prevent silently dropping a friendly alias. A port change
updates source endpoints while preserving names. Channel ordering changes do
not rewrite the manifest or change its revision. Revisions are deterministic
change identifiers and are not authentication checksums.

## Compatibility aliases

A trusted optional file preserves existing friendly names. These aliases are
exact exported names; `--prefix` is not added to them.

```yaml
schema_version: 1
aliases:
  - role: BASE
    upstream: acq2206-097:0:MODEL
    aliases: [SL:INST:ICM:XRM:carrier:MODEL]
  - role: APP
    upstream: acq2206_597:FMT:TAB:Tbl
    aliases: [SL:INST:ICM:XRM:application:FMT:TAB:Tbl]
```

Only `BASE` and `APP` are accepted. Every compatibility source must exist in the
complete inventory with its exact native spelling, including prefix case. Role
classification does not assume a prefix's case, and exported canonical names
preserve the native case. If firmware changes a prefix, update trusted
compatibility upstreams from the fresh inventory while retaining the friendly
alias strings; no implicit case-insensitive substitution is performed.
Canonical/alias collisions, duplicate entries, unknown
fields and duplicate YAML keys are rejected. No access group or policy may be
specified in this file or the generated manifest.

The output contract is:

```yaml
schema_version: 1
revision: xrm-10.200.8.253-0123456789abcdef
pvs:
  - name: SL:INST:ICM:XRM:BASE:acq2206-097:0:MODEL
    upstream: acq2206-097:0:MODEL
    server: 10.200.8.253:5075
    aliases: [SL:INST:ICM:XRM:carrier:MODEL]
```

Set the consuming IOC's `server.namespace` to empty when these canonical names
already contain the full project prefix. Gateway aliases are exact names and
do not receive the IOC namespace. ACF assignments belong in the consuming IOC's
`pva_gateway.access_overrides` or defaults; sidecar inventory never changes
policy or global IOC settings.

## Publication and deployment

Run the sidecar as a separate nonroot process beside the IOC on a network that
reaches the named XRM. The ICM Compose deployment can share the existing
Redis-owned network namespace. Give the sidecar read-only access to its alias
file and write access to a dedicated generated directory. Mount that directory
read-only into the IOC as a directory, so atomic file replacements are visible.
No Docker socket, shared PID namespace, or write access to the IOC policy is
required.

The sidecar holds an advisory exclusive lock at `<manifest>.lock`; a second
writer exits. It validates both role inventories and all names before creating
a temporary file in the destination directory, synchronizes it, atomically
renames it over the manifest, and synchronizes the directory. Existing symlink
manifest targets are rejected. An unchanged catalog leaves the original file
and inode in place. The consuming IOC independently validates the manifest,
stages a complete generation with its existing ACF, and retains its active
generation if staging fails. Publishing the file is not proof that the IOC has
activated it; inspect the IOC's gateway/configuration status after publication.

Input files and generated output are limited to 16 MiB, each server inventory to
100,000 names, the combined canonical and alias catalog to 100,000 names, and
discovery to 8 endpoints.
Native names and aliases are limited to 1,024 bytes and may not contain whitespace
or control characters. These checks bound catalog processing after PVA decoding;
they do not establish a wire-level incoming-message allocation limit.

## Isolated verification

`--once` exits successfully after one complete publication, or returns nonzero
after a rejected attempt. `--inventory FILE` replaces all PVA discovery/RPC
activity with a bounded offline inventory. With the optional Redis worker
disabled, it performs no network operations:

```yaml
schema_version: 1
servers:
  - server: 192.0.2.10:5075
    channels: [acq2206-097:0:MODEL, acq2206-097:0:SERIAL, shared]
  - server: 192.0.2.10:44000
    channels: [acq2206_597:FMT:TAB:Tbl, acq2206_597:SOE_HLD:TAB:Tbl, shared]
```

```sh
xrm-proxy --ip 192.0.2.10 --inventory inventory.yaml \
  --manifest /tmp/xrm-gateway.yaml --once
python3 tests/xrm_proxy_cli_tests.py /path/to/xrm-proxy
```

The CLI fixtures cover cross-role duplicate native names, compatibility aliases,
prefixes, stable revisions, application-port changes, complete-catalog size,
single-writer ownership, retained last-good files, and continuous recovery.
They do not substitute for qualifying directed discovery and operation
forwarding with isolated PVA servers.

## Optional Redis demultiplexing

`--demux-config FILE` enables the sidecar's independent Redis raw-stream worker.
With no option, the worker is disabled. It consumes only configured Redis input
streams and uses the configured static sample layout, output mappings,
checkpoint/idempotency rules and retention limits. It does not open a hardware
stream, enable a producer, change acquisition settings, or contact port 4210.
Source timestamps must come from the raw entry's declared timestamp field; a
missing or invalid source timestamp is marked INVALID rather than replaced with
the sidecar's clock.
See [Redis demultiplexing](xrm-demux.md) for the exact configuration and stream
contracts.

These demux outputs do not automatically create PVA waveform mappings. Only an
adapter that honors the acquisition timestamp and validity fields may serve
them as timestamped waveforms. A generic array adapter that derives timestamps
from Redis stream IDs is unsuitable for this contract.

```sh
xrm-proxy --ip 10.200.8.253 --prefix SL:INST:ICM:XRM \
  --manifest /var/lib/xrm-proxy/gateway.yaml \
  --compat-aliases /etc/xrm-proxy/aliases.yaml \
  --demux-config /etc/xrm-proxy/demux.yaml
```

The worker runs in its own thread while PVA discovery refreshes independently.
Malformed raw data and Redis connection failures are contained within that
worker. A missing or invalid static demux configuration is reported at startup
while the PVA mirror continues; correcting static configuration requires a
sidecar restart. Worker state, accepted/rejected entry counts, failure counts and
detected continuity gaps are reported in the sidecar log as inventory attempts
complete. These reports
are independent of the IOC's PVA gateway readiness.

Validate the selected static configuration before enabling it:

```sh
xrm-proxy --check-demux-config /etc/xrm-proxy/demux.yaml
```

This standalone mode returns nonzero for invalid configuration and performs no
PVA discovery, manifest writes, Redis connections or worker startup. It can run
inside the exact deployment image with networking disabled.

`--once` performs one inventory attempt and then stops the optional worker;
continuous operation is required for sustained demultiplexing. `--inventory`
replaces only PVA network discovery. Combined with `--demux-config`, it still
allows the explicitly configured Redis worker to run. Omit `--demux-config` for
fully network-free inventory tests.
