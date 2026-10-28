# Path to v1.0.0

[Issue #144](https://github.com/fermi-ad/redis-pvxs-ioc/issues/144) tracks the
release scope, decisions, implementation PRs and qualification evidence. The
[v0.9 roadmap](roadmap-v0.9.md) is the historical planning record for the released
version. This branch begins 1.0 preparation; it does not establish release
qualification or change `VERSION`.

## Platform decisions

Linux amd64 is the primary required release and validation target. Removing
macOS CI and its forward qualification requirement is the early prerequisite
tracked in [#145](https://github.com/fermi-ad/redis-pvxs-ioc/issues/145). Historical
release evidence and existing Darwin build code are retained.

Existing Linux arm64 CI remains informational. Linux arm64 image publication
and qualification are an explicit optional 1.0 scope decision, currently leaning
toward exclusion. No arm64 expansion is part of the prerequisite. If platform
support expands, prioritize Linux arm64 ahead of macOS and document the deployment
need before adding release gates.

## Branch and review structure

| Branch or PR | Purpose |
| --- | --- |
| `main` | Reviewed mainline and the source for final release qualification. |
| `dev/v1.0.0` | Shared integration branch for reviewed 1.0 changes. |
| Focused `dev/*` PRs into `dev/v1.0.0` | Review and test each change before integration. |
| Draft `dev/v1.0.0` PR into `main` | Review the combined changes and land the batch with one merge. |

Keep the umbrella PR linked from #144 and maintain its list of constituent PRs.
Each focused PR links its owning issue and #144, describes its validation, and
targets `dev/v1.0.0`. Review dependent changes in order; integrate their
prerequisites before branching the next dependent change. Independent changes
can proceed in parallel.

The integration branch requires an Instrumentation code-owner approval and
successful Linux amd64 minimal, sanitizer and image validation. The existing
workflows already validate PRs targeting `dev/**`; Linux arm64 remains
informational. Merge reviewed changes into the integration branch as they are
ready. Mainline receives the accumulated batch only when the umbrella PR is
ready and its required review and CI checks pass. Use a merge commit for that
final merge to retain the constituent history.

The macOS CI prerequisite is an exception to the batch: open it against `main`
so runner retirement does not block current mainline validation. After it merges,
bring it into `dev/v1.0.0` through a synchronization PR from `main`. Bring later
mainline fixes across the same way; do not force-push or rebase the shared
integration branch. Keep focused PRs current with their target before merging.

Use `Refs #...` in focused PRs and track "integrated" separately from "released"
in #144. Close completed implementation issues when their changes reach `main`;
merging a PR into the integration branch alone does not complete release work.

## Final integration and qualification

Before marking the umbrella PR ready, synchronize current `main`, reconcile all
required roadmap items or explicit dispositions in #144, and obtain green CI on
the resulting combined changes. Preserve mainline branch protection and the
required independent code-owner review.

Final candidate publication, qualification and promotion retain the existing
source and trust checks. Qualification evidence must refer to the exact merged
`main` revision and immutable image digest; integration-branch CI does not replace
it. Extending the currently exact-v0.9.0 qualification policy for the 1.0 RC,
including the required seven-day representative soak, remains separate work in
#144. Production fleet deployment remains separate.
