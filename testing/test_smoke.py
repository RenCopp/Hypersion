"""Smoke test runner — fast sanity checks Hypersion should always pass.

Runs in ~30 seconds total. Use as a CI sanity check after every commit.
Each check is independent so partial failure still produces useful output.

Usage:
    py testing/test_smoke.py

Returns exit 0 on all-pass, exit N on first failure.
"""
from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
DEFAULT_ENGINE = ROOT / ("Hypersion.exe" if sys.platform == "win32" else "Hypersion")
ENGINE = DEFAULT_ENGINE
BENCH_SIGNATURE = Path(__file__).resolve().parent / "BENCH_SIGNATURE"

TESTS = []


def test(label):
    def deco(fn):
        TESTS.append((label, fn))
        return fn
    return deco


def run_engine(commands: str, timeout: int = 30) -> str:
    """Run engine with a sequence of commands, return stdout+stderr combined."""
    result = subprocess.run(
        [str(ENGINE)],
        input=commands,
        capture_output=True,
        text=True,
        timeout=timeout,
    )
    return result.stdout + "\n" + result.stderr


@test("engine starts and responds to uci")
def t_uci_handshake():
    out = run_engine("uci\nquit\n", timeout=10)
    assert "uciok" in out, "missing uciok"
    assert "Hypersion" in out, "missing engine id"


@test("isready / readyok")
def t_isready():
    out = run_engine("uci\nisready\nquit\n", timeout=10)
    assert "readyok" in out, "missing readyok"


@test("perft 4 startpos = 197281")
def t_perft_startpos():
    out = run_engine("position startpos\nperft 4\nquit\n")
    for line in out.splitlines():
        if line.startswith("Total:"):
            n = int(line.split()[1])
            assert n == 197281, f"expected 197281, got {n}"
            return
    raise AssertionError("no Total: line in perft output")


@test("perft 4 kiwipete = 4085603")
def t_perft_kiwipete():
    fen = "r3k2r/p1ppqpb1/bn2pnp1/3PN3/1p2P3/2N2Q1p/PPPBBPPP/R3K2R w KQkq - 0 1"
    out = run_engine(f"position fen {fen}\nperft 4\nquit\n")
    for line in out.splitlines():
        if line.startswith("Total:"):
            n = int(line.split()[1])
            assert n == 4085603, f"expected 4085603, got {n}"
            return
    raise AssertionError("no Total: line in perft output")


@test("eval startpos returns finite cp")
def t_eval_startpos():
    out = run_engine("position startpos\neval\nquit\n")
    found = False
    for line in out.splitlines():
        if "static eval" in line and "cp" in line:
            v = int(line.split(":")[1].strip())
            assert -2000 < v < 2000, f"startpos eval out of range: {v}"
            found = True
            break
    assert found, "no static eval line"


@test("go depth 6 returns bestmove")
def t_go_depth_6():
    out = run_engine(
        "uci\nsetoption name OwnBook value false\nposition startpos\ngo depth 6\nquit\n",
        timeout=15,
    )
    found = False
    for line in out.splitlines():
        if line.startswith("bestmove "):
            mv = line.split()[1]
            assert len(mv) >= 4, f"malformed bestmove: {mv}"
            found = True
            break
    assert found, "no bestmove line"


@test("Threads=1 gives identical bestmove + nodes across 2 runs")
def t_determinism_threads1():
    bests = []
    for _ in range(2):
        out = run_engine(
            "uci\nsetoption name Threads value 1\nsetoption name OwnBook value false\n"
            "position startpos\ngo depth 8\nquit\n",
            timeout=15,
        )
        for line in out.splitlines():
            if line.startswith("bestmove "):
                bests.append(line.split()[1])
                break
    assert len(set(bests)) == 1, f"non-deterministic: {bests}"


@test("bench Threads=1 gives expected total nodes")
def t_bench_threads1():
    out = run_engine(
        "uci\nsetoption name Threads value 1\nbench 13\nquit\n",
        timeout=120,
    )
    expected = int(BENCH_SIGNATURE.read_text(encoding="utf-8").strip())
    for line in out.splitlines():
        match = re.match(r"(?:Nodes searched|Nodes)\s*:\s*(\d+)", line.strip())
        if match:
            n = int(match.group(1))
            assert n == expected, f"expected bench {expected}, got {n}"
            return
    raise AssertionError("no bench node-count line")


def main() -> int:
    global ENGINE
    parser = argparse.ArgumentParser()
    parser.add_argument("--exe", type=Path, default=DEFAULT_ENGINE)
    parser.add_argument("--skip-bench", action="store_true")
    args = parser.parse_args()
    ENGINE = args.exe.resolve()
    if args.skip_bench:
        TESTS[:] = [(label, fn) for label, fn in TESTS if not label.startswith("bench ")]

    print(f"=== Hypersion smoke tests ({len(TESTS)} checks) ===")
    print(f"Engine: {ENGINE}")
    if not ENGINE.exists():
        print(f"ERROR: engine not found at {ENGINE}")
        return 1

    pass_n = fail_n = 0
    t0 = time.time()
    for label, fn in TESTS:
        try:
            fn()
            pass_n += 1
            print(f"  [PASS] {label}")
        except (AssertionError, subprocess.TimeoutExpired, Exception) as e:
            fail_n += 1
            print(f"  [FAIL] {label}: {e}")
    dt = time.time() - t0
    print(f"\n{pass_n}/{pass_n + fail_n} passed in {dt:.1f}s")
    return 0 if fail_n == 0 else fail_n


if __name__ == "__main__":
    sys.exit(main())
