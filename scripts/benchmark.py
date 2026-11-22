#!/usr/bin/env python3
"""Paired, cache-warmed benchmarks with separate process and engine timings."""
import argparse
import json
import os
from pathlib import Path
import platform
import re
import statistics
import subprocess
import tempfile
import time


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--data", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    mode = parser.add_mutually_exclusive_group(required=True)
    mode.add_argument("--image")
    mode.add_argument("--engine", type=Path)
    parser.add_argument("--baseline-image")
    parser.add_argument("--baseline-engine", type=Path)
    parser.add_argument("--platform", default="linux/amd64")
    parser.add_argument("--cpus", type=int, default=8)
    parser.add_argument("--memory", default="24g")
    parser.add_argument("--runs", type=int, default=5)
    parser.add_argument("--warmups", type=int, default=1)
    parser.add_argument("--compare-script", type=Path)
    args = parser.parse_args()
    if args.cpus < 1 or args.runs < 1 or args.warmups < 0:
        parser.error("cpus and runs must be positive; warmups must be nonnegative")
    if args.image and args.baseline_engine or args.engine and args.baseline_image:
        parser.error("candidate and baseline must use the same execution mode")
    data = args.data.resolve(strict=True)
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    # Each invocation owns a new directory; never mix old Parquet parts into a run.
    results_dir = Path(tempfile.mkdtemp(prefix="benchmark-", dir=root))
    engines = {"candidate": args.image or args.engine.resolve(strict=True)}
    if args.baseline_image or args.baseline_engine:
        engines["baseline"] = args.baseline_image or args.baseline_engine.resolve(strict=True)
    records = []
    last_output = {}
    for run in range(-args.warmups, args.runs):
        order = list(engines)
        if run % 2:
            order.reverse()
        for label in order:
            output = results_dir / f"{run + args.warmups:02d}-{label}"
            output.mkdir(mode=0o777)
            output.chmod(0o777)
            with tempfile.TemporaryDirectory(prefix="scratch-", dir=results_dir) as scratch:
                Path(scratch).chmod(0o777)
                env = dict(os.environ)
                if args.image:
                    command = [
                        "docker", "run", "--rm", "--platform", args.platform,
                        "--network", "none", "--read-only", "--tmpfs", "/tmp:rw,size=64m,mode=1777",
                        "--cap-drop", "ALL", "--security-opt", "no-new-privileges",
                        "--pids-limit", "4096", "--cpus", str(args.cpus),
                        "--memory", args.memory, "--memory-swap", args.memory, "--user", "1000:1000",
                        "-v", f"{data}:/data:ro", "-v", f"{output}:/output", "-v", f"{scratch}:/scratch",
                        "-e", "DATA_DIR=/data", "-e", "OUTPUT_DIR=/output", "-e", "SCRATCH_DIR=/scratch",
                        "-e", "HOME=/scratch", "-e", f"NCPU={args.cpus}", "-e", f"MEM_GB={args.memory.rstrip('gG')}",
                        str(engines[label]),
                    ]
                else:
                    env.update(DATA_DIR=str(data), OUTPUT_DIR=str(output), SCRATCH_DIR=scratch, NCPU=str(args.cpus))
                    command = [str(engines[label])]
                start = time.perf_counter()
                result = subprocess.run(command, env=env, capture_output=True, text=True)
                wall = time.perf_counter() - start
                (output / "stderr.log").write_text(result.stderr)
                if result.returncode:
                    raise RuntimeError(f"{label} exited {result.returncode}:\n{result.stderr}")
                match = re.search(r"^engine time=([\d.eE+-]+)s$", result.stderr, re.M)
                record = {
                    "run": run, "label": label, "warmup": run < 0, "wall_seconds": wall,
                    "engine_seconds": float(match[1]) if match else None,
                    "phases": {k: float(v) for k, v in re.findall(r"^phase (\w+)=([\d.eE+-]+)s$", result.stderr, re.M)},
                }
                records.append(record)
                last_output[label] = str(output)
                print(json.dumps(record), flush=True)
    summary = {}
    for label in engines:
        timed = [r for r in records if r["label"] == label and not r["warmup"]]
        summary[label] = {
            "median_wall_seconds": statistics.median(r["wall_seconds"] for r in timed),
            "median_engine_seconds": statistics.median(r["engine_seconds"] for r in timed if r["engine_seconds"] is not None),
            "median_phases": {name: statistics.median(r["phases"][name] for r in timed) for name in timed[0]["phases"]},
        }
    if "baseline" in summary:
        summary["speedup"] = summary["baseline"]["median_wall_seconds"] / summary["candidate"]["median_wall_seconds"]
    report = {"host": platform.platform(), "execution": "docker" if args.image else "native", "cpus": args.cpus,
              "data": str(data), "engines": {k: str(v) for k, v in engines.items()}, "summary": summary, "runs": records}
    (results_dir / "results.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(summary, indent=2))
    print(f"Report: {results_dir / 'results.json'}", flush=True)
    if args.compare_script and "baseline" in engines:
        subprocess.run([os.sys.executable, str(args.compare_script.resolve()), last_output["candidate"], last_output["baseline"]], check=True)


if __name__ == "__main__":
    main()
