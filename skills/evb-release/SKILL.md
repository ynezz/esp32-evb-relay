---
name: evb-release
description: Cut an esp32-evb-relay release as a maintainer. Use when asked to release, tag, ship or publish a new version or release candidate (vX.Y.Z or vX.Y.Z-rc.N), pick the next version number, watch the release workflow, verify draft release assets, checksums and attestations, bench-test a draft, re-tag a failed draft, or check what happens after a release is published.
---

# Release esp32-evb-relay

One annotated `vX.Y.Z` tag releases firmware and CLI together. CI builds a
**draft** GitHub release; an agent verifies and bench-tests it; ynezz
publishes it. Agents never publish.

Repo: `ynezz/esp32-evb-relay`. Below, `$V` is the full tag, e.g. `v1.2.0`
or `v1.2.0-rc.1`.

## Policy

- Tags match `^v[0-9]+\.[0-9]+\.[0-9]+(-rc\.[0-9]+)?$` and must point at
  a commit on `origin/main`; `.github/workflows/release.yml` rejects
  anything else.
- Semver: major for breaking REST API or CLI interface changes, minor for
  new endpoints, commands or features, patch for fixes, docs and internal
  refactors. `-rc.N` marks a prerelease (GitHub marks it one
  automatically). The first release is `v1.0.0-rc.1`.
- **Unpublished drafts may be re-tagged**: delete the draft and the tag,
  fix on main, tag the same version again (see "Failed draft").
- **Published versions are immutable.** Never delete, move or re-push a
  published tag or release, nor its `cli/$V` tag. Fix forward with the
  next rc or patch version.
- No `CHANGELOG.md`: release notes come from commit messages via
  `cliff.toml`, so conventional commit subjects are the changelog.

## 1. Pick the version

```bash
git fetch --tags origin
git tag -l 'v*' --sort=-v:refname | head -5
git cliff --bumped-version          # hint from commits since the last tag
```

`git cliff --bumped-version` only suggests; check it against the policy
above and against `git log --oneline <last-tag>..origin/main`. Unsure
whether something counts as breaking: ask ynezz. Usually cut `-rc.1`
first, then the final version from the same commit once the rc passes.

## 2. Preflight

All must hold; stop and report otherwise:

```bash
git switch main && git pull --ff-only origin main
git status --porcelain                     # empty
test "$(git rev-parse HEAD)" = "$(git rev-parse origin/main)"
git ls-remote --tags origin "refs/tags/$V" # empty
gh release view "$V" --repo ynezz/esp32-evb-relay   # "release not found"
just ci                                    # full pass
```

## 3. Tag and push

```bash
git -c tag.gpgsign=false tag -a "$V" -m "esp32-evb-relay $V"
git push origin "refs/tags/$V"
```

## 4. Watch the release workflow

```bash
gh run list --repo ynezz/esp32-evb-relay --workflow release.yml \
  --branch "$V" --limit 1 --json databaseId,status,url
gh run watch <databaseId> --repo ynezz/esp32-evb-relay --exit-status
```

The workflow validates the tag, runs the full CI suite
(`.github/workflows/ci.yml`), builds firmware and CLI, writes
`SHA256SUMS`, attests every asset and writes release notes. The release
stays a draft. A failed run: `gh run view <databaseId> --log-failed`,
then "Failed draft".

## 5. Verify the draft

```bash
gh release view "$V" --repo ynezz/esp32-evb-relay --json isDraft,isPrerelease,url,assets
mkdir -p "/tmp/evb-release-$V" && cd "/tmp/evb-release-$V"
gh release download "$V" --repo ynezz/esp32-evb-relay --clobber
sha256sum -c SHA256SUMS
for f in *; do gh attestation verify "$f" --repo ynezz/esp32-evb-relay || echo "FAIL $f"; done
```

Expect `isDraft: true`, `isPrerelease` true exactly for `-rc` tags, and
these assets: `evb-relay-fw-$V-full.bin`, `evb-relay-fw-$V-ota.bin`,
`evb-relay-fw-$V.elf`, CLI archives for linux, darwin and windows on
amd64 and arm64, and `SHA256SUMS`. Any mismatch, missing asset or failed
attestation fails the draft.

## 6. Bench test

Hardware rules from the evb-test-device skill apply: own the board alone,
and on any bench other than ynezz's ask before switching relays.

1. Install the linux CLI archive from the download dir and check that
   `evb-relay --version` prints the version from `$V` without the `v`.
2. Flash `evb-relay-fw-$V-full.bin` from a clean board (erase, write at
   `0x0`, provision), following the evb-flash skill.
3. `evb-relay status` must report the same firmware version.
4. Work through `docs/release-test-plan.md` with this CLI and firmware.
   Fill a copy outside the repo (e.g. `/tmp/evb-release-$V/test-plan.md`);
   the file in the repo is the template, do not commit results into it.
   Physical steps you cannot perform are `SKIP` with a reason.

## 7. Hand over to ynezz

If the telegram-notify skill is available, send one message: what to do
(publish or reject), the draft URL, the version, verification result and
test totals (PASS/FAIL/SKIP, and every FAIL by test ID). Otherwise stop
and give the same summary to the user. Then wait; do not publish.

ynezz publishes (`gh release edit "$V" --draft=false` or the web UI).
Publishing triggers `.github/workflows/release-published.yml`, which
pushes the `cli/$V` tag on the same commit so the Go module version
resolves. Afterwards check:

```bash
gh run list --repo ynezz/esp32-evb-relay --workflow release-published.yml --limit 1
git ls-remote --tags origin "refs/tags/cli/$V"   # same commit as $V
```

## Failed draft (never published)

A failed workflow, a bad asset or a failed test plan all end here:

```bash
gh release delete "$V" --repo ynezz/esp32-evb-relay --yes --cleanup-tag
git push origin ":refs/tags/$V"   # only if the tag survived (no draft was created)
git tag -d "$V"
```

Then fix the problem on main with normal commits and CI, and rerun this
runbook from step 2 with the **same** `$V`. File a bead for anything the
bench test found that does not block this release. If there is any doubt
whether `$V` was ever published (`gh release view "$V"` shows
`isDraft: false`, or `cli/$V` exists), it was: do not re-tag; take the
next version.
