# MVP Spec

## Product contract

- The binary is `redis-pvxs-ioc`.
- `redis-pvxs-ioc --config <path>` starts the service.
- `redis-pvxs-ioc --check-config <path>` validates config and exits.
- `SIGHUP` reloads the active config file.
- The runtime image loads `/etc/redis-pvxs-ioc/config.yaml` by default.

## Config contract

- YAML only.
- Top-level sections are `server`, `redis`, optional `alarms`, and `pvs`.
- Each PV declares `name`, `type`, `shape`, `read`, optional `write`, optional `confirm`, optional `metadata`, optional `alarm`, and optional `transform`.
- Only Redis routes are supported in the MVP.
- The Redis backend target is a standalone single-node Redis server.
- The current vendored `redis-adapter` is used in direct single-node mode with a local patch that skips cluster probing.
- Reader keys must be unique across `read.key` and `confirm.key` subscriptions in a config generation.

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
- Alarm transitions are also published to a Redis stream, default `acorn:alarms`.
- Array PVs do not evaluate threshold alarms in the MVP.

## Transform strategy

- Linear transforms are supported for `float32`, `float64`, and arrays of those types.
- YAML tags are `transform.kind: linear`, `transform.scale`, and `transform.offset`.
- The transform is applied on read from Redis and inverted on write to Redis.
- Clients see only the served PVA value. `display`, `control`, `units`, and `valueAlarm` metadata are configured in served units.
- Logarithmic and exponential transforms are out of scope for the MVP.

## Acceptance criteria

- The process serves configured PVs from Redis without an EPICS IOC database layer.
- A bad replacement config does not terminate the running process or replace the live config.
- Reloading can add PVs, remove PVs, and replace writable/readback behavior without restarting the process.
- Metadata fields expected by Phoebus are populated on numeric PVs.
- The demo Docker image and compose stack start successfully with the sample config.
- Redis Cluster support is out of scope for the MVP.
