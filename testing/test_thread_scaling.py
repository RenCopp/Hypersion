#!/usr/bin/env python3
"""Bounded fixed-node wall-clock scaling measurement for Hypersion."""

from __future__ import annotations

import argparse
import json
import queue
import re
import statistics
import subprocess
import sys
import threading
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / ("Hypersion.exe" if sys.platform == "win32" else "Hypersion")
FENS = (
    "startpos",
    "fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
    "fen 4rrk1/pp1n1ppp/2p1b3/3pP3/3P1P2/2N1B3/PP4PP/2R2RK1 w - - 0 18",
    "fen 8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1",
)
INFO_VALUE_RE = re.compile(r"\b(nodes|nps|time) (\d+)\b")


def measure(exe: Path, threads: int, nodes: int, repeats: int, timeout: float) -> dict:
    process = subprocess.Popen(
        [str(exe)],
        text=True,
        encoding="utf-8",
        errors="replace",
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        stdin=subprocess.PIPE,
        bufsize=1,
    )
    assert process.stdin is not None and process.stdout is not None
    lines: queue.Queue[str | None] = queue.Queue()

    def read_output() -> None:
        for line in process.stdout:
            lines.put(line.rstrip("\r\n"))
        lines.put(None)

    threading.Thread(target=read_output, daemon=True).start()

    def send(command: str) -> None:
        process.stdin.write(command + "\n")
        process.stdin.flush()

    def wait_for(marker: str, deadline: float) -> list[str]:
        captured = []
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                raise TimeoutError(f"Threads={threads}: timed out waiting for {marker}")
            line = lines.get(timeout=remaining)
            if line is None:
                raise RuntimeError(f"Threads={threads}: engine exited while waiting for {marker}")
            captured.append(line)
            if line.startswith(marker):
                return captured

    deadline = time.monotonic() + timeout
    send("uci")
    wait_for("uciok", deadline)
    send("setoption name OwnBook value false")
    send(f"setoption name Threads value {threads}")
    send("setoption name Hash value 64")
    send("isready")
    wait_for("readyok", deadline)

    times: list[int] = []
    nps_values: list[int] = []
    searches = repeats * len(FENS)
    try:
        for _ in range(repeats):
            for position in FENS:
                send(f"position {position}")
                send(f"go nodes {nodes}")
                output = wait_for("bestmove ", deadline)
                latest = None
                for line in output:
                    values = {key: int(value) for key, value in INFO_VALUE_RE.findall(line)}
                    if {"time", "nodes", "nps"} <= values.keys():
                        latest = (values["time"], values["nodes"], values["nps"])
                if latest is None:
                    raise RuntimeError(f"Threads={threads}: bestmove without final info")
                elapsed, _actual_nodes, nps = latest
                times.append(elapsed)
                nps_values.append(nps)
    finally:
        if process.poll() is None:
            send("quit")
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()

    return {
        "threads": threads,
        "searches": searches,
        "median_time_ms": round(statistics.median(times), 1),
        "median_nps": round(statistics.median(nps_values)),
        "min_nps": min(nps_values),
        "max_nps": max(nps_values),
    }


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--nodes", type=int, default=500_000)
    parser.add_argument("--repeats", type=int, default=3)
    parser.add_argument("--timeout", type=float, default=180.0)
    parser.add_argument("--json-output", type=Path)
    args = parser.parse_args()
    if args.nodes < 1 or args.repeats < 1 or any(value < 1 for value in args.threads):
        parser.error("threads, nodes, and repeats must be positive")
    exe = args.exe.resolve()
    if not exe.is_file():
        parser.error(f"engine not found: {exe}")

    rows = [measure(exe, value, args.nodes, args.repeats, args.timeout) for value in args.threads]
    baseline = rows[0]["median_nps"]
    for row in rows:
        row["speedup_vs_first"] = round(row["median_nps"] / baseline, 3)
        row["efficiency_vs_first"] = round(
            row["speedup_vs_first"] * rows[0]["threads"] / row["threads"], 3
        )
        print(
            f"Threads={row['threads']}: median {row['median_nps']:,} nps, "
            f"{row['median_time_ms']} ms, speedup {row['speedup_vs_first']:.3f}x, "
            f"efficiency {row['efficiency_vs_first']:.3f}"
        )

    report = {
        "schema": 1,
        "engine": str(exe),
        "nodes_per_search": args.nodes,
        "repeats": args.repeats,
        "positions": len(FENS),
        "results": rows,
    }
    if args.json_output:
        destination = args.json_output.resolve()
        destination.parent.mkdir(parents=True, exist_ok=True)
        destination.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
        print(f"JSON: {destination}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
