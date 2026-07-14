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
3. Confirm the project license and every vendored source tree are cleared for
   public redistribution and inventoried in `THIRD_PARTY_NOTICES.md`.
4. Run local verification:

```sh
cmake -S . -B build
cmake --build build -j"$(nproc)"
ctest --test-dir build --output-on-failure
./build/redis-pvxs-ioc --version
./scripts/smoke-test.sh
```

5. Commit the release prep and push the branch.
6. Let public CI and downstream candidate-image testing pass on the PR branch.
7. Merge the release prep to `main`.
8. Tag the merged `main` release commit with `v$(cat VERSION)`.
9. Let the release workflow build and push the production image from that tag.
10. Record the runtime digest from the workflow summary and GitHub Release.
11. Confirm the GitHub Release includes:
   - the semver tag
   - the immutable runtime image digest
   - the validation commands used
12. Open a post-release pin-sync PR that replaces every checked-in main-runtime
    image example with `image:v${VERSION}@sha256:<digest>`.
13. Validate and merge that PR. Do not change the legacy-sidecar tag unless a
    separate sidecar release was intentionally built and validated.

## Automated workflows

The primary runtime image release process is split into three workflows:

- `Validate redis-pvxs-ioc image` runs pull requests on a GitHub-hosted runner
  without Fermilab credentials. Pushes to `main` and manual dispatches run on
  `adlinux3`, matching release infrastructure. It builds the image with
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

The legacy sidecar has its own version and immutable digest. It is published
only as a separate, intentional sidecar release. Standardizing it into an
automated workflow is a separate scoped change.

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

LEGACY_IMAGE="${LEGACY_IOC_IMAGE:?set the independently versioned legacy sidecar image}"

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
image: adregistry.fnal.gov/instrumentation/redis-pvxs-ioc-legacy-sidecar:<sidecar-version>@sha256:<digest>
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

Then open the post-release pin-sync PR. A tag identifies the intended release;
only the published digest can identify the exact artifact in checked-in examples.
