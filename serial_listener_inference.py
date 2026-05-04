#!/usr/bin/env python3
"""
Listens to the railway system serial output in inference mode and records
complete runs to a timestamped log file.

Each run captures:
  - rejected sample details (original and substituted values)
  - rejected sample count
  - raw distances (15 values)
  - filtered distances (15 values)
  - MLP confidence and direction (MLP mode only)
  - model used (DT / MLP)
  - inference result (OPEN / CLOSE)

Stops automatically after --runs complete runs (default 100).
Invalid runs (too many rejections → no INFERENCE line) are silently discarded.

Usage:
    python serial_listener_inference.py [COM_PORT] [--runs N] [--baud B]

Output: test_data_<timestamp_ms>.log
"""

import re
import sys
import time
import serial
import argparse
from datetime import datetime

BAUD_DEFAULT = 115200
RUNS_DEFAULT = 100

# ESP-IDF log prefix: "I (123456) TAG: " or "W (123456) TAG: "
RE_PREFIX         = re.compile(r'^[IVWDE] \(\d+\) [^:]+: ')

RE_TRAIN_DETECTED = re.compile(r'Train Detected')
RE_REJ_SAMPLE     = re.compile(r'Rejected ultrasonic sample ([-\d.]+) cm at idx (\d+); substituted ([-\d.]+) cm')
RE_REJ_COUNT      = re.compile(r'Rejected samples: (\d+)/(\d+)')
RE_RAW            = re.compile(r'^Raw Distances: (.+)')
RE_FILTERED       = re.compile(r'^Filtered Distances: (.+)')
RE_CONFIDENCE     = re.compile(r'confidence=([\d.]+) -> (OPEN|CLOSE)')
RE_RESULT         = re.compile(r'INFERENCE \((DT|MLP)\): Train (stopping/safe|approaching)')


def strip_espidf_prefix(line: str) -> str:
    m = RE_PREFIX.match(line)
    return line[m.end():] if m else line


def format_run(n: int, run: dict) -> str:
    lines = [f"RUN {n:03d}"]

    rc = run["rejected_count"]
    rt = run["rejected_total"]
    lines.append(f"rejected_count: {rc if rc is not None else '?'}/{rt if rt is not None else '?'}")

    for idx, orig, subst in run["rejected_samples"]:
        lines.append(f"rejected[{idx}]: {orig} -> {subst}")

    if run["raw"] is not None:
        lines.append(f"raw: {run['raw']}")
    if run["filtered"] is not None:
        lines.append(f"filtered: {run['filtered']}")
    if run["confidence"] is not None:
        lines.append(f"confidence: {run['confidence']:.4f} -> {run['confidence_dir']}")

    lines.append(f"model: {run['model'] or '?'}")
    lines.append(f"result: {'CLOSE' if run['result'] == 'approaching' else 'OPEN'}")
    lines.append("")
    return "\n".join(lines)


def console_summary(n: int, total: int, run: dict) -> str:
    result = "CLOSE" if run["result"] == "approaching" else "OPEN"
    rc = run["rejected_count"]
    rt = run["rejected_total"]
    conf_str = f"  conf={run['confidence']:.4f}" if run["confidence"] is not None else ""
    return f"  Run {n:3d}/{total}  {result:<5}  rej={rc}/{rt}  model={run['model'] or '?'}{conf_str}"


def new_run() -> dict:
    return {
        "rejected_samples": [],
        "rejected_count":   None,
        "rejected_total":   None,
        "raw":              None,
        "filtered":         None,
        "confidence":       None,
        "confidence_dir":   None,
        "model":            None,
        "result":           None,
    }


def main():
    parser = argparse.ArgumentParser(description="Record railway inference runs from serial.")
    parser.add_argument("port",   nargs="?", default="COM4", help="Serial port (default: COM4)")
    parser.add_argument("--runs", type=int,  default=RUNS_DEFAULT, help="Number of runs to collect")
    parser.add_argument("--baud", type=int,  default=BAUD_DEFAULT, help="Baud rate")
    args = parser.parse_args()

    session_ts  = int(time.time() * 1000)
    output_file = f"test_data_{session_ts}.log"

    print(f"Opening {args.port} at {args.baud} baud.")
    print(f"Target: {args.runs} runs  →  {output_file}")
    print("Press Ctrl+C to stop early.\n")

    run_count     = 0
    discard_count = 0
    current_run   = None

    try:
        with serial.Serial(args.port, args.baud, timeout=1) as ser:
            with open(output_file, "w", buffering=1) as f:
                f.write(f"# {output_file}\n")
                f.write(f"# Started:     {datetime.now().strftime('%Y-%m-%d %H:%M:%S')}\n")
                f.write(f"# Target runs: {args.runs}\n")
                f.write("#\n\n")

                while run_count < args.runs:
                    raw_bytes = ser.readline()
                    if not raw_bytes:
                        continue
                    line     = raw_bytes.decode("utf-8", errors="replace").strip()
                    stripped = strip_espidf_prefix(line)

                    # ── start of a new run ──────────────────────────────────
                    if RE_TRAIN_DETECTED.search(line):
                        if current_run is not None:
                            # previous run never produced an INFERENCE line → discard
                            discard_count += 1
                        current_run = new_run()
                        continue

                    if current_run is None:
                        continue

                    # ── individual rejected sample ───────────────────────────
                    m = RE_REJ_SAMPLE.search(stripped)
                    if m:
                        orig  = float(m.group(1))
                        idx   = int(m.group(2))
                        subst = float(m.group(3))
                        current_run["rejected_samples"].append((idx, orig, subst))
                        continue

                    # ── rejected count summary ───────────────────────────────
                    m = RE_REJ_COUNT.search(stripped)
                    if m:
                        current_run["rejected_count"] = int(m.group(1))
                        current_run["rejected_total"] = int(m.group(2))
                        continue

                    # ── raw distances (plain printf, no ESP-IDF prefix) ──────
                    m = RE_RAW.match(stripped)
                    if m:
                        current_run["raw"] = m.group(1).strip()
                        continue

                    # ── filtered distances ───────────────────────────────────
                    m = RE_FILTERED.match(stripped)
                    if m:
                        current_run["filtered"] = m.group(1).strip()
                        continue

                    # ── MLP confidence ───────────────────────────────────────
                    m = RE_CONFIDENCE.search(stripped)
                    if m:
                        current_run["confidence"]     = float(m.group(1))
                        current_run["confidence_dir"] = m.group(2)
                        continue

                    # ── inference result (terminal event for a run) ──────────
                    m = RE_RESULT.search(stripped)
                    if m:
                        current_run["model"]  = m.group(1)
                        current_run["result"] = m.group(2)
                        run_count += 1
                        f.write(format_run(run_count, current_run))
                        print(console_summary(run_count, args.runs, current_run))
                        current_run = None

                f.write(f"# DONE: {run_count} runs recorded, {discard_count} discarded.\n")

    except KeyboardInterrupt:
        print(f"\nStopped early after {run_count} runs ({discard_count} discarded).")
        print(f"Partial data saved to {output_file}.")
        sys.exit(0)
    except serial.SerialException as e:
        print(f"Serial error: {e}", file=sys.stderr)
        sys.exit(1)

    print(f"\nDone. {run_count} runs saved to {output_file} ({discard_count} discarded).")


if __name__ == "__main__":
    main()
