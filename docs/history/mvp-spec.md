# Historical MVP Specification

> **Status:** Historical implementation milestone. This file is preserved to
> explain the initial product contract; it is not the current specification.
> See [`../configuration.md`](../configuration.md), [`../operations.md`](../operations.md),
> and [`../feature-state-roadmap.md`](../feature-state-roadmap.md) for current behavior.

## Product contract

- The binary is `redis-pvxs-ioc`.
- `redis-pvxs-ioc --config <path>` starts the service.
- `redis-pvxs-ioc --check-config <path>` validates config and exits.
- `SIGHUP` reloads the active config file.
- The runtime image loads `/etc/redis-pvxs-ioc/config.yaml` by default.

## Config contract

- YAML only.
- Top-level sections are `server`, either `redis` or `redis_backends`, optional `alarms`, and `pvs`.
- Each PV declares `name`, `type`, `shape`, `read`, optional `write`, optional `confirm`, optional `metadata`, optional `alarm`, and optional `transform`.
- Only Redis routes are supported in the MVP.
- Redis backends target standalone single-node Redis servers.
- Legacy single-backend configs may omit route backend aliases; multi-backend configs must resolve every route to a configured backend alias.
- Multi-backend configs must also set `alarms.backend` explicitly; the runtime does not pick a default alarm-stream backend when more than one backend is configured.
- Reader keys must be unique per backend across `read` and `confirm` subscriptions in a config generation.

## PVA payload contract

- Public PVs are `NTScalar` or `NTScalarArray`.
- Every PV always exposes `value`, `alarm`, `timeStamp`, and `display`.
- Numeric PVs also expose `control` and `valueAlarm`.
- `display.form.choices` is always the standard 7-choice list expected by Phoebus.
- Units are a plain string.
- Transforms are internal to the server; clients see only the served value.

## Alarm strategy

- Scalar numeric PVs evaluate warning/alarm thresholds locally in the runtime.
- Alarm transitions are reflected in the PVA `alarm` fields.
- Alarm transitions are also published to a Redis stream on the configured `alarms.backend`, default `acorn:alarms`.
- Redis alarm stream events follow the `fermi-ad/epics-alarm-push` schema from commit `cfbee1e110cf8b08c79c5faf604e0a859bcffbfe`: `device`, `source`, `severity`, `timestamp`, `detail`, and optional `message`.
- With one configured backend, `alarms.backend` defaults to that backend. With multiple configured backends, `alarms.backend` is required.
- Array PVs do not evaluate threshold alarms in the MVP.

## Transform strategy

- Linear transforms are supported for `float32`, `float64`, and arrays of those types.
- YAML tags are `transform.kind: linear`, `transform.scale`, and `transform.offset`.
- The transform is applied on read from Redis and inverted on write to Redis.
- Clients see only the served PVA value. `display`, `control`, `units`, and `valueAlarm` metadata are configured in served units.
- Logarithmic and exponential transforms are out of scope for the MVP.

## Acceptance criteria

- The process serves configured PVs from Redis without an EPICS IOC database layer.
- The process supports multiple Redis backends in one config generation.
- A bad replacement config does not terminate the running process or replace the live config.
- Reloading can add PVs, remove PVs, and replace writable/readback behavior without restarting the process.
- Metadata fields expected by Phoebus are populated on numeric PVs.
- The demo Docker image and compose stack start successfully with the sample config.
- Redis Cluster support is out of scope for the MVP.
