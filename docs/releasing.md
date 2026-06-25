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
5. Let CI and downstream candidate-image testing pass on the PR branch.
6. Merge the release prep to `main`.
7. Tag the merged `main` release commit with `v$(cat VERSION)`.
8. Let the release workflow build and push the production image from that tag.
9. Build and push the legacy sidecar sample image when `legacy-sidecar/` changes.
10. Record the pushed digests from the workflow summary, GitHub Release, or manual sidecar push.
11. Confirm the GitHub Release includes:
   - the semver tag
   - the immutable image digest for each published image
   - the validation commands used
12. Pin production compose defaults as `image:v${VERSION}@sha256:<digest>`.

## Automated workflows

The primary runtime image release process is split into three workflows:

- `Validate redis-pvxs-ioc image` runs on PRs, pushes to `main`, and manual
  dispatch. It builds the image from the repo root with
  `REDIS_PVXS_IOC_VERSION=$(cat VERSION)` and
  `REDIS_PVXS_IOC_REVISION=<source revision>`, validates image labels, checks
  `redis-pvxs-ioc --version`, and validates the default runtime config with
  `--check-config`. It does not push registry tags.
- `Publish redis-pvxs-ioc candidate image` is manual-only. Use it for
  integration testing before merge. It pushes only a non-production candidate
  tag such as `candidate-<ref>-<sha>` and never updates `latest` or `vX.Y.Z`.
- `Publish redis-pvxs-ioc release image` runs on `v*` tag pushes or manual
  dispatch with an existing tag. It verifies `vX.Y.Z` matches `VERSION`,
  verifies the tag commit is contained in `origin/main`, builds and pushes
  `vX.Y.Z` plus `latest`, captures the digest, validates the pushed image, and
  creates or updates the GitHub Release.

The legacy sidecar image is still published with the manual sidecar commands
below when `legacy-sidecar/` changes. Standardizing that sidecar into the same
automated release workflow should be handled as a separate scoped change.

## Image reference policy

- Development and local experiments may use `:latest` or branch/test tags.
- Integration may use candidate tags while validating a PR or release branch.
- Production compose files must use `:v${VERSION}@sha256:<digest>`.

Tags describe releases. Digests define deployments. The tag keeps the human-readable release intent visible; the digest guarantees the exact artifact that runs.

## Manual build and publish fallback

Prefer the release workflow for normal runtime image releases. Use this fallback
only when the workflow is unavailable, and keep the same release identity checks.

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
