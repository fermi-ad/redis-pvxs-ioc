# Releasing `redis-pvxs-ioc`

## Version source of truth

- `VERSION` is authoritative.
- `CMakeLists.txt` reads `VERSION` and sets the CMake project version from it.
- `redis-pvxs-ioc --version` must match `VERSION`.
- Git tags, GitHub Release names, and release image tags use `v${VERSION}`.

## Semver rules

- Bump `MAJOR` for incompatible config or runtime contract changes.
- Bump `MINOR` for backward-compatible feature additions.
- Bump `PATCH` for fixes, documentation-only release adjustments, or packaging changes that do not change the public contract.

## Release checklist

1. Update `VERSION`.
2. Add or update the matching entry in `CHANGELOG.md`.
3. Run local verification:

```sh
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/redis-pvxs-ioc --version
./scripts/smoke-test.sh
```

4. Commit the release prep and push the branch.
5. Tag the release commit with `v$(cat VERSION)`.
6. Build and push the production image.
7. Build and push the legacy sidecar sample image when `legacy-sidecar/` changes.
8. Record the pushed digests.
9. Create the GitHub Release and include:
   - the semver tag
   - the immutable image digest for each published image
   - the validation commands used
10. Pin production compose defaults as `image:v${VERSION}@sha256:<digest>`.

## Image reference policy

- Development and local experiments may use `:latest` or branch/test tags.
- Integration may use `:v${VERSION}` while validating a release.
- Production compose files must use `:v${VERSION}@sha256:<digest>`.

Tags describe releases. Digests define deployments. The tag keeps the human-readable release intent visible; the digest guarantees the exact artifact that runs.

## Manual build and publish

```sh
VERSION="$(cat VERSION)"
REVISION="$(git rev-parse --short=12 HEAD)"
IMAGE="adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:v${VERSION}"
LEGACY_IMAGE="adregistry.fnal.gov/instrumentation/redis-pvxs-ioc-legacy-sidecar:v${VERSION}"

git tag "v${VERSION}"
git push origin HEAD
git push origin "v${VERSION}"

docker build \
  --platform linux/amd64 \
  --build-arg REDIS_PVXS_IOC_VERSION="${VERSION}" \
  --build-arg REDIS_PVXS_IOC_REVISION="${REVISION}" \
  --build-arg REDIS_PVXS_IOC_SOURCE="https://github.com/fermi-ad/redis-pvxs-ioc" \
  -t "${IMAGE}" \
  -t adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:latest \
  .

docker push "${IMAGE}"
docker push adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:latest

LEGACY_IOC_IMAGE="${LEGACY_IMAGE}" \
  docker compose \
    -f docker-compose.yml \
    -f docker-compose.legacy-sidecar.yml \
    -f docker-compose.legacy-sidecar.build.yml \
    --profile legacy \
    build legacy-ioc

LEGACY_IOC_IMAGE="${LEGACY_IMAGE}" \
  docker compose \
    -f docker-compose.yml \
    -f docker-compose.legacy-sidecar.yml \
    -f docker-compose.legacy-sidecar.build.yml \
    --profile legacy \
    push legacy-ioc

docker tag "${LEGACY_IMAGE}" adregistry.fnal.gov/instrumentation/redis-pvxs-ioc-legacy-sidecar:latest
docker push adregistry.fnal.gov/instrumentation/redis-pvxs-ioc-legacy-sidecar:latest
```

Capture the immutable digest after push:

```sh
docker image inspect "${IMAGE}" --format '{{join .RepoDigests "\n"}}'
docker image inspect "${LEGACY_IMAGE}" --format '{{join .RepoDigests "\n"}}'
```

When updating deployment compose files, keep the tag and digest together:

```sh
image: adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:v${VERSION}@sha256:<digest>
image: adregistry.fnal.gov/instrumentation/redis-pvxs-ioc-legacy-sidecar:v${VERSION}@sha256:<digest>
```

## Manual GitHub release

```sh
VERSION="$(cat VERSION)"
gh release create "v${VERSION}" \
  --repo fermi-ad/redis-pvxs-ioc \
  --title "v${VERSION}" \
  --notes-file CHANGELOG.md
```

After the release is created, edit the release notes to add the final image digests and the tested validation commands.
