#!/usr/bin/env bash
# Pre-commit hook: verify C/H formatting with astyle_py
set -euo pipefail

mapfile -t STAGED_FILES < <(
    git diff --cached --name-only --diff-filter=ACM \
        | grep -E '^firmware/.*\.[ch]$' \
        | grep -Ev '^firmware/(build|managed_components|test/build|test_app/build|test_app/managed_components)/' || true
)

if [ "${#STAGED_FILES[@]}" -eq 0 ]; then
    exit 0
fi

if ! command -v astyle_py &>/dev/null; then
    echo "ERROR: astyle_py not found. Run: python3 -m pip install astyle_py==1.0.5"
    exit 1
fi

ASTYLE_FLAGS="--astyle-version=3.4.7 --style=otbs \
    --attach-namespaces --attach-classes \
    --indent=spaces=4 --convert-tabs --align-reference=name \
    --keep-one-line-statements --pad-header --pad-oper \
    --unpad-paren --max-continuation-indent=120"

TMPDIR=$(mktemp -d)
trap 'rm -rf "$TMPDIR"' EXIT

TMP_FILES=()
for path in "${STAGED_FILES[@]}"; do
    mkdir -p "$TMPDIR/$(dirname "$path")"
    git show ":$path" > "$TMPDIR/$path"
    TMP_FILES+=("$TMPDIR/$path")
done

# shellcheck disable=SC2086
if ! astyle_py --dry-run $ASTYLE_FLAGS "${TMP_FILES[@]}"; then
    echo ""
    echo "Formatting errors detected. Run 'just format' to fix."
    exit 1
fi
