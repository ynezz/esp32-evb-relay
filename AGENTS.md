# AGENTS.md - esp32-evb-relay

Guidelines for AI coding agents working in this codebase. ESP-IDF firmware
lives in `firmware/`, the Go CLI (`evb-relay`) in `cli/`.

Task runbooks live in `skills/` (discovered through the
`.claude/skills/` and `.agents/skills/` symlinks): `evb-flash`,
`evb-use`, `evb-test-device` and `evb-release`. Follow the matching skill
instead of improvising flash, test or release steps. When you change a
recipe, CLI command, flag or script a skill names, update the skill in
the same change; `just ci` fails on drift (`tests/repo/test_skills.py`).

## Backwards Compatibility

We do not care about backwards compatibility — early development, no
users. Do things the **RIGHT** way with **NO TECH DEBT**: never add
compatibility shims or wrapper functions for deprecated APIs, fix the
code directly.

## Quality Gates (CRITICAL)

**After any firmware code changes, run `just ci` before committing:**

```bash
just ci    # format-check + build + host tests + CLI gates + CLI e2e + skills drift
```

If you changed component behavior, also run `just test-device` when
hardware is available (`just ci-full` runs both).

## Test Requirements

- New or changed firmware public APIs MUST have corresponding Tier 1
  host tests when the code can run in the host harness; otherwise add
  the narrowest possible Tier 2 regression coverage
- Bug fixes MUST include a regression test
- Run `just format` before committing any firmware C/H files

## Conventional Commits

`<type>[(<scope>)][!]: <description>`. Scopes: `firmware`, `cli`, or omit
for cross-cutting changes. `!` or a `BREAKING CHANGE:` footer for a
major-version change.

| Type | `feat` | `fix` | `refactor` | `perf` | `docs` | `ci` | `test` | `build` | `chore` |
|---|---|---|---|---|---|---|---|---|---|
| Changelog | Features | Bug Fixes | Refactoring | Performance | Documentation | CI/CD | Testing | Build System | *(dropped)* |

Sign off with `git commit -s`. Never put bead IDs in the subject line —
link them as git trailers after a blank line: `References: evb-xxx`,
`Fixes: evb-xxx`, `Closes: evb-xxx`.

## Versioning

A single `vX.Y.Z` tag releases firmware and CLI in lockstep. Firmware
version comes from `git describe` locally, or `PROJECT_VER` injected by
release CI. CLI version comes from goreleaser ldflags on release builds,
falling back to `debug.ReadBuildInfo()` for `go install`/local builds.

## Beads (br) — Issue Tracking

`br` (beads_rust) tracks task status/priority/dependencies in
`.beads/`. `br ready --json` picks up ready work; update status
in_progress -> closed as you go; `br` auto-flushes JSONL on mutating
commands. Run `br sync --flush-only` only to force/verify export before
`git add .beads/`. `br` never runs git commands itself.

## Hardware Reference

See [`docs/hardware-reference.md`](docs/hardware-reference.md) for
ESP32-EVB board specs, pin mappings, MOD-IO I2C protocol, flashing
commands, and debugging tips. **Read it before writing any firmware
code.**
