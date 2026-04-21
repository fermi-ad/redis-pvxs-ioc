# ADR 0004: Generation-Based Reload

## Status

Accepted

## Decision

Config applies are modeled as generation changes. A reload parses and validates the replacement config, stages any new PV runtimes, then swaps or reconfigures PVs while fencing stale backend callbacks and in-flight puts.

## Rationale

- The product promise is process continuity under config and backend changes.
- The hard failure mode is stale work from an old configuration mutating live state after a reload.
- Generation fencing is the minimum machinery needed to make hot reload believable.

## Consequences

- Each PV runtime carries a generation identity and an active/inactive state.
- Old callbacks no-op after retirement.
- In-flight puts fail fast when their generation is no longer authoritative.
- Reconfigurations that do not require backend rebind can update an existing PV in place.
