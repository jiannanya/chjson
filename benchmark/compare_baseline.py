"""Compare two chjson_bench executables built with identical compiler flags."""
import argparse
import json
import os
import re
import subprocess
import statistics
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--baseline", required=True)
parser.add_argument("--candidate", required=True)
parser.add_argument("--runs", type=int, default=7)
parser.add_argument("--samples", type=int, default=1,
                    help="Independent process pairs per case; alternate run order and report their median")
parser.add_argument("--affinity-mask", type=lambda value: int(value, 0),
                    help="Optional CPU bitmask inherited by both executables (e.g. 0xffff)")
parser.add_argument("--extended", action="store_true", help="Also measure Unicode, escapes, and larger MT payloads")
parser.add_argument("--output", type=Path, default=Path("benchmark-results.json"))
args = parser.parse_args()
if args.runs < 1:
    parser.error("--runs must be positive")
if args.samples < 1:
    parser.error("--samples must be positive")
if args.affinity_mask is not None:
    if args.affinity_mask <= 0:
        parser.error("--affinity-mask must be positive")
    if os.name == "nt":
        import ctypes
        kernel = ctypes.WinDLL("kernel32", use_last_error=True)
        kernel.GetCurrentProcess.restype = ctypes.c_void_p
        kernel.SetProcessAffinityMask.argtypes = [ctypes.c_void_p, ctypes.c_size_t]
        if args.affinity_mask.bit_length() > ctypes.sizeof(ctypes.c_size_t) * 8:
            parser.error("--affinity-mask exceeds the native Windows mask size")
        if not kernel.SetProcessAffinityMask(kernel.GetCurrentProcess(), args.affinity_mask):
            raise ctypes.WinError(ctypes.get_last_error())
    else:
        os.sched_setaffinity(0, {i for i in range(args.affinity_mask.bit_length()) if args.affinity_mask & (1 << i)})

cases = [
    ("scalar", 1, 2000000, "scalar", 24),
    ("small_object", 1, 500000, "objects", 24),
    ("objects", 2000, 1000, "objects", 24),
    ("integers", 20000, 500, "integers", 24),
    ("floats", 20000, 300, "floats", 24),
    ("empty_containers", 5000, 300, "empty", 24),
    ("strings", 2000, 500, "strings", 256),
    ("large_objects", 128, 200, "objects", 4096),
]
if args.extended:
    cases.extend([
        ("escaped_strings", 2000, 300, "escaped_strings", 256),
        ("utf8_strings", 2000, 300, "utf8_strings", 256),
        ("mt_objects_2m", 512, 100, "objects", 4096),
        ("mt_objects_8m", 2048, 30, "objects", 4096),
    ])
report = {"runs_per_case": args.runs, "samples_per_case": args.samples,
          "affinity_mask": hex(args.affinity_mask) if args.affinity_mask is not None else None, "cases": {}}
for name, count, iterations, mode, length in cases:
    samples = {"baseline": [], "candidate": []}
    for sample in range(args.samples):
        order = [("baseline", args.baseline), ("candidate", args.candidate)]
        if sample % 2:
            order.reverse()
        for label, executable in order:
            command = [str(Path(executable).resolve()), str(count), str(iterations), str(args.runs), mode, str(length)]
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                samples[label].append({"command": command, "exit_code": result.returncode,
                                       "stdout": result.stdout, "stderr": result.stderr})
                continue
            text = result.stdout
            metrics = {}
            for operation, speed in re.findall(r"(parse\(dom\)|parse\(in_situ\)|dump\(dom\)): ([\d.]+) MiB/s", text):
                metrics[operation] = float(speed)
            for prefix, used, committed in re.findall(r"(cold arena|arena) used bytes: (\d+), committed bytes: (\d+)", text):
                metrics[prefix + " used"] = int(used)
                metrics[prefix + " committed"] = int(committed)
            metrics["payload bytes"] = int(re.search(r"payload bytes: (\d+)", text)[1])
            samples[label].append({"command": command, "metrics": metrics, "stdout": text})
    row = {}
    for label, values in samples.items():
        if args.samples == 1:
            row[label] = values[0]
        elif any(value.get("exit_code", 0) for value in values):
            # Never hide a failed run behind successful samples.
            row[label] = {"samples": values,
                          "exit_code": next(value["exit_code"] for value in values if value.get("exit_code", 0))}
        else:
            row[label] = {"samples": values,
                          "metrics": {key: statistics.median(value["metrics"][key] for value in values)
                                      for key in values[0]["metrics"]}}
    report["cases"][name] = row
    if "metrics" in row["baseline"] and "metrics" in row["candidate"]:
        before = row["baseline"]["metrics"]
        after = row["candidate"]["metrics"]
        print(f"{name}: parse {before['parse(dom)']:.1f} -> {after['parse(dom)']:.1f} MiB/s; "
              f"cold arena {before['cold arena committed']} -> {after['cold arena committed']} bytes", flush=True)
    else:
        print(f"{name}: baseline exit={row['baseline'].get('exit_code', 0)}, "
              f"candidate exit={row['candidate'].get('exit_code', 0)}", flush=True)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")

if any(case["candidate"].get("exit_code", 0) for case in report["cases"].values()):
    raise SystemExit("candidate benchmark failed; see the result file")
