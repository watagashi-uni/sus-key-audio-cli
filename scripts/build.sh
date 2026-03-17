#!/bin/zsh
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "$ROOT/bin"

if [[ -n "${CXX:-}" ]]; then
  COMPILER="$CXX"
elif command -v clang++ >/dev/null 2>&1; then
  COMPILER="clang++"
elif command -v g++ >/dev/null 2>&1; then
  COMPILER="g++"
else
  echo "No C++ compiler found. Install clang++ or g++." >&2
  exit 1
fi

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
)

if [[ -n "${EXTRA_CXXFLAGS:-}" ]]; then
  CXXFLAGS+=(${=EXTRA_CXXFLAGS})
fi

echo "[BUILD] compiler=$COMPILER"
"$COMPILER" "${CXXFLAGS[@]}" -o "$ROOT/bin/render-key-audio" "$ROOT/src/main.cpp"
echo "[BUILD] output=$ROOT/bin/render-key-audio"
