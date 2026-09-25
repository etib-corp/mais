#!/usr/bin/env bash
#
# Formats every first-party C++ source with the checked-in .clang-format.
#
# CI runs this script and then checks `git diff --exit-code`, so run it before
# opening a pull request. Fetched and vendored third-party sources are skipped.
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if ! command -v clang-format > /dev/null 2>&1; then
    echo "clang-format was not found on PATH" >&2
    exit 1
fi

# --cached covers committed files; --others adds new files that are not yet
# committed, while --exclude-standard keeps build output and other ignored
# directories out of the list.
git ls-files --cached --others --exclude-standard -- '*.hpp' '*.cpp' \
    | grep -v '^third_party/' \
    | while IFS= read -r file; do
        clang-format --style=file -i "$file"
        echo "formatted $file"
    done

echo "clang-format applied"
