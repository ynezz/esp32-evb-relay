#!/usr/bin/env bash
# Pre-commit hook: verify staged firmware/CLI formatting and vet CLI packages
set -euo pipefail

mapfile -t STAGED_C_FILES < <(
    git diff --cached --name-only --diff-filter=ACM \
        | grep -E '^firmware/.*\.[ch]$' \
        | grep -Ev '^firmware/(build|managed_components|test/build|test_app/build|test_app/managed_components)/' || true
)

mapfile -t STAGED_GO_FILES < <(
    git diff --cached --name-only --diff-filter=ACM \
        | grep -E '^cli/.*\.go$' || true
)

if [ "${#STAGED_C_FILES[@]}" -eq 0 ] && [ "${#STAGED_GO_FILES[@]}" -eq 0 ]; then
    exit 0
fi

ASTYLE_FLAGS="--astyle-version=3.4.7 --style=otbs \
    --attach-namespaces --attach-classes \
    --indent=spaces=4 --convert-tabs --align-reference=name \
    --keep-one-line-statements --pad-header --pad-oper \
    --unpad-paren --max-continuation-indent=120"
ASTYLE_PY=".venv/bin/astyle_py"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

TMP_FILES=()
if [ "${#STAGED_C_FILES[@]}" -gt 0 ]; then
    if [ ! -x "$ASTYLE_PY" ]; then
        if ! ASTYLE_PY=$(command -v astyle_py); then
            echo "ERROR: astyle_py not found. Run: just setup"
            exit 1
        fi
    fi

    for path in "${STAGED_C_FILES[@]}"; do
        mkdir -p "$TMPDIR/$(dirname "$path")"
        git show ":$path" > "$TMPDIR/$path"
        TMP_FILES+=("$TMPDIR/$path")
    done

    # shellcheck disable=SC2086
    if ! "$ASTYLE_PY" --dry-run $ASTYLE_FLAGS "${TMP_FILES[@]}"; then
        echo ""
        echo "Formatting errors detected. Run 'just format' to fix."
        exit 1
    fi
fi

if [ "${#STAGED_GO_FILES[@]}" -gt 0 ]; then
    if ! command -v go &>/dev/null; then
        echo "ERROR: go not found. Install Go before committing staged CLI changes."
        exit 1
    fi

    GO_TMP_FILES=()
    for path in "${STAGED_GO_FILES[@]}"; do
        mkdir -p "$TMPDIR/$(dirname "$path")"
        git show ":$path" > "$TMPDIR/$path"
        GO_TMP_FILES+=("$TMPDIR/$path")
    done

    if [ -n "$(gofmt -l "${GO_TMP_FILES[@]}")" ]; then
        echo "ERROR: gofmt differences detected in staged CLI files."
        gofmt -d "${GO_TMP_FILES[@]}"
        exit 1
    fi

    declare -A SEEN_GO_PACKAGES=()
    GO_PACKAGES=()
    for path in "${STAGED_GO_FILES[@]}"; do
        package_dir=$(dirname "$path")
        if [ "$package_dir" = "cli" ]; then
            package="."
        else
            package="./${package_dir#cli/}"
        fi

        if [ -z "${SEEN_GO_PACKAGES[$package]:-}" ]; then
            SEEN_GO_PACKAGES["$package"]=1
            GO_PACKAGES+=("$package")
        fi
    done

    INDEX_SNAPSHOT_DIR="$TMPDIR/index"
    mkdir -p "$INDEX_SNAPSHOT_DIR"
    git checkout-index --all --prefix="$INDEX_SNAPSHOT_DIR/"

    (
        # Vet the staged index snapshot so unstaged edits cannot hide
        # or invent failures for the commit being created.
        cd "$INDEX_SNAPSHOT_DIR/cli"
        go vet "${GO_PACKAGES[@]}"
    )
fi
