# Live ACF Acceptance and Rollback Runbook

Use this runbook when a representative Fermilab IOC host is reachable. Begin
with a non-production instance. Do not recreate a production-like service,
change a mounted policy, or perform root-level writes without explicit
approval.

## Local gate before handoff

Before publishing a candidate, repeat the repository-local gates from a clean
checkout:

```sh
docker build --progress=plain -t redis-pvxs-ioc:acf-local .
REDIS_PVXS_IOC_IMAGE=redis-pvxs-ioc:acf-local \
  REDIS_IMAGE=redis:7-alpine ./scripts/smoke-test.sh
./scripts/access-e2e-test.sh
ITERATIONS=20000 REPETITIONS=3 sh scripts/benchmark-access.sh
```

The image build runs CTest. The smoke test protects the default access-disabled
path, while the end-to-end script covers enabled ACF behavior and preserves
monitor, IOC, Redis, and access-status-PV evidence under
`build/access-e2e-run.*`. The benchmark preserves environment metadata and
every raw sample under `build/access-benchmark-report` unless `REPORT_DIR` is
overridden.

The workstation image is an ARM64 local artifact. Commit and review the exact
worktree, then build and publish the server architecture on the authoritative
build host. Record the immutable registry digest; do not deploy the workstation
tag.

## Scope to record

- Target host and IOC container or service.
- Authoritative Compose project, files, and working directory.
- Candidate image tag and digest.
- Mounted YAML configuration and ACF paths.
- Writable Redis-backed test PV, test identities, and test RPC.
- Unrelated containers and services that are explicitly out of scope.

## Read-only preflight

Use a bounded SSH probe:

```sh
ssh -o BatchMode=yes -o ConnectTimeout=10 \
    -o ServerAliveInterval=5 -o ServerAliveCountMax=1 HOSTNAME \
    'hostname; docker ps --format "{{.Names}} {{.Image}} {{.Status}}"'
```

Identify the authoritative deployment before changing anything:

```sh
docker inspect IOC_CONTAINER
docker inspect -f '{{json .Config.Labels}}' IOC_CONTAINER
docker inspect -f '{{json .Mounts}}' IOC_CONTAINER
docker image inspect CURRENT_IMAGE
sha256sum CONFIG_PATH ACF_PATH
```

Record the current container and image IDs or digests, Compose labels, mounts,
configuration hashes, network and IP, access/config generations, test value,
and latest Redis stream ID. Confirm that the ACF is a separately writable
mount if watcher testing will modify it.

Validate the candidate configuration without starting the IOC:

```sh
docker run --rm -v CONFIG_DIRECTORY:/config:ro CANDIDATE_IMAGE \
    --check-config /config/config.yaml
```

## Approved cutover

After approval, preserve the YAML, ACF, and current image digest. Recreate only
the named IOC service:

```sh
docker compose -p COMPOSE_PROJECT -f COMPOSE_FILE up -d --no-deps IOC_SERVICE
```

Confirm the selected image, mounts, IP, PVA listener, and service health. An
enabled instance must start with access generation `1`.

## Acceptance matrix

1. **Discovery and initial rights:** list protected names; exercise allowed and
   denied GET, GET_FIELD, monitor, PUT, and RPC operations. Prove a denied PUT
   does not change Redis and a denied RPC does not reach its backend.
2. **Dedicated reload:** reload unchanged policy and confirm generation advances
   with a stable fingerprint. Remove access while a monitor is connected,
   confirm it disconnects and reconnect remains denied, then restore access.
3. **Whole configuration and recovery:** reload through the configuration PV
   and SIGHUP. Confirm both generations advance. Attempt to change
   `access.enabled` and confirm the reload fails with both generations intact.
4. **Watcher:** cover in-place write, atomic replacement, invalid or partial
   content, deletion, identical restoration without duplicate activation, and
   watcher-disabled behavior.
5. **Credentials, HAG, and audit:** exercise account and `role/<group>`
   credentials. Use a deterministic site fixture for HAG TTL/address changes.
   Verify TRAPWRITE fields, denial log limiting, and exact counters.
6. **Status and performance:** inspect every access administration PV. Run all
   four benchmark modes and archive the raw data, environment, and generated
   report without applying a hard regression threshold.

## Site-only closeout checks

Local tests cover policy parsing, macros, ASG/ASL assignments, account and role
decisions, aggregate credentials, deterministic HAG refresh callbacks, actual
PVA operations, Redis non-mutation, watchers, reload paths, counters, auditing,
and performance. The following checks require a representative controls host:

- Confirm the real PVA authentication method, operator account, and
  `role/<group>` mapping using both an authorized and unauthorized identity.
- Exercise a site DNS HAG entry through a real TTL/address transition while a
  client remains connected; compare the observed timing with the pinned EPICS
  TTL/retry behavior.
- Deny a reflected RPC backed by the deployed gRPC service and prove from
  backend evidence that its handler was not invoked.
- Validate PV discovery and the normal operator-facing client across the real
  multicast/unicast network path.
- Measure representative host load and reconnect disruption, then exercise or
  explicitly waive the prepared rollback before broader rollout.

## Rollback

Rollback triggers include startup failure, incorrect identity mapping,
administrative lockout without SIGHUP recovery, a denied write reaching Redis,
monitor revocation failure, inconsistent generations, or unacceptable service
disruption.

Restore the preserved YAML and ACF and select the previous image digest. Recreate
only the IOC service with the same Compose command. Verify the previous service
state and retain failure evidence; do not perform broad host or Docker cleanup.

## Report

Record the outcome, scope, before state, actions, verification, whether rollback
was exercised, and any skipped checks. Live acceptance is not complete until an
operator client passes and Redis non-mutation is proven for a denied write.
