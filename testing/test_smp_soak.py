#!/usr/bin/env python3
"""Fixed-thread-count UCI/SMP soak runner for Hypersion."""

from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / ("Hypersion.exe" if os.name == "nt" else "Hypersion")
MOVE_RE = re.compile(r"[a-h][1-8][a-h][1-8][qrbn]?")
POSITIONS = (
    "position startpos",
    "position startpos moves e2e4 e7e5 g1f3",
    "position startpos moves d2d4 d7d5 c2c4",
    "position fen r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1",
)


def build_commands(threads: int, searches: int, seed: int) -> list[str]:
    rng = random.Random(seed + threads * 1_000_003)
    commands = [
        "uci",
        "setoption name OwnBook value false",
        "setoption name Hash value 16",
        f"setoption name Threads value {threads}",
        "isready",
    ]

    for index in range(searches):
        commands.append(POSITIONS[index % len(POSITIONS)])
        mode = rng.randrange(5)
        if mode == 0:
            commands.append("go nodes 256")
        elif mode == 1:
            commands.append("go depth 2")
        elif mode == 2:
            commands.extend(("go infinite", "stop"))
        elif mode == 3:
            commands.extend(("go ponder", "ponderhit", "stop"))
        else:
            commands.append("go movetime 1")

        if index % 97 == 0:
            commands.append("setoption name Clear Hash")
        elif index % 53 == 0:
            commands.append("ucinewgame")
        if index % 100 == 0:
            commands.append("isready")

    commands.append("quit")
    return commands


def run_count(
    exe: Path,
    threads: int,
    searches: int,
    seed: int,
    timeout: float,
    with_nnue: bool,
) -> tuple[bool, str, float]:
    commands = build_commands(threads, searches, seed)
    argv = [str(exe)]
    if not with_nnue:
        argv.append("--no-nnue-default")

    started = time.perf_counter()
    proc = subprocess.Popen(
        argv,
        cwd=ROOT,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    try:
        output, _ = proc.communicate("\n".join(commands) + "\n", timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        output, _ = proc.communicate()
        elapsed = time.perf_counter() - started
        tail = "\n".join(output.splitlines()[-20:])
        return False, f"timed out after {timeout:g}s\n{tail}", elapsed

    elapsed = time.perf_counter() - started
    if proc.returncode != 0:
        tail = "\n".join(output.splitlines()[-20:])
        return False, f"exit code {proc.returncode}\n{tail}", elapsed
    if "uciok" not in output or "readyok" not in output:
        return False, "missing UCI readiness response", elapsed

    bestmoves = re.findall(r"^bestmove\s+(\S+)", output, flags=re.MULTILINE)
    if len(bestmoves) != searches:
        return False, f"expected {searches} bestmoves, received {len(bestmoves)}", elapsed
    invalid = [move for move in bestmoves if MOVE_RE.fullmatch(move) is None]
    if invalid:
        return False, f"invalid bestmove token: {invalid[0]}", elapsed
    return True, f"{searches} searches, {searches} legal-form bestmoves", elapsed


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--threads", type=int, nargs="+", default=[1, 2, 4, 8])
    parser.add_argument("--searches", type=int, default=1000)
    parser.add_argument("--seed", type=int, default=20260731)
    parser.add_argument("--timeout", type=float, default=300.0)
    parser.add_argument("--with-nnue", action="store_true")
    args = parser.parse_args()

    exe = args.exe.resolve()
    if not exe.is_file():
        parser.error(f"engine not found: {exe}")
    if args.searches < 1:
        parser.error("--searches must be at least 1")
    if any(count < 1 or count > 1024 for count in args.threads):
        parser.error("--threads values must be in [1, 1024]")

    print("=== Hypersion fixed-thread SMP soak ===")
    print(f"Engine: {exe}")
    print(f"Searches per thread count: {args.searches}")
    failures = 0
    for threads in args.threads:
        ok, detail, elapsed = run_count(
            exe, threads, args.searches, args.seed, args.timeout, args.with_nnue
        )
        print(
            f"  [{'PASS' if ok else 'FAIL'}] Threads={threads}: "
            f"{detail} ({elapsed:.2f}s)"
        )
        failures += int(not ok)

    if failures:
        print(f"\nFAIL: {failures} thread count(s)")
        return 1
    total = args.searches * len(args.threads)
    print(f"\nPASS: {total} searches across {len(args.threads)} thread counts")
    return 0


if __name__ == "__main__":
    sys.exit(main())
