#!/bin/zsh
set -euo pipefail

MASTERDATA="/Users/watagashi/Documents/Code/Sekai/masterdata/musics.json"
SCORE_ROOT="/Users/watagashi/Documents/Code/Sekai/data/assets/sekai/assetbundle/resources/startapp/music/music_score"
BINARY="/Users/watagashi/Documents/Code/sus-key-audio-cli/bin/render-key-audio"
SOUND_ROOT="/Users/watagashi/Documents/Code/sekai-mmw-preview-web/public/assets/mmw/sound"
DIFFICULTY="master"
OUT_DIR=""
FORCE=0
WORKER_JOB=""

if command -v sysctl >/dev/null 2>&1; then
  WORKERS="$(sysctl -n hw.ncpu 2>/dev/null || echo 8)"
else
  WORKERS=8
fi

usage() {
  cat <<'EOF'
Usage:
  ./scripts/render_all_musics.sh --out-dir <dir> [options]

Options:
  --out-dir <dir>        Output directory for {musicId}.mp3
  --workers <n>          Parallel worker count
  --masterdata <path>    Path to musics.json
  --score-root <path>    Root path of music_score
  --binary <path>        Path to render-key-audio binary
  --sound-root <path>    Path to MMW sound assets
  --difficulty <name>    Chart difficulty, default: master
  --force                Re-render even if output mp3 already exists
  --help                 Show this help
EOF
}

while [[ $# -gt 0 ]]; do
  case "$1" in
    --out-dir)
      OUT_DIR="$2"
      shift 2
      ;;
    --workers)
      WORKERS="$2"
      shift 2
      ;;
    --masterdata)
      MASTERDATA="$2"
      shift 2
      ;;
    --score-root)
      SCORE_ROOT="$2"
      shift 2
      ;;
    --binary)
      BINARY="$2"
      shift 2
      ;;
    --sound-root)
      SOUND_ROOT="$2"
      shift 2
      ;;
    --difficulty)
      DIFFICULTY="$2"
      shift 2
      ;;
    --force)
      FORCE=1
      shift 1
      ;;
    --help|-h)
      usage
      exit 0
      ;;
    --worker-job)
      WORKER_JOB="$2"
      shift 2
      ;;
    *)
      echo "Unknown argument: $1" >&2
      usage >&2
      exit 1
      ;;
  esac
done

if [[ -z "$OUT_DIR" ]]; then
  echo "--out-dir is required" >&2
  usage >&2
  exit 1
fi

if [[ ! -f "$MASTERDATA" ]]; then
  echo "Masterdata not found: $MASTERDATA" >&2
  exit 1
fi

if [[ ! -x "$BINARY" ]]; then
  echo "Binary not found or not executable: $BINARY" >&2
  echo "Build it first with: ./scripts/build.sh" >&2
  exit 1
fi

if ! command -v jq >/dev/null 2>&1; then
  echo "jq not found. Please install jq first." >&2
  exit 1
fi

mkdir -p "$OUT_DIR"
MISSING_FILE="$OUT_DIR/batch-missing-charts.tsv"
LOG_FILE="$OUT_DIR/batch-failures.log"

render_one() {
  local job="$1"
  local music_id offset_ms padded_id sus_path out_path started elapsed

  music_id="$(printf '%s' "$job" | jq -Rr '@base64d | fromjson | .[0]')"
  offset_ms="$(printf '%s' "$job" | jq -Rr '@base64d | fromjson | .[1]')"
  padded_id="$(printf '%04d' "$music_id")"
  sus_path="$SCORE_ROOT/${padded_id}_01/$DIFFICULTY"
  out_path="$OUT_DIR/${music_id}.mp3"

  if [[ ! -f "$sus_path" ]]; then
    printf '%s\t%s\n' "$music_id" "$sus_path" >> "$MISSING_FILE"
    echo "[MISSING] musicId=$music_id chart=$sus_path"
    return 0
  fi

  if [[ -f "$out_path" && "$FORCE" -ne 1 ]]; then
    echo "[SKIPPED] musicId=$music_id output=$(basename "$out_path")"
    return 0
  fi

  started="$SECONDS"
  if "$BINARY" \
    --sus "$sus_path" \
    --out "$out_path" \
    --offset "$offset_ms" \
    --sound-root "$SOUND_ROOT" \
    --format mp3 >/tmp/render-key-audio-"$music_id".log 2>&1; then
    elapsed="$((SECONDS - started))"
    echo "[DONE] musicId=$music_id output=$(basename "$out_path") offsetMs=$offset_ms elapsed=${elapsed}s"
  else
    {
      echo "[musicId=$music_id]"
      cat /tmp/render-key-audio-"$music_id".log
      echo
    } >> "$LOG_FILE"
    echo "[FAILED] musicId=$music_id"
    rm -f "$out_path"
    return 1
  fi

  rm -f /tmp/render-key-audio-"$music_id".log
}

if [[ -n "$WORKER_JOB" ]]; then
  render_one "$WORKER_JOB"
  exit $?
fi

JOBS_FILE="$(mktemp -t sus-key-jobs.XXXXXX)"
rm -f "$MISSING_FILE" "$LOG_FILE"

cleanup() {
  rm -f "$JOBS_FILE"
}
trap cleanup EXIT

jq -r '.[] | [.id, ((.fillerSec // 0) * 1000 | round)] | @base64' "$MASTERDATA" > "$JOBS_FILE"

TOTAL="$(wc -l < "$JOBS_FILE" | tr -d ' ')"
echo "[START] total=$TOTAL workers=$WORKERS difficulty=$DIFFICULTY"
echo "[INPUT] masterdata=$MASTERDATA"
echo "[OUTPUT] dir=$OUT_DIR"

set +e
< "$JOBS_FILE" xargs -P "$WORKERS" -I {} "$0" \
  --worker-job "{}" \
  --out-dir "$OUT_DIR" \
  --masterdata "$MASTERDATA" \
  --score-root "$SCORE_ROOT" \
  --binary "$BINARY" \
  --sound-root "$SOUND_ROOT" \
  --difficulty "$DIFFICULTY" \
  --workers "$WORKERS" \
  $([[ "$FORCE" -eq 1 ]] && echo --force)
STATUS=$?
set -e

OK_COUNT="$(find "$OUT_DIR" -maxdepth 1 -type f -name '*.mp3' | wc -l | tr -d ' ')"
MISSING_COUNT=0
FAIL_COUNT=0

if [[ -f "$MISSING_FILE" ]]; then
  MISSING_COUNT="$(wc -l < "$MISSING_FILE" | tr -d ' ')"
fi

if [[ -f "$LOG_FILE" ]]; then
  FAIL_COUNT="$(grep -c '^\[' "$LOG_FILE" || true)"
fi

echo "[SUMMARY] renderedMp3=$OK_COUNT missing=$MISSING_COUNT failed=$FAIL_COUNT"
if [[ -f "$MISSING_FILE" ]]; then
  echo "[ARTIFACT] missingCharts=$MISSING_FILE"
fi
if [[ -f "$LOG_FILE" ]]; then
  echo "[ARTIFACT] failureLog=$LOG_FILE"
fi

exit "$STATUS"
