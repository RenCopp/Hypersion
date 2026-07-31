"""Regression tests for Hypersion: bench-signature determinism + perft.

Run before every release / before submitting a PR.

Usage
-----
    py testing/regression.py [--exe PATH]

Exits 0 on pass, non-zero on any failure.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
DEFAULT_EXE = ROOT / ("Hypersion.exe" if sys.platform == "win32" else "Hypersion")

# Standard perft positions (Stockfish wiki). Format: (FEN, depth, expected_nodes).
PERFT_POSITIONS = [
    ("rnbqkbnr/pppppppp/8/8/8/8/PPPPPPPP/RNBQKBNR w KQkq - 0 1", 4, 197281),
    ("r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1", 4, 4085603),
    ("8/2p5/3p4/KP5r/1R3p1k/8/4P1P1/8 w - - 0 1", 5, 674624),
    ("r3k2r/Pppp1ppp/1b3nbN/nP6/BBP1P3/q4N2/Pp1P2PP/R2Q1RK1 w kq - 0 1", 4, 422333),
    ("rnbq1k1r/pp1Pbppp/2p5/8/2B5/8/PPP1NnPP/RNBQK2R w KQ - 1 8", 4, 2103487),
    ("r4rk1/1pp1qppp/p1np1n2/2b1p1B1/2B1P1b1/P1NP1N2/1PP1QPPP/R4RK1 w - - 0 10", 4, 3894594),
]


def run_uci(exe: Path, commands: list[str], timeout: int = 60) -> str:
    """Send commands to the engine, return stdout."""
    inp = "\n".join(commands) + "\nquit\n"
    res = subprocess.run(
        [str(exe)], input=inp, capture_output=True, text=True, timeout=timeout
    )
    return res.stdout


def run_bench(exe: Path) -> int:
    """Return the deterministic Threads=1 bench node count."""
    out = run_uci(exe, ["setoption name Threads value 1", "bench 13"], timeout=120)
    nodes = 0
    for line in out.splitlines():
        m = re.search(r"Nodes\s*:\s*(\d+)", line)
        if m:
            nodes = int(m.group(1))
    if nodes == 0:
        raise RuntimeError(f"bench produced no node count\nOutput:\n{out}")
    return nodes


def run_perft(exe: Path, fen: str, depth: int) -> int:
    """Run perft at depth, return total node count."""
    out = run_uci(
        exe,
        [f"position fen {fen}", f"go perft {depth}"],
        timeout=180,
    )
    for line in out.splitlines():
        m = re.match(r"Nodes searched:\s*(\d+)", line.strip())
        if m:
            return int(m.group(1))
        m = re.match(r"Total\s*:\s*(\d+)", line.strip())
        if m:
            return int(m.group(1))
    raise RuntimeError(f"perft {depth} did not produce a node total\nOutput tail:\n{out[-500:]}")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--exe", type=Path, default=DEFAULT_EXE)
    ap.add_argument("--skip-bench", action="store_true")
    ap.add_argument("--skip-perft", action="store_true")
    args = ap.parse_args()

    if not args.exe.exists():
        print(f"FAIL: engine not found at {args.exe}", file=sys.stderr)
        return 2

    print(f"== Hypersion regression suite ==")
    print(f"   exe: {args.exe}")

    failures: list[str] = []

    if not args.skip_bench:
        print("\n[1] bench (Threads=1, depth 13) ... ", end="", flush=True)
        try:
            nodes = run_bench(args.exe)
            print(f"OK  {nodes} nodes")

            sig_path = HERE / "BENCH_SIGNATURE"
            if sig_path.exists():
                expected = int(sig_path.read_text().strip())
                if nodes != expected:
                    msg = (
                        f"bench signature mismatch: expected {expected}, got {nodes}.\n"
                        f"   If the change was intentional (search semantics), update {sig_path}."
                    )
                    failures.append(msg)
                    print(f"   WARN: {msg}")
            else:
                failures.append(f"missing tracked bench signature: {sig_path}")
                print(f"   FAIL: missing tracked signature {sig_path}")
        except Exception as e:
            failures.append(f"bench failed: {e}")
            print(f"FAIL: {e}")

    if not args.skip_perft:
        print("\n[2] perft positions ...")
        for fen, depth, expected in PERFT_POSITIONS:
            print(f"  depth={depth} {fen[:40]}... ", end="", flush=True)
            try:
                got = run_perft(args.exe, fen, depth)
                if got == expected:
                    print(f"OK ({got})")
                else:
                    failures.append(f"perft mismatch: {fen} d{depth} expected {expected} got {got}")
                    print(f"FAIL: expected {expected}, got {got}")
            except Exception as e:
                failures.append(f"perft error: {e}")
                print(f"ERR: {e}")

    print("\n== Summary ==")
    if failures:
        print(f"FAIL: {len(failures)} issue(s)")
        for f in failures:
            print(f"  - {f}")
        return 1
    print("PASS")
    return 0


if __name__ == "__main__":
    sys.exit(main())
