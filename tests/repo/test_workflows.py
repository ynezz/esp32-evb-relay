"""Static checks on .github/workflows that actionlint does not cover.

A job that calls a local reusable workflow (`uses: ./.github/workflows/x.yml`)
hands it the caller's GITHUB_TOKEN permissions. The called workflow may only
narrow them: if it asks for a scope the caller did not grant, GitHub rejects
the whole run at startup ("startup_failure", no logs), which is how the first
v1.0.0-rc.1 release run died.
"""

from pathlib import Path

import pytest
import yaml

ROOT = Path(__file__).resolve().parents[2]
WORKFLOWS_DIR = ROOT / ".github" / "workflows"
LEVELS = {"none": 0, "read": 1, "write": 2}


def load(path: Path) -> dict:
    return yaml.safe_load(path.read_text())


def normalize(perms) -> dict[str, int] | None:
    """Map a `permissions:` value to {scope: level}; None means unspecified."""
    if perms is None:
        return None
    if perms == "read-all":
        return {"*": LEVELS["read"]}
    if perms == "write-all":
        return {"*": LEVELS["write"]}
    if perms == {}:
        return {}
    return {scope: LEVELS[level] for scope, level in perms.items()}


def granted(perms: dict[str, int], scope: str) -> int:
    return perms.get(scope, perms.get("*", LEVELS["none"]))


def reusable_calls() -> list[tuple[str, str, dict, Path]]:
    calls = []
    for caller in sorted(WORKFLOWS_DIR.glob("*.yml")):
        workflow = load(caller)
        for job_id, job in (workflow.get("jobs") or {}).items():
            uses = job.get("uses", "")
            if uses.startswith("./.github/workflows/"):
                perms = job.get("permissions", workflow.get("permissions"))
                calls.append((caller.name, job_id, perms, ROOT / uses))
    return calls


CALLS = reusable_calls()


def test_release_calls_ci():
    assert any(c[0] == "release.yml" and c[3].name == "ci.yml" for c in CALLS)


@pytest.mark.parametrize(
    "caller,job_id,caller_perms,called", CALLS, ids=[f"{c[0]}:{c[1]}" for c in CALLS]
)
def test_caller_grants_called_workflow_permissions(caller, job_id, caller_perms, called):
    caller_granted = normalize(caller_perms)
    if caller_granted is None:
        pytest.skip("caller uses the repository default token permissions")

    called_workflow = load(called)
    requested = [("workflow", called_workflow.get("permissions"))]
    requested += [
        (f"job {name}", job.get("permissions"))
        for name, job in (called_workflow.get("jobs") or {}).items()
    ]

    missing = []
    for where, perms in requested:
        for scope, level in (normalize(perms) or {}).items():
            if scope == "*":
                continue
            if level > granted(caller_granted, scope):
                missing.append(f"{called.name} {where} wants {scope}: {perms[scope]}")
    assert not missing, f"{caller} job '{job_id}' does not grant: " + "; ".join(missing)
