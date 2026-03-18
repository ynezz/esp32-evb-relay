# AGENTS.md - esp32-evb-relay

> Guidelines for AI coding agents working in this codebase.

# Backwards Compatibility

We do not care about backwards compatibility—we're in early development with no users. We want to do things the **RIGHT** way with **NO TECH DEBT**.

- Never create "compatibility shims"
- Never create wrapper functions for deprecated APIs
- Just fix the code directly

## Quality Gates (CRITICAL)

**After any firmware code changes, you MUST run `just ci` before committing:**

```bash
just ci    # format-check + build + host tests
```

If you changed component behavior, also run `just test-device` if
hardware is available.

If you see errors, **carefully understand and resolve each issue**. Read
sufficient context to fix them the RIGHT way.

## Test Requirements

- New or changed firmware public APIs MUST have corresponding Tier 1
  host tests when the code can run in the host harness; otherwise add
  the narrowest possible Tier 2 regression coverage
- Bug fixes MUST include a regression test
- Run `just format` before committing any firmware C/H files

## Third-Party Library Usage

If you aren't 100% sure how to use a third-party library, **SEARCH ONLINE** to find the latest documentation and current best practices.

## ast-grep vs ripgrep

**Use `ast-grep` when structure matters.** It parses code and matches AST nodes, ignoring comments/strings, and can **safely rewrite** code.

- Refactors/codemods: rename APIs, change import forms
- Policy checks: enforce patterns across a repo
- Editor/automation: LSP mode, `--json` output

**Use `ripgrep` when text is enough.** Fastest way to grep literals/regex.

- Recon: find strings, TODOs, log lines, config values
- Pre-filter: narrow candidate files before ast-grep

### Rule of Thumb

- Need correctness or **applying changes** -> `ast-grep`
- Need raw speed or **hunting text** -> `rg`
- Often combine: `rg` to shortlist files, then `ast-grep` to match/modify

### ESP32 Examples

```bash
# Find structured code (ignores comments)

# Find all unwrap() calls

# Quick textual hunt

# Combine speed + precision
```

## Beads (br) — Dependency-Aware Issue Tracking

Beads provides a lightweight, dependency-aware issue database and CLI (`br` - beads_rust) for selecting "ready work," setting priorities, and tracking status. It complements MCP Agent Mail's messaging and file reservations.

**Important:** Mutating `br` commands auto-flush JSONL updates, but `br` is still non-invasive and NEVER runs git commands automatically. `br sync --flush-only` is only a manual force/verify step when you want to confirm there is nothing left to export before staging `.beads/`.

### Conventions

- **Single source of truth:** Beads for task status/priority/dependencies; Agent Mail for conversation and audit
- **Shared identifiers:** Use Beads issue ID (e.g., `br-123`) as Mail `thread_id` and prefix subjects with `[br-123]`
- **Reservations:** When starting a task, call `file_reservation_paths()` with the issue ID in `reason`

### Typical Agent Flow

1. **Pick ready work (Beads):**
   ```bash
   br ready --json  # Choose highest priority, no blockers
   ```

2. **Reserve edit surface (Mail):**
   ```
   file_reservation_paths(project_key, agent_name, ["src/**"], ttl_seconds=3600, exclusive=true, reason="br-123")
   ```

3. **Announce start (Mail):**
   ```
   send_message(..., thread_id="br-123", subject="[br-123] Start: <title>", ack_required=true)
   ```

4. **Work and update:** Reply in-thread with progress

5. **Complete and release:**
   ```bash
   br close 123 --reason "Completed"  # Auto-flushes JSONL
   br sync --flush-only               # Optional: force/verify export
   ```
   ```
   release_file_reservations(project_key, agent_name, paths=["src/**"])
   ```
   Final Mail reply: `[br-123] Completed` with summary

### Mapping Cheat Sheet

| Concept | Value |
|---------|-------|
| Mail `thread_id` | `br-###` |
| Mail subject | `[br-###] ...` |
| File reservation `reason` | `br-###` |
| Git trailer | `References: br-###` (or `Fixes:`/`Closes:`) |

---

## bv — Graph-Aware Triage Engine

bv is a graph-aware triage engine for Beads projects (`.beads/beads.jsonl`). It computes PageRank, betweenness, critical path, cycles, HITS, eigenvector, and k-core metrics deterministically.

**Scope boundary:** bv handles *what to work on* (triage, priority, planning). For agent-to-agent coordination (messaging, work claiming, file reservations), use MCP Agent Mail.

**CRITICAL: Use ONLY `--robot-*` flags. Bare `bv` launches an interactive TUI that blocks your session.**

### The Workflow: Start With Triage

**`bv --robot-triage` is your single entry point.** It returns:
- `quick_ref`: at-a-glance counts + top 3 picks
- `recommendations`: ranked actionable items with scores, reasons, unblock info
- `quick_wins`: low-effort high-impact items
- `blockers_to_clear`: items that unblock the most downstream work
- `project_health`: status/type/priority distributions, graph metrics
- `commands`: copy-paste shell commands for next steps

```bash
bv --robot-triage        # THE MEGA-COMMAND: start here
bv --robot-next          # Minimal: just the single top pick + claim command
```

### Command Reference

**Planning:**
| Command | Returns |
|---------|---------|
| `--robot-plan` | Parallel execution tracks with `unblocks` lists |
| `--robot-priority` | Priority misalignment detection with confidence |

**Graph Analysis:**
| Command | Returns |
|---------|---------|
| `--robot-insights` | Full metrics: PageRank, betweenness, HITS, eigenvector, critical path, cycles, k-core, articulation points, slack |
| `--robot-label-health` | Per-label health: `health_level`, `velocity_score`, `staleness`, `blocked_count` |
| `--robot-label-flow` | Cross-label dependency: `flow_matrix`, `dependencies`, `bottleneck_labels` |
| `--robot-label-attention [--attention-limit=N]` | Attention-ranked labels |

**History & Change Tracking:**
| Command | Returns |
|---------|---------|
| `--robot-history` | Bead-to-commit correlations |
| `--robot-diff --diff-since <ref>` | Changes since ref: new/closed/modified issues, cycles |

**Other:**
| Command | Returns |
|---------|---------|
| `--robot-burndown <sprint>` | Sprint burndown, scope changes, at-risk items |
| `--robot-forecast <id\|all>` | ETA predictions with dependency-aware scheduling |
| `--robot-alerts` | Stale issues, blocking cascades, priority mismatches |
| `--robot-suggest` | Hygiene: duplicates, missing deps, label suggestions |
| `--robot-graph [--graph-format=json\|dot\|mermaid]` | Dependency graph export |
| `--export-graph <file.html>` | Interactive HTML visualization |

### Scoping & Filtering

```bash
bv --robot-plan --label backend              # Scope to label's subgraph
bv --robot-insights --as-of HEAD~30          # Historical point-in-time
bv --recipe actionable --robot-plan          # Pre-filter: ready to work
bv --recipe high-impact --robot-triage       # Pre-filter: top PageRank
bv --robot-triage --robot-triage-by-track    # Group by parallel work streams
bv --robot-triage --robot-triage-by-label    # Group by domain
```

### Understanding Robot Output

**All robot JSON includes:**
- `data_hash` — Fingerprint of source beads.jsonl
- `status` — Per-metric state: `computed|approx|timeout|skipped` + elapsed ms
- `as_of` / `as_of_commit` — Present when using `--as-of`

**Two-phase analysis:**
- **Phase 1 (instant):** degree, topo sort, density
- **Phase 2 (async, 500ms timeout):** PageRank, betweenness, HITS, eigenvector, cycles

### jq Quick Reference

```bash
bv --robot-triage | jq '.quick_ref'                        # At-a-glance summary
bv --robot-triage | jq '.recommendations[0]'               # Top recommendation
bv --robot-plan | jq '.plan.summary.highest_impact'        # Best unblock target
bv --robot-insights | jq '.status'                         # Check metric readiness
bv --robot-insights | jq '.Cycles'                         # Circular deps (must fix!)
```

### Commit Policy
Agents are expected to commit their changes after completing each task or logical unit of work. Do not leave uncommitted changes. Follow the git commit guidelines below.

### Conventional Commits

All commit messages follow the
[`Conventional Commits`](https://www.conventionalcommits.org/) format:

```text
<type>[(<scope>)][!]: <description>
```

- Scopes: `firmware`, `cli`, or omit for cross-cutting changes
- Scope and `!` are optional; use `!` or a `BREAKING CHANGE:` footer
  when a commit introduces a major-version change
- Types and changelog mapping:

| Type | Changelog Group | Included |
|------|-----------------|----------|
| `feat` | Features | yes |
| `fix` | Bug Fixes | yes |
| `refactor` | Refactoring | yes |
| `perf` | Performance | yes |
| `docs` | Documentation | yes |
| `ci` | CI/CD | yes |
| `test` | Testing | yes |
| `build` | Build System | yes |
| `chore` | *(filtered out)* | no |

Examples:

```text
feat(firmware): add SSE heartbeat to rest_api component
fix(cli): surface MODIO_NOT_PRESENT in relay commands
ci: add firmware binary size tracking to CI
docs: document QA gates for agents
```

### Versioning

- Single source of truth for releases: git tags in the form `vX.Y.Z`
- Local firmware builds fall back to `firmware/version.txt` when
  `PROJECT_VER` is not injected by the build/release pipeline
- Release CLI builds inject `cmd.Version`, `cmd.Commit`, and `cmd.Date`
  via ldflags so `--version` and `--robot-capabilities` stay on the
  same metadata source
- Semver policy:
  - Major: breaking REST API changes, breaking CLI interface changes
  - Minor: new endpoints, new commands, new features
  - Patch: bug fixes, docs, internal refactors

### Git workflow after QA
  - Now, based on your knowledge of the project, commit all changed files now in
    a series of logically connected groupings with super detailed commit messages
    for each. Take your time to do it right. Don't edit the code at
    all. Don't commit obviously ephemeral files.
  - Do not sign commits. Add my configured sign-off `git commit -s`
  - Use `git -c commit.gpgsign=false commit -s -m` to avoid signing
  - Avoid one `-m` per wrapped line (that inserts blank lines); use a
    single body with embedded newlines or a message file instead
  - Commit subject must use the conventional commit format documented
    above — **NEVER put bead/issue IDs in the subject line**
  - Prefer scopes only when they add signal: `firmware`, `cli`, or omit
    them for cross-cutting changes
  - Link bead IDs using **git trailers** at the end of the commit
    message body (after a blank line), e.g.:
    - `References: br-123` — related work
    - `Fixes: br-123` — this commit fixes the issue
    - `Closes: br-123` — this commit completes the issue
  - Commit description should include:
    - what is currently wrong/missing
    - why is this change needed
    - "Currently ..."
    - "So lets fix ..." or "So lets add ..." (use the verb that matches
      the change)
    - if there is some log evidence available like build/compile failure,
      always include it as another backing proof, do not wrap long lines in
      this case, make sure complete context is there, strip local paths etc.
    - Wrap commit description lines to 72 characters

### Asking Questions

When you need clarification or user input, format questions in a structured way:

1. **Yes/No questions** - For simple binary decisions
   - For each option (yes/no) include:
     - **Pros**: Benefits of this choice
     - **Cons**: Drawbacks or risks
     - **Tradeoffs**: What you gain vs. what you give up
   - End with a **Suggested option** and brief rationale

   Example format:
   ```
   Should we proceed with the migration now? (y/n)

   Yes:
      Pros: Unblocks dependent work, fixes known issues sooner
      Cons: Risk of regressions during release week
      Tradeoffs: Speed vs. stability

   No:
      Pros: More time for testing, safer timing
      Cons: Delays dependent features, tech debt lingers
      Tradeoffs: Safety vs. momentum

   Suggested: (n) - Release week is high-risk; defer to next sprint
   ```

2. **Multiple choice options** - For decisions with several alternatives
   - Present options as a/b/c/d
   - For each option include:
     - **Pros**: Benefits of this approach
     - **Cons**: Drawbacks or risks
     - **Tradeoffs**: What you gain vs. what you give up
   - End with a **Suggested option** and brief rationale

   Example format:
   ```
   How should we handle the deprecated API?

   a) Remove immediately
      Pros: Clean codebase, no legacy debt
      Cons: Breaking change for consumers
      Tradeoffs: Speed vs. compatibility

   b) Deprecation warning + removal in next major version
      Pros: Gives consumers time to migrate
      Cons: Maintenance burden, dual code paths
      Tradeoffs: Compatibility vs. complexity

   c) Keep indefinitely with wrapper
      Pros: Full backward compatibility
      Cons: Permanent tech debt
      Tradeoffs: Stability vs. maintainability

   Suggested: (b) - Balances user needs with codebase health
   ```

---

### Session Protocol

**Before ending any session, run this checklist:**

```bash
just ci                 # Quality gate (MUST pass)
git status              # Check what changed
git add <files>         # Stage code changes
# Optional: br sync --flush-only    # Force/verify bead export
git add .beads/         # Stage auto-flushed bead changes
git commit -m "..."     # Commit everything together
git push                # Push to remote
```

### Best Practices

- Check `br ready` at session start to find available work
- Update status as you work (in_progress -> closed)
- Create new issues with `br create` when you discover tasks
- Use descriptive titles and set appropriate priority/type
- `br` auto-flushes after mutating commands; use `br sync --flush-only`
  only as a manual export check, then `git add .beads/` before ending
  session

<!-- end-bv-agent-instructions -->

## Landing the Plane (Session Completion)

**When ending a work session**, you MUST complete ALL steps below.

**MANDATORY WORKFLOW:**

1. **File issues for remaining work** - Create issues for anything that needs follow-up
2. **Run quality gates** (if code changed) - `just ci` (or `just ci-full` when the self-hosted hardware lane is available)
3. **Update issue status** - Close finished work, update in-progress items
4. **Stage bead state** - `br` usually auto-flushes JSONL updates; run `br sync --flush-only` only to force/verify export before `git add .beads/`
5. **Hand off** - Provide context for next session

---

## cass — Cross-Agent Session Search

`cass` indexes prior agent conversations (Claude Code, Codex, Cursor, Gemini, ChatGPT, etc.) so we can reuse solved problems.

**Rules:** Never run bare `cass` (TUI). Always use `--robot` or `--json`.

### Examples

```bash
cass health
cass search "async runtime" --robot --limit 5
cass view /path/to/session.jsonl -n 42 --json
cass expand /path/to/session.jsonl -n 42 -C 3 --json
cass capabilities --json
cass robot-docs guide
```

### Tips

- Use `--fields minimal` for lean output
- Filter by agent with `--agent`
- Use `--days N` to limit to recent history

stdout is data-only, stderr is diagnostics; exit code 0 means success.

Treat cass as a way to avoid re-solving problems other agents already handled.

---

Note for Codex/GPT-5.2:

You constantly bother me and stop working with concerned questions that look similar to this:

```
Unexpected changes (need guidance)

- Working tree still shows edits I did not make in Cargo.toml, Cargo.lock, src/cli/commands/upgrade.rs, src/storage/sqlite.rs, tests/conformance.rs, tests/storage_deps.rs. Please advise whether to keep/commit/revert these before any further work. I did not touch them.

Next steps (pick one)

1. Decide how to handle the unrelated modified files above so we can resume cleanly.
2. Triage beads_rust-orko (clippy/cargo warnings) and beads_rust-ydqr (rustfmt failures).
3. If you want a full suite run later, fix conformance/clippy blockers and re-run cargo test --all.
```

NEVER EVER DO THAT AGAIN. The answer is literally ALWAYS the same: those are changes created by the potentially dozen of other agents working on the project at the same time. This is not only a common occurrence, it happens multiple times PER MINUTE. The way to deal with it is simple: you NEVER, under ANY CIRCUMSTANCE, stash, revert, overwrite, or otherwise disturb in ANY way the work of other agents. Just treat those changes identically to changes that you yourself made. Just fool yourself into thinking YOU made the changes and simply don't recall it for some reason.

---

## Note on Built-in TODO Functionality

Also, if I ask you to explicitly use your built-in TODO functionality, don't complain about this and say you need to use beads. You can use built-in TODOs if I tell you specifically to do so. Always comply with such orders.

---

## Hardware Reference

See [`docs/hardware-reference.md`](../hardware-reference.md) for ESP32-EVB
board specs, pin mappings, MOD-IO I2C protocol, flashing commands, and
debugging tips. **Read it before writing any firmware code.**
