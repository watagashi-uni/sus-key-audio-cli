#!/bin/zsh
set -euo pipefail

TAG="${1:-}"

if [[ "${TAG:-}" == "--help" || "${TAG:-}" == "-h" ]]; then
  cat <<'EOF'
Usage:
  ./scripts/gh_release_tag.sh v0.1.0
EOF
  exit 0
fi

if [[ -z "$TAG" ]]; then
  echo "Usage: ./scripts/gh_release_tag.sh v0.1.0" >&2
  exit 1
fi

if ! command -v gh >/dev/null 2>&1; then
  echo "GitHub CLI (gh) not found. Install from https://cli.github.com/" >&2
  exit 1
fi

if ! git rev-parse --is-inside-work-tree >/dev/null 2>&1; then
  echo "Current directory is not a git repository." >&2
  exit 1
fi

if git rev-parse "$TAG" >/dev/null 2>&1; then
  echo "Tag already exists: $TAG"
else
  git tag -a "$TAG" -m "Release $TAG"
  echo "Created tag: $TAG"
fi

git push origin "$TAG"
echo "Pushed tag: $TAG"
echo "Release will be created automatically by GitHub Actions workflow build-and-release.yml"
echo "You can watch it with:"
echo "  gh run watch --workflow build-and-release.yml --exit-status"
