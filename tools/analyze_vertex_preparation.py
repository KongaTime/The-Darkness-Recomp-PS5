import argparse
import csv
import json
import math
import statistics
from pathlib import Path


def analyze_capture(path):
    with Path(path).open() as file:
        header = file.readline()
        if not header.startswith("# mode="):
            raise ValueError("missing capture metadata")
        metadata = {key: int(value) for key, value in (word.split("=", 1) for word in header[2:].split())}
        rows = [{key: int(value) for key, value in row.items()} for row in csv.DictReader(file)]
    if metadata["mode"] not in (0, 1) or metadata["duration_ns"] <= 0 or len(rows) < 2:
        raise ValueError("invalid mode, duration or sample count")
    previous = 0
    counters = [metadata["initial_real"], metadata["initial_generated"], metadata["initial_draws"], metadata["initial_rejected"]]
    for row in rows:
        if row["elapsed_ns"] <= previous or row["elapsed_ns"] > metadata["duration_ns"]:
            raise ValueError("frame outside fixed capture window")
        if row["interval_ns"] != row["elapsed_ns"] - previous:
            raise ValueError("inconsistent raw frame interval")
        current = [row[key] for key in ("real_frames", "generated_frames", "world_draws", "rejected")]
        if current[0] != counters[0] + 1 or any(a < b for a, b in zip(current, counters)):
            raise ValueError("inconsistent real frame or resource counters")
        if row["sampling"]:
            raise ValueError("CPU sampling contaminated frame pacing capture")
        previous = row["elapsed_ns"]
        counters = current
    if metadata["boundary_elapsed_ns"] < metadata["duration_ns"]:
        raise ValueError("capture ended before its scheduled boundary")
    if len({row["timers"] for row in rows}) != 1:
        raise ValueError("profiling mode changed during capture")
    intervals = sorted(row["interval_ns"] for row in rows)
    count = len(intervals)
    over = sum(value * 60 > 1000000000 for value in intervals)
    return {
        "file": str(path), "mode": "optimized" if metadata["mode"] else "original",
        "duration_seconds": metadata["duration_ns"] / 1e9, "frames": count,
        "median_ms": statistics.median(intervals) / 1e6,
        "p95_ms": intervals[math.ceil(count * .95) - 1] / 1e6,
        "p99_ms": intervals[math.ceil(count * .99) - 1] / 1e6,
        "max_ms": intervals[-1] / 1e6,
        "frames_over_60fps_budget": over, "percent_over_60fps_budget": over * 100 / count,
        "frames_over_33_333ms": sum(value * 60 > 2000000000 for value in intervals),
        "frames_over_50ms": sum(value > 50000000 for value in intervals),
        "real_fps": count * 1e9 / metadata["duration_ns"],
        "generated_frames": counters[1] - metadata["initial_generated"],
        "world_draws_per_frame": (counters[2] - metadata["initial_draws"]) / count,
        "new_rejections": counters[3] - metadata["initial_rejected"],
        "per_call_timers_enabled": bool(rows[0]["timers"]),
        "unobserved_tail_ms": (metadata["duration_ns"] - previous) / 1e6,
        "boundary_overshoot_ms": (metadata["boundary_elapsed_ns"] - metadata["duration_ns"]) / 1e6,
        "percentile_method": "nearest-rank",
        "measurement": "real-frame vkQueuePresent completion intervals; not physical scanout timestamps",
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("captures", nargs="+", type=Path)
    args = parser.parse_args()
    print(json.dumps([analyze_capture(path) for path in args.captures], indent=2))


if __name__ == "__main__":
    main()
