"""Drift test: keep the agent skills in skills/ honest.

Every command, recipe, flag and path a skill tells an agent to use must
exist in this checkout. When this fails, fix the skill (or the thing it
describes) in the same change that caused the drift.
"""

import json
import os
import re
import shlex
import shutil
import subprocess
from functools import cache
from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]
SKILLS_DIR = ROOT / "skills"
SHARED_DIR = SKILLS_DIR / "_shared"
DISCOVERY_DIRS = (ROOT / ".agents" / "skills", ROOT / ".claude" / "skills")
MAX_SKILL_LINES = 200

# Repo-relative paths a skill may point at. Build outputs (firmware/build,
# bin/, dist/) are deliberately not checked: they exist only after a build.
REPO_PATH = re.compile(
    r"(?<![\w./~$-])((?:scripts|docs|skills|tools|\.github)/[A-Za-z0-9_./-]*[A-Za-z0-9_])"
)
JUST_RECIPE = re.compile(r"(?:^|[\s`(])just ([a-z][a-z0-9-]*)", re.MULTILINE)
EVB_RELAY = re.compile(r"(?:^|(?<=[\s`(/]))evb-relay(?![\w.-])")
# Global flags of the CLI that take a value, so the token after them is
# not mistaken for a command verb.
VALUE_FLAGS = {"-H", "--host", "-k", "--api-token", "-f", "--format", "-t", "--timeout"}
SHELL_STOP = {"|", "||", "&&", ";", ">", ">>", "2>&1", "#"}


def skill_names() -> list[str]:
    return sorted(
        p.name for p in SKILLS_DIR.iterdir() if p.is_dir() and not p.name.startswith("_")
    )


def skill_docs() -> list[Path]:
    """Every markdown file agents can be sent to: SKILL.md, references, shared."""
    return sorted(SKILLS_DIR.rglob("*.md"))


def rel(path: Path) -> str:
    return str(path.relative_to(ROOT))


def frontmatter(text: str) -> dict[str, str]:
    match = re.match(r"---\n(.*?)\n---\n", text, re.DOTALL)
    if not match:
        raise AssertionError("must start with a '---' YAML frontmatter block")
    fields: dict[str, str] = {}
    for line in match.group(1).splitlines():
        key, sep, value = line.partition(":")
        if not sep:
            raise AssertionError(f"frontmatter line is not 'key: value': {line!r}")
        fields[key.strip()] = value.strip()
    return fields


def code_spans(text: str) -> list[str]:
    """Fenced block lines and inline code spans; where commands live."""
    spans: list[str] = []
    for block in re.findall(r"```[^\n]*\n(.*?)```", text, re.DOTALL):
        # Join backslash continuations so one command stays one span.
        spans.extend(block.replace("\\\n", " ").splitlines())
    without_fences = re.sub(r"```[^\n]*\n.*?```", "", text, flags=re.DOTALL)
    spans.extend(re.findall(r"`([^`\n]+)`", without_fences))
    return spans


def split_words(span: str) -> list[str]:
    try:
        return shlex.split(span, comments=True)
    except ValueError:
        return span.split()


def evb_relay_invocations(span: str) -> list[list[str]]:
    """Argument lists following each `evb-relay` in a code span."""
    invocations = []
    for match in EVB_RELAY.finditer(span):
        args = []
        for word in split_words(span[match.end():]):
            if word in SHELL_STOP:
                break
            args.append(word)
        invocations.append(args)
    return invocations


def cli_capabilities(tmp_dir: str) -> tuple[set[str], set[str]]:
    """Build the CLI from this checkout; return (command names, flags)."""
    go = shutil.which("go")
    if go is None:
        pytest.fail("go is not on PATH: the skill drift test builds cli/ to read --robot-capabilities")
    binary = Path(tmp_dir) / "evb-relay"
    build = subprocess.run(
        [go, "build", "-o", str(binary), "."],
        cwd=ROOT / "cli",
        capture_output=True,
        text=True,
    )
    if build.returncode != 0:
        pytest.fail(f"building cli/ failed:\n{build.stderr}")

    caps = json.loads(
        subprocess.run(
            [str(binary), "--robot-capabilities"], check=True, capture_output=True, text=True
        ).stdout
    )
    commands = {c["name"] for c in caps["commands"]}
    flags = {f for c in caps["commands"] for f in c.get("flags", [])}
    # Global and per-command flags as cobra prints them, which also covers
    # flags --robot-capabilities does not list (e.g. --version, -H).
    for command in [""] + sorted(commands):
        help_text = subprocess.run(
            [str(binary), *command.split(), "--help"], capture_output=True, text=True
        ).stdout
        for short, long in re.findall(r"^\s+(?:(-\w), )?(--[\w-]+)", help_text, re.MULTILINE):
            flags.add(long)
            if short:
                flags.add(short)
    return commands, flags


@cache
def just_recipes() -> set[str]:
    just = shutil.which("just")
    if just is None:
        pytest.fail("just is not on PATH: needed to list Justfile recipes")
    out = subprocess.run(
        [just, "--summary"], cwd=ROOT, check=True, capture_output=True, text=True
    ).stdout
    return set(out.split())


@pytest.fixture(scope="session")
def cli(tmp_path_factory):
    return cli_capabilities(str(tmp_path_factory.mktemp("evb-relay-cli")))


def test_skills_exist():
    assert skill_names(), "skills/ has no skill directories"


@pytest.mark.parametrize("name", skill_names())
def test_frontmatter(name):
    path = SKILLS_DIR / name / "SKILL.md"
    assert path.is_file(), f"{rel(path)} is missing: every skills/<name>/ needs a SKILL.md"
    text = path.read_text()
    fields = frontmatter(text)
    assert fields.get("name") == name, (
        f"{rel(path)}: frontmatter name is {fields.get('name')!r}, must equal the directory name {name!r}"
    )
    assert fields.get("description"), f"{rel(path)}: frontmatter needs a non-empty description"
    assert set(fields) == {"name", "description"}, (
        f"{rel(path)}: frontmatter may only hold name and description, found {sorted(fields)}"
    )
    lines = len(text.splitlines())
    assert lines <= MAX_SKILL_LINES, (
        f"{rel(path)} has {lines} lines (max {MAX_SKILL_LINES}): move detail into "
        f"skills/{name}/references/ or skills/_shared/"
    )


@pytest.mark.parametrize("name", skill_names())
@pytest.mark.parametrize("discovery", DISCOVERY_DIRS, ids=lambda p: rel(p))
def test_discovery_symlink(name, discovery):
    link = discovery / name / "SKILL.md"
    target = f"../../../skills/{name}/SKILL.md"
    fix = f"fix: mkdir -p {rel(link.parent)} && ln -sfn {target} {rel(link)}"
    assert link.is_symlink(), f"{rel(link)} must be a symlink to the canonical skill; {fix}"
    assert os.readlink(link) == target, (
        f"{rel(link)} -> {os.readlink(link)}, must be the relative link {target}; {fix}"
    )
    assert link.resolve() == (SKILLS_DIR / name / "SKILL.md").resolve(), f"{rel(link)} does not resolve; {fix}"


@pytest.mark.parametrize("discovery", DISCOVERY_DIRS, ids=lambda p: rel(p))
def test_no_orphan_discovery_entries(discovery):
    if not discovery.exists():
        pytest.fail(f"{rel(discovery)} is missing; create the per-skill symlinks")
    orphans = sorted(p.name for p in discovery.iterdir() if p.name not in skill_names())
    assert not orphans, (
        f"{rel(discovery)} has entries without a skills/<name>/SKILL.md: {orphans}; "
        "remove them or add the canonical skill"
    )


@pytest.mark.parametrize("doc", skill_docs(), ids=rel)
def test_just_recipes_exist(doc):
    mentioned = set()
    for span in code_spans(doc.read_text()):
        mentioned.update(JUST_RECIPE.findall(" " + span))
    missing = sorted(mentioned - just_recipes())
    assert not missing, (
        f"{rel(doc)} names just recipes that the Justfile does not define: {missing}; "
        f"known recipes: {sorted(just_recipes())}"
    )


@pytest.mark.parametrize("doc", skill_docs(), ids=rel)
def test_evb_relay_commands_and_flags_exist(doc, cli):
    commands, flags = cli
    groups = {c.split()[0] for c in commands if " " in c}
    problems = []
    for span in code_spans(doc.read_text()):
        for args in evb_relay_invocations(span):
            words: list[str] = []
            skip_value = False
            for arg in args:
                if skip_value:
                    skip_value = False
                    continue
                if arg.startswith("-"):
                    flag = arg.split("=", 1)[0]
                    if flag not in flags:
                        problems.append(f"unknown flag {flag!r} in `{span.strip()}`")
                    skip_value = flag in VALUE_FLAGS and "=" not in arg
                    continue
                words.append(arg)
            if not words:
                continue
            verb = words[0]
            if not re.fullmatch(r"[a-z]+", verb):
                continue  # a placeholder or file name, not a verb
            if verb in groups:
                if len(words) < 2:
                    problems.append(f"{verb!r} needs a subcommand in `{span.strip()}`")
                    continue
                name = f"{verb} {words[1]}"
            else:
                name = verb
            if name not in commands:
                problems.append(f"unknown command {name!r} in `{span.strip()}`")
    assert not problems, (
        f"{rel(doc)} drifted from `evb-relay --robot-capabilities`:\n  "
        + "\n  ".join(problems)
        + f"\nknown commands: {sorted(commands)}"
    )


@pytest.mark.parametrize("doc", skill_docs(), ids=rel)
def test_repo_paths_exist(doc):
    missing = sorted({p for p in REPO_PATH.findall(doc.read_text()) if not (ROOT / p).exists()})
    assert not missing, (
        f"{rel(doc)} points at repo paths that do not exist: {missing}; "
        "fix the path or the skill (paths are relative to the repo root)"
    )


@pytest.mark.parametrize("doc", [ROOT / "AGENTS.md", ROOT / "README.md"], ids=rel)
def test_entry_docs_point_at_skills(doc):
    text = doc.read_text()
    if "skills/" not in text:
        pytest.fail(f"{rel(doc)} must point agents at skills/")
    if doc.name == "README.md":
        unlisted = [name for name in skill_names() if name not in text]
        if unlisted:
            pytest.fail(f"README.md 'Drive it from an agent' must list these skills: {unlisted}")


def test_shared_docs_are_referenced():
    corpus = "\n".join(p.read_text() for p in skill_docs())
    unused = sorted(rel(p) for p in SHARED_DIR.glob("*.md") if rel(p) not in corpus)
    assert not unused, f"shared skill docs nobody references: {unused}; link them or delete them"
