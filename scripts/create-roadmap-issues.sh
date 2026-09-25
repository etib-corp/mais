#!/usr/bin/env bash
#
# Creates the GitHub milestones and issues described in docs/issues/.
#
# Dry run by default:
#   bash scripts/create-roadmap-issues.sh
# Create for real:
#   bash scripts/create-roadmap-issues.sh --apply
#
# Idempotent: milestones and issues are matched by title, so re-running after a
# partial failure does not duplicate anything.
set -euo pipefail

apply=false
for arg in "$@"; do
    case "$arg" in
        --apply)
            apply=true
            ;;
        -h | --help)
            sed -n '2,10p' "$0"
            exit 0
            ;;
        *)
            echo "unknown argument: $arg" >&2
            exit 2
            ;;
    esac
done

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_root"

if ! command -v gh > /dev/null 2>&1; then
    echo "gh was not found on PATH; install the GitHub CLI first" >&2
    exit 1
fi

if ! gh auth status > /dev/null 2>&1; then
    echo "gh is not authenticated; run 'gh auth login' first" >&2
    exit 1
fi

milestone_m1="M1 - Host-ready invocation surface"
milestone_m2="M2 - Prove and document the consumer integration path"
milestone_m3="M3 - Reload scripts without restarting the headset app"
milestone_m4="M4 - Survive long XR sessions"

# title|description, consumed by the milestone loop.
milestones=(
    "${milestone_m1}|Interpreter lifecycle, bindings, loading and invocation are delivered. This milestone makes the invocation surface rich enough for a real XR/UI host."
    "${milestone_m2}|A second example and the documentation a consumer implementer follows, without adding engine or UI code to mais."
    "${milestone_m3}|Edit a script and see the change in a running session, repeatedly, without leaks and within a frame budget."
    "${milestone_m4}|A session that runs for hours, with a script that may be wrong, degrades instead of crashing."
)

# milestone|labels|issue file, consumed by the issue loop. Keep in sync with
# docs/MILESTONES.md and docs/issues/README.md.
issues=(
    "${milestone_m1}|enhancement|docs/issues/m1-01-host-object-arguments.md"
    "${milestone_m1}|enhancement|docs/issues/m1-02-typed-return-values.md"
    "${milestone_m1}|enhancement|docs/issues/m1-03-hook-introspection.md"
    "${milestone_m1}|enhancement|docs/issues/m1-04-hook-policy-and-timing.md"
    "${milestone_m1}|enhancement|docs/issues/m1-05-structured-traceback-frames.md"
    "${milestone_m1}|documentation|docs/issues/m1-06-threading-model-decision.md"
    "${milestone_m1}|enhancement|docs/issues/m1-07-support-matrix.md"
    "${milestone_m1}|enhancement,good first issue|docs/issues/m1-08-dependency-boundary-guard.md"
    "${milestone_m2}|enhancement|docs/issues/m2-01-frame-loop-host-example.md"
    "${milestone_m2}|documentation|docs/issues/m2-02-adapter-guide.md"
    "${milestone_m2}|documentation|docs/issues/m2-03-script-packaging.md"
    "${milestone_m3}|documentation|docs/issues/m3-01-hot-reload-design.md"
    "${milestone_m3}|enhancement|docs/issues/m3-02-module-reload-api.md"
    "${milestone_m3}|enhancement|docs/issues/m3-03-change-detection-budget.md"
    "${milestone_m3}|enhancement|docs/issues/m3-04-state-migration.md"
    "${milestone_m4}|enhancement|docs/issues/m4-01-cross-thread-dispatch.md"
    "${milestone_m4}|enhancement|docs/issues/m4-02-memory-soak.md"
    "${milestone_m4}|enhancement|docs/issues/m4-03-failing-hook-policy.md"
    "${milestone_m4}|documentation|docs/issues/m4-04-trust-model-decision.md"
    "${milestone_m4}|enhancement|docs/issues/m4-05-diagnostics-sink.md"
)

# The body of an issue is the file without its YAML frontmatter block.
issue_body() {
    awk 'BEGIN { delimiters = 0 } delimiters < 2 && /^---[[:space:]]*$/ { delimiters++; next } delimiters >= 2 { print }' "$1"
}

# The title comes from the frontmatter, with the quotes stripped.
issue_title() {
    sed -n 's/^title:[[:space:]]*//p' "$1" | head -1 | sed 's/^"//; s/"$//'
}

report() {
    if [ "$apply" = true ]; then
        echo "$@"
    else
        echo "[dry run] $*"
    fi
}

# --- Milestones -------------------------------------------------------------

existing_milestones="$(gh api "repos/{owner}/{repo}/milestones?state=all" --jq '.[].title')"

for entry in "${milestones[@]}"; do
    title="${entry%%|*}"
    description="${entry#*|}"

    if grep -qxF "$title" <<< "$existing_milestones"; then
        report "milestone already exists: $title"
        continue
    fi

    report "create milestone: $title"
    if [ "$apply" = true ]; then
        gh api --method POST "repos/{owner}/{repo}/milestones" \
            -f title="$title" \
            -f description="$description" \
            --jq '"  -> " + .html_url'
    fi
done

# --- Issues -----------------------------------------------------------------

existing_issues="$(gh issue list --state all --limit 500 --json title --jq '.[].title')"
created=0

for entry in "${issues[@]}"; do
    milestone="${entry%%|*}"
    rest="${entry#*|}"
    labels="${rest%%|*}"
    file="${rest#*|}"

    if [ ! -f "$file" ]; then
        echo "missing issue file: $file" >&2
        exit 1
    fi

    title="$(issue_title "$file")"
    if [ -z "$title" ]; then
        echo "no title in frontmatter: $file" >&2
        exit 1
    fi

    if grep -qxF "$title" <<< "$existing_issues"; then
        report "issue already exists: $title"
        continue
    fi

    report "create issue: $title [$milestone] labels=$labels"
    if [ "$apply" = true ]; then
        issue_body "$file" | gh issue create \
            --title "$title" \
            --milestone "$milestone" \
            --label "$labels" \
            --body-file -
        created=$((created + 1))
    fi
done

if [ "$apply" = false ]; then
    echo
    echo "Dry run only. Re-run with --apply to create the milestones and issues."
elif [ "$created" -eq 0 ]; then
    echo
    echo "Nothing to create: the tracker already matches docs/issues/."
else
    echo
    echo "Created $created issue(s)."
fi
