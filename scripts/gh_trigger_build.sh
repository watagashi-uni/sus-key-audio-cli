#!/bin/zsh
set -euo pipefail

WORKFLOW_FILE="build-and-release.yml"
REF="${1:-}"

if [[ "${REF:-}" == "--help" || "${REF:-}" == "-h" ]]; then
  cat <<'EOF'
Usage:
  ./scripts/gh_trigger_build.sh [ref]

Examples:
  ./scripts/gh_trigger_build.sh
  ./scripts/gh_trigger_build.sh main
EOF
  exit 0
fi

if ! command -v gh >/dev/null 2>&1; then
  echo "GitHub CLI (gh) not found. Install from https://cli.github.com/" >&2
  exit 1
fi

if [[ -n "$REF" ]]; then
  gh workflow run "$WORKFLOW_FILE" --ref "$REF"
else
  gh workflow run "$WORKFLOW_FILE"
fi

echo "Workflow triggered: $WORKFLOW_FILE"
echo "Watching latest run..."
gh run watch --workflow "$WORKFLOW_FILE" --exit-status
