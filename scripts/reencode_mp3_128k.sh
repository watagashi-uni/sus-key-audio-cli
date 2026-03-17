#!/bin/zsh
set -euo pipefail

INPUT_DIR="/Users/watagashi/Documents/Code/sus-key-audio-cli/music-key-audio-master-mp3"
OUTPUT_DIR="/Users/watagashi/Documents/Code/sus-key-audio-cli/music-key-audio-master-mp3-128k"
BITRATE="128k"
FORCE=0
WORKER_INPUT=""

if command -v sysctl >/dev/null 2>&1; then
  WORKERS="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"
else
  WORKERS=8
fi

usage() {
  cat <<'EOF'
Usage:
  ./scripts/reencode_mp3_128k.sh [options]

Options:
  --input-dir <dir>     Source mp3 directory
  --output-dir <dir>    Output mp3 directory
  --workers <n>         Parallel worker count
  --bitrate <rate>      Output bitrate, default: 128k
  --force               Overwrite existing output files
  --help                Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --input-dir)
      INPUT_DIR="$2"
      shift 2
      ;;
    --output-dir)
      OUTPUT_DIR="$2"
      shift 2
      ;;
    --workers)
      WORKERS="$2"
      shift 2
      ;;
    --bitrate)
      BITRATE="$2"
      shift 2
      ;;
    --force)
      FORCE=1
      shift 1
      ;;
    --worker-input)
      WORKER_INPUT="$2"
      shift 2
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ ! -d "$INPUT_DIR" ]]; then
  echo "Input directory not found: $INPUT_DIR" >&2
  exit 1
fi

if ! command -v ffmpeg >/dev/null 2>&1; then
  echo "ffmpeg not found" >&2
  exit 1
fi

mkdir -p "$OUTPUT_DIR"
LOG_FILE="$OUTPUT_DIR/reencode-failures.log"

reencode_one() {
  local input_path="$1"
  local base_name output_path temp_path started elapsed

  base_name="$(basename "$input_path")"
  output_path="$OUTPUT_DIR/$base_name"
  temp_path="$OUTPUT_DIR/.tmp-$base_name"

  if [[ -f "$output_path" && "$FORCE" -ne 1 ]]; then
    echo "[SKIPPED] file=$base_name"
    return 0
  fi

  started="$SECONDS"
  if ffmpeg -v error -y -i "$input_path" -vn -map_metadata -1 -c:a libmp3lame -b:a "$BITRATE" "$temp_path"; then
    mv -f "$temp_path" "$output_path"
    elapsed="$((SECONDS - started))"
    echo "[DONE] file=$base_name bitrate=$BITRATE elapsed=${elapsed}s"
  else
    rm -f "$temp_path"
    {
      echo "[$base_name]"
      echo "input=$input_path"
      echo
    } >> "$LOG_FILE"
    echo "[FAILED] file=$base_name"
    return 1
  fi
}

if [[ -n "$WORKER_INPUT" ]]; then
  reencode_one "$WORKER_INPUT"
  exit $?
fi

LIST_FILE="$(mktemp -t sus-key-mp3-list.XXXXXX)"
rm -f "$LOG_FILE"

cleanup() {
  rm -f "$LIST_FILE"
}
trap cleanup EXIT

find "$INPUT_DIR" -maxdepth 1 -type f -name '*.mp3' -print | sort > "$LIST_FILE"
TOTAL="$(wc -l < "$LIST_FILE" | tr -d ' ')"

echo "[START] total=$TOTAL workers=$WORKERS bitrate=$BITRATE"
echo "[INPUT] dir=$INPUT_DIR"
echo "[OUTPUT] dir=$OUTPUT_DIR"

set +e
< "$LIST_FILE" xargs -P "$WORKERS" -I {} "$0" \
  --worker-input "{}" \
  --input-dir "$INPUT_DIR" \
  --output-dir "$OUTPUT_DIR" \
  --workers "$WORKERS" \
  --bitrate "$BITRATE" \
  $([[ "$FORCE" -eq 1 ]] && echo --force)
STATUS=$?
set -e

OUTPUT_COUNT="$(find "$OUTPUT_DIR" -maxdepth 1 -type f -name '*.mp3' | wc -l | tr -d ' ')"
FAIL_COUNT=0
if [[ -f "$LOG_FILE" ]]; then
  FAIL_COUNT="$(grep -c '^\[' "$LOG_FILE" || true)"
fi

echo "[SUMMARY] outputMp3=$OUTPUT_COUNT failed=$FAIL_COUNT bitrate=$BITRATE"
if [[ -f "$LOG_FILE" ]]; then
  echo "[ARTIFACT] failureLog=$LOG_FILE"
fi

exit "$STATUS"
