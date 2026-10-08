# ADR 0004: Generation-Based Reload

## Status

Accepted

## Decision

Config applies are modeled as generation changes. A reload prepares runtimes,
metadata, access members and policy bytes, the complete endpoint registry, and
the discovery catalog before changing live state. Policy activation is the last
fallible gate. Prepared runtime configuration swaps and one endpoint snapshot
publication commit the generation; old endpoint leases are fenced before their
channels are disconnected.

## Rationale

- The product promise is process continuity under config and backend changes.
- The hard failure mode is stale work from an old configuration mutating live state after a reload.
- Generation fencing is the minimum machinery needed to make hot reload believable.

## Consequences

- Each PV runtime carries a generation identity and an active/inactive state.
- Old callbacks no-op after retirement.
- In-flight puts fail fast when their generation is no longer authoritative.
- Reconfigurations that do not require backend rebind can update an existing PV in place.
- Alias removal closes only the removed name's channels, preserving monitors on
  the canonical name and unchanged aliases.
- Rejected preparation/final activation leaves values, policy, catalog and
  pending operations in the existing generation. Metadata refresh after commit
  uses the latest source sample, including updates received during preparation.
- Post-commit notification failures are operational errors in the committed
  generation; they do not claim that an already published generation rolled back.
