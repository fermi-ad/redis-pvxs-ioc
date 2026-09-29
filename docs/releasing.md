# Releasing `redis-pvxs-ioc`

## Identity and platforms

`VERSION` is authoritative. It accepts full SemVer, including prerelease and
build identifiers. CMake uses the numeric core; the binary and OCI label preserve
the complete version. Git and GitHub Release tags are `v${VERSION}`. Docker tags
replace the SemVer `+` separator with `_`, because Docker tags cannot contain `+`.
For example, `v0.9.0-rc.1+build.2` uses image tag `v0.9.0-rc.1_build.2`.

0.9.0 publishes Linux amd64 images. Native macOS development checks remain part
of qualification; existing Linux arm64 checks are informational. ARM image
publication and soak qualification may be reconsidered for 1.0.0 when a real
deployment needs them. Fleet deployment is a separate task.

## Prepare, validate, then promote

1. Review and merge release preparation into `main`, including `VERSION`, the
   matching `CHANGELOG.md` entry, compatibility documentation, license and
   dependency notices, and regression evidence. Keep branch protections and
   independent Instrumentation code-owner approval.
2. Dispatch **Publish redis-pvxs-ioc candidate image** from that exact merged
   revision. An isolated, digest-pinned BuildKit builder on adlinux3 builds and
   pushes only a unique `candidate-<run>-<attempt>-<commit>` tag, including an
   SPDX SBOM and full build provenance. This tag is unqualified until the run
   passes. It then pulls the immutable digest, validates Linux amd64 identity
   and default configuration, and runs isolated Redis/PVA and access-control
   tests against that digest. A successful run retains `candidate.json`,
   attestations, source revisions and validation logs in the
   `release-candidate-<run>-<attempt>` workflow artifact. Candidate publication
   never changes a stable release tag or `latest`.
3. For stable 0.9.0 and later, complete the separate qualification workflow on
   the same commit and digest: integration/sanitizer/native checks, 600 exact
   1080p Mono8 frames, capacity evidence, a 24-hour amd64 soak, and rollback to
   the saved 0.8.2 image/configuration. The promotion tool requires the successful
   `.github/workflows/qualify-image.yml` run and its
   `release-qualification-<run>-<attempt>/qualification.json` artifact. Until that
   qualification workflow and all evidence exist, stable promotion fails closed.
4. Tag that exact merged commit with `v${VERSION}` and push the tag. Tag pushes
   do not build or publish an image.
5. Dispatch **Publish redis-pvxs-ioc release image** from `main`, supplying the
   tag, candidate workflow run ID, and applicable qualification workflow run ID.
   It verifies tag ancestry before executing the release code; validates successful
   workflow identity, source, attempt, version and digest; and rechecks the pulled
   image and smoke behavior before any release tag is written.
6. Promotion copies the already validated registry manifest. No rebuild occurs.
   Existing release tags cannot be replaced by a different digest. Prereleases
   never change `latest`, and publishing an older stable version cannot move
   `latest` backward. The GitHub Release includes its changelog, source, immutable
   image, validation links, evidence JSON, and `release-evidence.tar.gz` containing
   the candidate and qualification artifacts. The complete image index, including
   its attestations, retains the validated digest during promotion.
7. Open and merge a post-release pin-sync PR updating every checked-in main-runtime
   image example to `image:v${VERSION}@sha256:<digest>`. Keep the independently
   versioned historical sidecar image unchanged.

A final release candidate must already contain its final version: qualify
`VERSION=0.9.0` before publishing v0.9.0. A `0.9.0-rc.1` binary cannot be relabeled
as `0.9.0`. Any runtime/dependency change creates a new candidate and invalidates
qualification of the prior digest.

## Build evidence

The candidate workflow pins the BuildKit and SBOM scanner images by digest.
Docker's [SBOM](https://docs.docker.com/build/metadata/attestations/sbom/) covers
the runtime and builder stages. The source inventory records every recursive
Git submodule revision, because package scanning cannot identify every static
C++ dependency. Dirty checkouts and uninitialized or changed gitlinks fail
before building. Full [provenance](https://docs.docker.com/build/metadata/attestations/slsa-provenance/)
retains build arguments and the build definition; credentials must never be
passed as build arguments.

Candidate records bind the retained source inventory, SBOM, provenance and
registry manifest by SHA-256. Promotion checks these files before publishing.
The attestations describe the build and are bound to the image digest; they
are not an independent code review or a claim of a particular SLSA assurance
level. Validation logs live with GitHub Releases after the workflow artifact's
90-day retention window expires.

## Image pull and smoke isolation

Remote mutable tags are always pulled. Immutable digests are pulled when absent.
For a locally built image, explicitly select the local-only policy:

```sh
REDIS_PVXS_IOC_IMAGE=redis-pvxs-ioc:local \
  REDIS_PVXS_IOC_PULL_POLICY=never ./scripts/smoke-test.sh
```

`auto` is the default; `always` forces a pull; `never` requires the image to
already exist and cannot silently fetch a remote replacement. The same policies
are available for the fixture Redis image through `REDIS_IMAGE_PULL_POLICY`.

Smoke tests use a generated Compose project, their own Compose file and temporary
configuration, and no published host ports. Production/demo project and container
name overrides do not change their ownership. Logs remain under `build/smoke-run.*`;
cleanup removes only the test's own services and temporary configuration.

## Recovery and manual operation

If validation fails, preserve its logs, fix the linked issue, and qualify a new
candidate. Do not overwrite a published release tag or bypass promotion with a
manual build/push. The same checked-in promotion tool may be run by a maintainer
with `GH_TOKEN`, registry authentication, and a checkout of the reviewed tag:

```sh
python3 scripts/release-image.py promote --tag v0.8.2 --candidate-run RUN_ID
```

Stable 0.9.0 additionally requires `--qualification-run RUN_ID`. The promotion
mechanism uses Docker's [manifest-copy behavior](https://docs.docker.com/reference/cli/docker/buildx/imagetools/create/)
with `--prefer-index=false` and verifies the destination digest after each tag.

Production references include both tag and digest. Rollback restores the previous
immutable image and its saved compatible configuration, including the ACF files;
it does not run the previous image against a newer incompatible configuration.
