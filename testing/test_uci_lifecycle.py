#!/usr/bin/env python3
"""Bounded UCI and lazy-SMP lifecycle stress tests for Hypersion."""

from __future__ import annotations

import argparse
import os
import random
import re
import subprocess
import sys
from pathlib import Path


HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULT_EXE = ROOT / ("Hypersion.exe" if os.name == "nt" else "Hypersion")


def run_case(
    exe: Path,
    name: str,
    commands: list[str],
    timeout: float,
    with_nnue: bool,
) -> tuple[bool, str]:
    argv = [str(exe)]
    if not with_nnue:
        argv.append("--no-nnue-default")

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
    script = "\n".join(commands) + "\n"
    try:
        output, _ = proc.communicate(script, timeout=timeout)
    except subprocess.TimeoutExpired:
        proc.kill()
        output, _ = proc.communicate()
        tail = "\n".join(output.splitlines()[-20:])
        return False, f"{name}: timed out after {timeout:g}s\n{tail}"

    if proc.returncode != 0:
        tail = "\n".join(output.splitlines()[-20:])
        return False, f"{name}: exit code {proc.returncode}\n{tail}"
    if "uciok" not in output or "readyok" not in output:
        tail = "\n".join(output.splitlines()[-20:])
        return False, f"{name}: missing UCI readiness response\n{tail}"

    expected_bestmoves = sum(command.startswith("go") for command in commands)
    bestmoves = re.findall(r"^bestmove\s+(\S+)", output, flags=re.MULTILINE)
    if len(bestmoves) != expected_bestmoves:
        tail = "\n".join(output.splitlines()[-20:])
        return False, (
            f"{name}: expected {expected_bestmoves} bestmove lines, "
            f"received {len(bestmoves)}\n{tail}"
        )
    invalid = [
        move for move in bestmoves
        if re.fullmatch(r"[a-h][1-8][a-h][1-8][qrbn]?", move) is None
    ]
    if invalid:
        return False, f"{name}: invalid bestmove token(s): {', '.join(invalid)}"
    return True, name


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    parser.add_argument("--cycles", type=int, default=12)
    parser.add_argument("--seed", type=int, default=20260731)
    parser.add_argument("--timeout", type=float, default=15.0)
    parser.add_argument(
        "--with-nnue",
        action="store_true",
        help="load the configured NNUE files instead of using classical fallback",
    )
    args = parser.parse_args()
    exe = args.exe.resolve()
    if not exe.is_file():
        parser.error(f"engine not found: {exe}")
    if args.cycles < 1:
        parser.error("--cycles must be at least 1")

    cases: list[tuple[str, list[str]]] = []

    cases.append(
        (
            "clear-hash while searching",
            [
                "uci",
                "setoption name OwnBook value false",
                "setoption name Threads value 2",
                "position startpos",
                "go infinite",
                "setoption name Clear Hash",
                "isready",
                "quit",
            ],
        )
    )

    reconfigure = [
        "uci",
        "setoption name OwnBook value false",
        "setoption name Threads value 2",
        "position startpos",
        "go infinite",
        "setoption name Hash value 32",
        "isready",
        "position startpos moves e2e4 e7e5",
        "go infinite",
        "setoption name EvalUseSmallOnly value true",
        "isready",
        "position startpos",
        "go infinite",
        "setoption name SyzygyProbeDepth value 2",
        "isready",
        "quit",
    ]
    cases.append(("shared-resource reconfiguration", reconfigure))

    repeated = [
        "uci",
        "setoption name OwnBook value false",
        "setoption name Threads value 2",
    ]
    for cycle in range(args.cycles):
        repeated.extend(
            [
                "position startpos",
                "go infinite",
                "ucinewgame" if cycle % 2 == 0 else "stop",
                f"setoption name Threads value {1 + (cycle % 4)}",
                "isready",
            ]
        )
    repeated.append("quit")
    cases.append((f"{args.cycles} stop/new-game/resize cycles", repeated))

    cases.append(
        (
            "EOF while searching",
            [
                "uci",
                "setoption name OwnBook value false",
                "setoption name Threads value 2",
                "isready",
                "position startpos",
                "go infinite",
            ],
        )
    )

    cases.append(
        (
            "out-of-range UCI options",
            [
                "uci",
                "setoption name OwnBook value false",
                "setoption name Threads value 0",
                "setoption name MultiPV value -1",
                "setoption name Move Overhead value -999",
                "setoption name Hash value 0",
                "position startpos",
                "go depth 2",
                "isready",
                "quit",
            ],
        )
    )

    cases.append(
        (
            "malformed FEN is rejected",
            [
                "uci",
                "setoption name OwnBook value false",
                "isready",
                "position fen 8/8/8/8/8/8/8/8/Q7 w - - 0 1",
                "position startpos",
                "go depth 2",
                "quit",
            ],
        )
    )

    long_history = " ".join(("g1f3", "g8f6", "f3g1", "f6g8") * 550)
    cases.append(
        (
            "overlong position history is bounded",
            [
                "uci",
                "setoption name OwnBook value false",
                "isready",
                f"position startpos moves {long_history}",
                "go depth 2",
                "quit",
            ],
        )
    )

    rng = random.Random(args.seed)
    mixed = [
        "uci",
        "setoption name OwnBook value false",
        "setoption name Threads value 2",
        "isready",
    ]
    for cycle in range(args.cycles):
        if cycle % 3 == 0:
            mixed.append("position startpos moves e2e4 e7e5 g1f3")
        else:
            mixed.append("position startpos")
        mixed.append(rng.choice(("go infinite", "go ponder", "go nodes 5000")))

        action = rng.randrange(9)
        if action == 0:
            mixed.append("stop")
        elif action == 1:
            mixed.append("ucinewgame")
        elif action == 2:
            mixed.append(f"setoption name Threads value {rng.randint(1, 4)}")
        elif action == 3:
            mixed.append(f"setoption name Hash value {rng.choice((16, 32, 64))}")
        elif action == 4:
            mixed.append("setoption name Clear Hash")
        elif action == 5:
            value = "true" if rng.randrange(2) else "false"
            mixed.append(f"setoption name EvalUseSmallOnly value {value}")
        elif action == 6:
            mixed.append(f"setoption name SyzygyProbeDepth value {rng.randint(1, 8)}")
        elif action == 7:
            mixed.append("go depth 2")
        else:
            mixed.extend(("ponderhit", "stop"))
        mixed.append("isready")
    mixed.append("quit")
    cases.append((f"{args.cycles} fixed-seed mixed sequences (seed={args.seed})", mixed))

    print(f"=== Hypersion UCI lifecycle stress ({len(cases)} cases) ===")
    print(f"Engine: {exe}")
    failures: list[str] = []
    for name, commands in cases:
        ok, detail = run_case(exe, name, commands, args.timeout, args.with_nnue)
        print(f"  [{'PASS' if ok else 'FAIL'}] {name}")
        if not ok:
            failures.append(detail)

    if failures:
        print("\n" + "\n\n".join(failures))
        return 1
    print(f"\n{len(cases)}/{len(cases)} passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
