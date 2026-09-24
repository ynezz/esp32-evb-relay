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


def esp_idf_ci_steps() -> list[tuple[str, dict]]:
    steps = []
    for path in sorted(WORKFLOWS_DIR.glob("*.yml")):
        for job_id, job in (load(path).get("jobs") or {}).items():
            for step in job.get("steps") or []:
                if step.get("uses", "").startswith("espressif/esp-idf-ci-action@"):
                    steps.append((f"{path.name}:{job_id}:{step.get('name', '?')}", step))
    return steps


ESP_IDF_STEPS = esp_idf_ci_steps()


# esp-idf-ci-action runs `docker run ... /bin/bash -c '<command>'` and only
# passes IDF_TARGET into the container. The release firmware job broke on
# both counts: a quoted `bash -lc '...'` command ("bash: -c: option requires
# an argument") and a step env var the container never saw.
@pytest.mark.parametrize("where,step", ESP_IDF_STEPS, ids=[s[0] for s in ESP_IDF_STEPS])
def test_esp_idf_ci_action_command_survives_docker_wrapping(where, step):
    command = step["with"]["command"]
    assert "'" not in command, f"{where}: single quote in command breaks the action's bash -c '...'"

    forwarded = step["with"].get("extra_docker_args", "").split()
    for name in step.get("env") or {}:
        if f"${name}" in command or f"${{{name}" in command:
            assert "-e" in forwarded and (
                name in forwarded or any(a.startswith(f"{name}=") for a in forwarded)
            ), f"{where}: command uses ${name} but extra_docker_args does not pass -e {name}"


# `idf.py merge-bin` runs esptool with cwd=build/, so a relative -o lands
# under build/ (the first fixed release run died with "FileNotFoundError:
# ... '../dist/evb-relay-fw-v1.0.0-rc.1-full.bin'").
@pytest.mark.parametrize("where,step", ESP_IDF_STEPS, ids=[s[0] for s in ESP_IDF_STEPS])
def test_esp_idf_merge_bin_output_is_absolute(where, step):
    words = step["with"]["command"].split()
    for i, word in enumerate(words):
        if word == "merge-bin" and "-o" in words[i:]:
            out = words[words.index("-o", i) + 1].strip('"')
            assert out.startswith(("/", "${PWD}", "$PWD")), f"{where}: merge-bin -o {out} is relative"
