#!/usr/bin/env python3

import argparse
import concurrent.futures
import json
import os
import pathlib
import subprocess
import sys
import threading
import time


DEFAULT_MASTERDATA = pathlib.Path("/Users/watagashi/Documents/Code/Sekai/masterdata/musics.json")
DEFAULT_SCORE_ROOT = pathlib.Path(
    "/Users/watagashi/Documents/Code/Sekai/data/assets/sekai/assetbundle/resources/startapp/music/music_score"
)
DEFAULT_BINARY = pathlib.Path("/Users/watagashi/Documents/Code/sus-key-audio-cli/bin/render-key-audio")
DEFAULT_SOUND_ROOT = pathlib.Path("/Users/watagashi/Documents/Code/sekai-mmw-preview-web/public/assets/mmw/sound")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Batch render key-only mp3 files for all musics in musics.json."
    )
    parser.add_argument("--masterdata", type=pathlib.Path, default=DEFAULT_MASTERDATA)
    parser.add_argument("--score-root", type=pathlib.Path, default=DEFAULT_SCORE_ROOT)
    parser.add_argument("--binary", type=pathlib.Path, default=DEFAULT_BINARY)
    parser.add_argument("--sound-root", type=pathlib.Path, default=DEFAULT_SOUND_ROOT)
    parser.add_argument("--out-dir", type=pathlib.Path, required=True)
    parser.add_argument("--workers", type=int, default=max(1, (os.cpu_count() or 1) - 1))
    parser.add_argument("--difficulty", default="master")
    parser.add_argument("--force", action="store_true")
    return parser.parse_args()


def load_jobs(masterdata_path: pathlib.Path, score_root: pathlib.Path, difficulty: str):
    musics = json.loads(masterdata_path.read_text())
    jobs = []
    missing = []

    for music in musics:
        music_id = int(music["id"])
        sus_path = score_root / f"{music_id:04d}_01" / difficulty
        if not sus_path.is_file():
            missing.append((music_id, sus_path))
            continue

        filler_sec = float(music.get("fillerSec") or 0.0)
        offset_ms = int(round(filler_sec * 1000.0))
        jobs.append(
            {
                "music_id": music_id,
                "sus_path": sus_path,
                "offset_ms": offset_ms,
                "title": music.get("title", ""),
            }
        )

    return jobs, missing


def render_one(job, out_dir: pathlib.Path, binary: pathlib.Path, sound_root: pathlib.Path, force: bool):
    out_path = out_dir / f"{job['music_id']}.mp3"
    if out_path.exists() and not force:
        return ("skipped", job["music_id"], f"exists: {out_path.name}")

    cmd = [
        str(binary),
        "--sus",
        str(job["sus_path"]),
        "--out",
        str(out_path),
        "--offset",
        str(job["offset_ms"]),
        "--sound-root",
        str(sound_root),
        "--format",
        "mp3",
    ]

    started = time.time()
    completed = subprocess.run(cmd, capture_output=True, text=True)
    elapsed = time.time() - started

    if completed.returncode != 0:
        stderr = completed.stderr.strip() or completed.stdout.strip() or "unknown error"
        return ("failed", job["music_id"], f"{job['sus_path'].name}: {stderr}")

    return ("ok", job["music_id"], f"{out_path.name} {elapsed:.1f}s")


def main() -> int:
    args = parse_args()
    args.out_dir.mkdir(parents=True, exist_ok=True)

    if not args.binary.is_file():
        print(f"Binary not found: {args.binary}", file=sys.stderr)
        print("Build it first with: ./scripts/build.sh", file=sys.stderr)
        return 1

    jobs, missing = load_jobs(args.masterdata, args.score_root, args.difficulty)
    if not jobs:
        print("No charts found to render.", file=sys.stderr)
        return 1

    print(f"Loaded {len(jobs)} charts from {args.masterdata}")
    if missing:
        print(f"Missing charts: {len(missing)}")

    total = len(jobs)
    done = 0
    ok_count = 0
    skip_count = 0
    fail_count = 0
    lock = threading.Lock()

    def handle_result(result):
        nonlocal done, ok_count, skip_count, fail_count
        status, music_id, detail = result
        with lock:
            done += 1
            if status == "ok":
                ok_count += 1
            elif status == "skipped":
                skip_count += 1
            else:
                fail_count += 1
            print(f"[{done}/{total}] {music_id} {status}: {detail}", flush=True)

    with concurrent.futures.ThreadPoolExecutor(max_workers=args.workers) as executor:
        futures = [
            executor.submit(render_one, job, args.out_dir, args.binary, args.sound_root, args.force)
            for job in jobs
        ]
        for future in concurrent.futures.as_completed(futures):
            handle_result(future.result())

    print(
        f"Finished. ok={ok_count} skipped={skip_count} failed={fail_count} "
        f"out_dir={args.out_dir}"
    )

    if missing:
        missing_path = args.out_dir / "_missing_charts.txt"
        missing_path.write_text(
            "\n".join(f"{music_id}\t{path}" for music_id, path in missing) + "\n"
        )
        print(f"Missing chart list written to {missing_path}")

    return 0 if fail_count == 0 else 2


if __name__ == "__main__":
    raise SystemExit(main())
