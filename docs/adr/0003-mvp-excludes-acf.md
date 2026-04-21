# ADR 0003: MVP Excludes ACF Enforcement

## Status

Accepted

## Decision

The MVP does not implement ACF parsing or access-security enforcement.

## Rationale

- The first deliverable is a standalone writable hot-reload core with Redis-backed PV serving.
- ACF support has meaningful product and dependency implications and should land on top of a proven runtime, not inside the initial bring-up.
- The design doc keeps ACF as a first-class product requirement after MVP.

## Consequences

- MVP deployments should be treated as trusted-network or lab deployments.
- ACF-related local branch behavior remains part of the dependency baseline and future roadmap, but not the first milestone.
- The product interfaces and admin namespace should avoid assumptions that would block later ACF integration.
