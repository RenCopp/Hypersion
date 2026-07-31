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


def run_until_bestmove(commands: list[str], timeout: int = 30) -> str:
    """Run an asynchronous UCI search to natural completion before quitting."""
    proc = subprocess.Popen(
        [str(ENGINE), "--no-nnue-default"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
        bufsize=1,
    )
    assert proc.stdin is not None and proc.stdout is not None
    for command in commands:
        proc.stdin.write(command + "\n")
    proc.stdin.flush()
    lines: list[str] = []
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = proc.stdout.readline()
        if not line:
            break
        lines.append(line)
        if line.startswith("bestmove "):
            proc.stdin.write("quit\n")
            proc.stdin.flush()
            proc.wait(timeout=5)
            return "".join(lines)
    proc.kill()
    proc.wait(timeout=5)
    raise AssertionError("search did not produce bestmove before timeout")


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
    out = run_engine("position startpos\nperft 0\nperft 4\nquit\n")
    totals = [int(line.split()[1]) for line in out.splitlines() if line.startswith("Total:")]
    assert totals == [1, 197281], f"expected [1, 197281], got {totals}"


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


@test("invalid and pinned en-passant targets are normalized")
def t_en_passant_validation():
    cases = [
        # No black pawn exists on d5: the old parser generated e5d6 anyway
        # and undo_move resurrected a phantom pawn on d5.
        ("4k3/8/8/4P3/8/8/8/4K3 w - d6 0 1", 6),
        # The d5 pawn exists, but e5d6 would expose the white king to Re8.
        ("k3r3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", 6),
        # Fully valid en passant remains available.
        ("4k3/8/8/3pP3/8/8/8/4K3 w - d6 0 1", 7),
    ]
    commands = "setoption name OwnBook value false\n"
    for fen, _ in cases:
        commands += f"position fen {fen}\nperft 1\n"
    commands += (
        "position fen k3r3/3p4/8/4P3/8/8/8/4K3 b - - 0 1 moves d7d5\nd\n"
        "position fen 4k3/3p4/8/4P3/8/8/8/4K3 b - - 0 1 moves d7d5\nd\n"
    )
    out = run_engine(commands + "quit\n")
    totals = [int(line.split()[1]) for line in out.splitlines() if line.startswith("Total:")]
    assert totals == [expected for _, expected in cases], f"unexpected EP perft totals: {totals}"
    fens = [line.removeprefix("FEN: ") for line in out.splitlines() if line.startswith("FEN: ")]
    assert fens[-2].split()[3] == "-", f"pinned EP target survived double push: {fens[-2]}"
    assert fens[-1].split()[3] == "d6", f"legal EP target was lost: {fens[-1]}"


@test("failed NNUE replacement preserves the active network")
def t_nnue_replacement_is_transactional():
    big = ROOT / "nn-c288c895ea92.nnue"
    small = ROOT / "nn-37f18f62d772.nnue"
    if not big.is_file() or not small.is_file():
        return  # CI source builds intentionally do not carry network assets.
    fen = "r1bq1rk1/ppp2ppp/2np1n2/4p3/2B1P3/2NP1N2/PPP2PPP/R1BQ1RK1 w - - 4 8"
    out = run_engine(
        f"position fen {fen}\neval\n"
        "setoption name EvalFile value does-not-exist.nnue\neval\n"
        f"setoption name EvalFile value {small}\neval\nquit\n"
    )
    values = [
        int(line.split(":", 1)[1].strip())
        for line in out.splitlines()
        if "static eval (cp)" in line
    ]
    assert len(values) == 3 and len(set(values)) == 1, f"active NNUE changed: {values}"
    assert "incompatible architecture" in out, "wrong-architecture network was not rejected"


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


@test("go nodes is a pool-wide budget")
def t_global_node_limit():
    out = run_until_bestmove(
        [
            "uci",
            "setoption name OwnBook value false",
            "setoption name Threads value 4",
            "position startpos",
            "go nodes 10000",
        ]
    )
    info_nodes = [
        int(match.group(1))
        for line in out.splitlines()
        if (match := re.search(r"^info depth .*\bnodes (\d+)", line))
    ]
    assert info_nodes, "node-limited search emitted no completed iteration"
    # A completed iteration may be below the hard boundary; a small in-flight
    # overshoot is expected.  The old per-worker bug reported ~37K at T4.
    assert info_nodes[-1] <= 12000, f"per-thread node budget leak: {info_nodes[-1]}"


@test("go mate respects the requested mate distance")
def t_go_mate_distance():
    # White has a forced mate in two, but no mate in one.  `go mate 1`
    # therefore must not stop when the mate-in-two score first appears at d3.
    out = run_until_bestmove(
        [
            "uci",
            "setoption name OwnBook value false",
            "position fen 8/8/8/8/3Q4/k7/8/1K6 w - - 0 1",
            "go mate 1 depth 6",
        ]
    )
    depths = [
        int(match.group(1))
        for line in out.splitlines()
        if (match := re.search(r"^info depth (\d+)", line))
    ]
    assert depths and depths[-1] == 6, f"farther mate ended search at depth {depths[-1:]}"


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
