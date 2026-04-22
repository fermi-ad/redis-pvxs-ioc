# Releasing `redis-pvxs-ioc`

## Version source of truth

- `VERSION` is authoritative.
- `CMakeLists.txt` reads `VERSION` and sets the CMake project version from it.
- `redis-pvxs-ioc --version`, Docker image tags, and GitHub Release names must match `VERSION`.

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
7. Record the pushed digest.
8. Create the GitHub Release and include:
   - the semver tag
   - the immutable image digest
   - the validation commands used

## Manual build and publish

```sh
VERSION="$(cat VERSION)"
REVISION="$(git rev-parse --short=12 HEAD)"
IMAGE="adregistry.fnal.gov/instrumentation/redis-pvxs-ioc:${VERSION}"

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
```

Capture the immutable digest after push:

```sh
docker image inspect "${IMAGE}" --format '{{join .RepoDigests "\n"}}'
```

## Manual GitHub release

```sh
VERSION="$(cat VERSION)"
gh release create "v${VERSION}" \
  --repo fermi-ad/redis-pvxs-ioc \
  --title "v${VERSION}" \
  --notes-file CHANGELOG.md
```

After the release is created, edit the release notes to add the final image digest and the tested validation commands.
