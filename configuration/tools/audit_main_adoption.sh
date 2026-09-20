#!/usr/bin/env bash
# Does adopting the development tree as `main` lose anything that is only on main?
#
# `main` shares NO common ancestor with the development line in either repo, so
# an ordinary merge is not available and the plan is to ADOPT the development
# tree wholesale (see docs/merge-to-main.md). That is only safe if every path
# reachable from main either survives in the development tree or is a deletion
# somebody can point at a reason for.
#
# This script answers exactly that, and nothing else. It does not compare file
# CONTENT: the development tree rewrote most of these files, so a content diff
# reports thousands of lines that were replaced rather than lost, which is noise
# for this question. What it checks is that no PATH disappears unaccounted for.
#
# Usage: audit_main_adoption.sh <main-ref> <dev-ref>
set -uo pipefail

MAIN="${1:-origin/main}"
DEV="${2:-origin/claude/analysis-only-t4ter8}"

# Paths that exist on main and are deliberately gone from the development tree.
# Each needs a reason, in the comment above it, or it does not belong here.
#
# The generated protobuf-c stubs: there used to be six copies across four
# locations, synced by hand. proto/regen_stubs.sh's banner records that three
# were redundant and deleted, leaving the two the two build systems each need.
# These are those three.
ALLOWED_DELETIONS=(
    "proto/blinds.pb-c.c"
    "proto/blinds.pb-c.h"
    "main/include/blinds.pb-c.h"
)

allowed() {
    local p="$1"
    for a in "${ALLOWED_DELETIONS[@]}"; do [ "$p" = "$a" ] && return 0; done
    return 1
}

echo "main: $MAIN   dev: $DEV"
if git merge-base "$MAIN" "$DEV" >/dev/null 2>&1; then
    echo "NOTE: these refs DO share an ancestor — an ordinary merge is available"
    echo "      and this script's premise no longer holds. Re-read merge-to-main.md."
fi

missing=0
unexplained=0
while IFS= read -r path; do
    if git cat-file -e "$DEV:$path" 2>/dev/null; then continue; fi
    missing=$((missing + 1))
    if allowed "$path"; then
        echo "  ok (documented deletion): $path"
    else
        echo "  UNEXPLAINED DELETION: $path"
        unexplained=$((unexplained + 1))
    fi
done < <(git ls-tree -r --name-only "$MAIN")

echo
echo "paths on main:            $(git ls-tree -r --name-only "$MAIN" | wc -l)"
echo "gone from dev tree:       $missing"
echo "of those, unexplained:    $unexplained"

if [ "$unexplained" -ne 0 ]; then
    echo
    echo "ADOPTION NOT SAFE AS-IS. Each path above is either content to carry"
    echo "across first, or a deletion to justify and add to ALLOWED_DELETIONS."
    exit 1
fi
echo
echo "SAFE: every path on main survives in the dev tree or is a documented deletion."
