# ADR 0002: Pinned Dependency Model

## Status

Accepted

## Decision

The repo vendors its build-time dependencies through pinned git submodules under `third_party/`.

## Rationale

- The artifact trail needs exact EPICS, PVXS, Redis, and YAML revisions attached to the product.
- The runtime depends on local EPICS/PVXS branch behavior that is not yet treated as an upstream release baseline.
- Container builds should not fetch arbitrary moving heads.

## Consequences

- `epics-base`, `pvxs`, `redis-adapter`, and `yaml-cpp` are versioned with the repo.
- Docker builds assume the submodules are already populated in the build context.
- Updating dependency behavior becomes an explicit repo change with a visible SHA delta.
- The submodule URLs now point at published remotes, and the repo still depends on those remotes preserving the pinned revisions.
