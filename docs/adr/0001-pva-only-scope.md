# ADR 0001: PVA-Only MVP Scope

> **Historical wording:** “MVP” names the original milestone. The continuing
> current decision is that the core runtime remains PVA-only; see
> [`../feature-state-roadmap.md`](../feature-state-roadmap.md).

## Status

Accepted

## Decision

The MVP serves PVAccess only. Channel Access is explicitly out of scope.

## Rationale

- The product priority is a high-performance PVXS data plane, not classic IOC compatibility.
- Phoebus, PVA archivers, and PVA-native tooling are satisfied by standard `NTScalar` and `NTScalarArray` responses with correct metadata, alarm, and timestamp fields.
- Avoiding CA in the MVP removes a large set of hidden assumptions around record semantics, DBR translation, and CA compatibility hosting.

## Consequences

- CA clients are unsupported in the MVP.
- Metadata, alarms, and transforms are designed around PVA/NT payloads first.
- Any future CA support must be added as a separate compatibility track, not by compromising the standalone runtime core.
