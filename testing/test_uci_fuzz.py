#!/usr/bin/env python3
"""Deterministic malformed UCI/FEN stress test with a bounded command count."""

from __future__ import annotations

import argparse
import random
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / ("Hypersion.exe" if sys.platform == "win32" else "Hypersion")


def malformed_fens(rng: random.Random, count: int) -> list[str]:
    fixed = [
        "8/8/8/8/8/8/8/8 w - - 0 1",
        "8/8/8/8/8/8/8/K6k w - - 0",
        "8/8/8/8/8/8/8/K6k x - - 0 1",
        "9/8/8/8/8/8/8/K6k w - - 0 1",
        "8/8/8/8/8/8/8/K6k w KK - 0 1",
        "8/8/8/8/8/8/8/K6k w - a4 0 1",
        "8/8/8/8/8/8/8/K6k w - - -1 1",
        "8/8/8/8/8/8/8/K6k w - - 0 0",
        "8/8/8/8/8/8/8/K6k w - - 999999999999999999999 1",
        "4k3/8/8/4P3/8/8/8/4K3 w - d6 0 1",
        "k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 1",
        "4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 2147483647",
        "P7/8/8/8/8/8/8/K6k w - - 0 1",
    ]
    alphabet = "pnbrqkPNBRQK123456789/x-abcdefghKQ "
    generated = [
        "".join(rng.choice(alphabet) for _ in range(rng.randint(1, 120)))
        for _ in range(max(0, count - len(fixed)))
    ]
    return (fixed + generated)[:count]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--cases", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=20260731)
    parser.add_argument("--timeout", type=float, default=30.0)
    args = parser.parse_args()

    if args.cases < 1:
        parser.error("--cases must be positive")
    exe = args.exe.resolve()
    if not exe.is_file():
        parser.error(f"engine not found: {exe}")

    rng = random.Random(args.seed)
    commands = ["uci", "setoption name OwnBook value false"]
    ready_count = 0
    for index, fen in enumerate(malformed_fens(rng, args.cases), 1):
        commands.append(f"position fen {fen}")
        if index % 50 == 0:
            commands.append("isready")
            ready_count += 1
        if index % 100 == 0:
            commands.append("this-command-does-not-exist " + "x" * (index % 257))

    commands.extend(
        (
            "go perft",
            "go perft -999999999999999999999",
            "perft nonsense",
            "bench -1",
            "setoption name SyzygyProbeDepth value not-a-number",
            "setoption name SyzygyProbeLimit value 999999999999999999999",
            "setoption name Tune_RFP_MARGIN_PER_DEPTH value 999999999999999999999",
            "position startpos",
            "go depth 5",
            "quit",
        )
    )
    try:
        result = subprocess.run(
            [str(exe), "--no-nnue-default"],
            input="\n".join(commands) + "\n",
            text=True,
            encoding="utf-8",
            errors="replace",
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            timeout=args.timeout,
        )
    except subprocess.TimeoutExpired:
        print(f"FAIL: engine timed out after {args.timeout}s", file=sys.stderr)
        return 1

    output = result.stdout
    failures = []
    if result.returncode != 0:
        failures.append(f"engine exited with {result.returncode}")
    if output.count("readyok") != ready_count:
        failures.append(f"expected {ready_count} readyok lines, got {output.count('readyok')}")
    if not any(line.startswith("bestmove ") for line in output.splitlines()):
        failures.append("valid recovery search produced no bestmove")
    if "uciok" not in output:
        failures.append("missing uciok")

    if failures:
        print("FAIL: " + "; ".join(failures), file=sys.stderr)
        print(output[-2000:], file=sys.stderr)
        return 1
    print(
        f"PASS: {args.cases} malformed FEN/command cases, seed={args.seed}, "
        f"{ready_count} readiness barriers, valid search recovery"
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
