# Normative Types Roadmap

`redis-pvxs-ioc` currently serves `NTScalar` and `NTScalarArray` payloads only. The long-term product goal is broader alignment with the EPICS V4 Normative Types specification:

- general normative types such as `NTEnum`, `NTMatrix`, `NTURI`, `NTNameValue`, `NTTable`, and `NTAttribute`
- specific normative types such as `NTMultiChannel`, `NTNDArray`, `NTContinuum`, `NTHistogram`, and `NTAggregate`
- Appendix A additions such as `NTUnion` and `NTScalarMultiChannel`

Reference specification:

- [EPICS V4 Normative Types](https://docs.epics-controls.org/en/latest/pv-access/Normative-Types-Specification.html)

## Current State

- `NTScalar` and `NTScalarArray` are the only served normative-type families today.
- The current implementation focuses on scalar/scalar-array values with alarm, timestamp, display, control, and value-alarm metadata.
- There is no support yet for the other general or specific normative types defined in the EPICS document.
- There is no reusable internal framework yet for richer normative-type self-identification, validation, or union-heavy payload construction.

## Backlog Structure

### Umbrella

- [Issue #17](https://github.com/fermi-ad/redis-pvxs-ioc/issues/17): full normative-types roadmap umbrella beyond `NTScalar` and `NTScalarArray`

### Foundation

- [Issue #20](https://github.com/fermi-ad/redis-pvxs-ioc/issues/20): reusable normative-type framework for type IDs, metadata, validation, and unions

This is the prerequisite track for broader coverage. It should establish:

- consistent normative-type self-identification
- a clean internal builder/adapter layer for structured and union-bearing payloads
- validation hooks for richer normative-type payloads
- a path for optional metadata beyond the current scalar-focused implementation

### General Normative Types

- [Issue #18](https://github.com/fermi-ad/redis-pvxs-ioc/issues/18): `NTEnum`, `NTMatrix`, `NTURI`, `NTNameValue`, `NTTable`, `NTAttribute`

This track should cover the general types listed in the specification beyond scalar/scalar-array support.

### Specific Normative Types

- [Issue #19](https://github.com/fermi-ad/redis-pvxs-ioc/issues/19): `NTMultiChannel`, `NTNDArray`, `NTContinuum`, `NTHistogram`, `NTAggregate`

This track should cover the richer domain-specific structures in the specification, including array/image-heavy and structured aggregate payloads.

### Appendix A Additions

- [Issue #21](https://github.com/fermi-ad/redis-pvxs-ioc/issues/21): `NTUnion`, `NTScalarMultiChannel`

The specification treats these as possible future additions. They remain part of the long-range goal, but they should stay separate from the current core/general/specific normative-type implementation tracks.

## Implementation Guardrails

- Keep the runtime PVA-first.
- Do not reintroduce IOC database coupling just to serve richer normative types.
- Do not bypass validation and self-identification by falling back to ad-hoc payloads everywhere.
- Keep the generation-based reload model intact as richer normative types are added.
- Keep the control-plane work compatible with Redis-backed definitions/settings so richer types can participate in the same dynamic definition model over time.
