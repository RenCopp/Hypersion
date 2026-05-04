"""Full ELO grid calibration test for Hypersion's UCI_LimitStrength.

Tests Hypersion at every 200 Elo step from 500 to 2700 against a
calibrated opponent at the same target Elo:

  500   - Stockfish skill=0 nodes=1     (~ random play baseline)
  700   - Stockfish skill=0 nodes=10
  900   - Stockfish skill=0 nodes=100
  1100  - Maia 1100  (lichess-trained)
  1300  - Stockfish UCI_Elo=1320 (SF's lowest supported)
  1500  - Maia 1500
  1700  - Stockfish UCI_Elo=1700
  1900  - Maia 1900
  2100  - Stockfish UCI_Elo=2100
  2300  - Stockfish UCI_Elo=2300
  2500  - Stockfish UCI_Elo=2500

For each, we want Hypersion@N to score around 50% (40-60%).
"""

from __future__ import annotations

import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HYPERSION = ROOT / "Hypersion.exe"
LC0       = ROOT / "testing" / "lc0" / "lc0.exe"
MAIA_DIR  = ROOT / "testing" / "maia_weights"
SF        = Path(r"C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe")
CUTECHESS = Path(r"C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe")
OPENINGS  = ROOT / "testing" / "openings" / "popularpos_lichess_v3.epd"

# Each entry: (target_elo, opponent_kind, opponent_args_for_cutechess)
# opponent_kind is one of "maia", "sf_nodes", "sf_skill", "sf_uci_elo"
GRID = [
    (500,  "sf_random",  None),                  # SF random-ish
    (700,  "sf_random",  10),                    # SF nodes=10
    (900,  "sf_random",  100),                   # SF nodes=100
    (1100, "maia",       "maia-1100.pb.gz"),
    (1300, "sf_uci_elo", 1320),                  # SF lowest
    (1500, "maia",       "maia-1500.pb.gz"),
    (1700, "sf_uci_elo", 1700),
    (1900, "maia",       "maia-1900.pb.gz"),
    (2100, "sf_uci_elo", 2100),
    (2300, "sf_uci_elo", 2300),
    (2500, "sf_uci_elo", 2500),
]

GAMES = 10
TC    = "30+0.3"


def opponent_engine_args(kind: str, arg, target_elo: int) -> list[str]:
    """Build the cutechess `-engine` block for the opponent at target_elo."""
    if kind == "maia":
        weights = MAIA_DIR / arg
        if not weights.exists():
            return []
        return [
            "-engine", "name=Maia", f"cmd={LC0}",
            f"arg=--weights={weights}",
            "arg=--threads=1",
            "arg=--backend=eigen",
            "arg=--minibatch-size=1",
            "option.OwnBook=false",
        ]
    if kind == "sf_random":
        # arg is nodes (None = nodes 1)
        nodes_per_move = 1 if arg is None else int(arg)
        return [
            "-engine", "name=SF_random", f"cmd={SF}",
            "option.OwnBook=false",
            "option.Skill Level=0",
            f"st={nodes_per_move}",   # this is searchTime — not nodes-bound
            # actually use 'nodes=N' via tc workaround:
        ]
    if kind == "sf_uci_elo":
        return [
            "-engine", "name=SF", f"cmd={SF}",
            "option.OwnBook=false",
            "option.UCI_LimitStrength=true",
            f"option.UCI_Elo={int(arg)}",
        ]
    return []


def run_match(target_elo: int, opp_args: list[str], use_nodes: int | None) -> tuple[int, int, int, float]:
    """Run cutechess match and return (W, D, L, elapsed)."""
    if not opp_args:
        return 0, 0, 0, 0.0

    cmd = [
        str(CUTECHESS),
        "-engine", "name=Hypersion", f"cmd={HYPERSION}",
            "option.OwnBook=false",
            "option.UCI_LimitStrength=true",
            f"option.UCI_Elo={target_elo}",
    ] + opp_args + [
        "-each", "proto=uci", f"tc={TC}",
        "-openings", f"file={OPENINGS}", "format=epd",
            "order=sequential", "start=1", "plies=8",
        "-repeat", "-recover",
        "-concurrency", "4",
        "-games", str(GAMES),
        "-pgnout", str(ROOT / "testing" / f"sprt_grid_{target_elo}.pgn"),
    ]
    # If we want nodes-limited SF, add `-each nodes=N` instead of tc
    if use_nodes is not None:
        # replace tc with nodes
        idx = cmd.index("-each") + 2
        # remove `tc=...`
        for i, x in enumerate(cmd[idx-1:]):
            if x.startswith("tc="):
                cmd[idx-1+i] = f"nodes={use_nodes}"
                break

    t0 = time.time()
    try:
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                              errors="replace", timeout=900)
    except subprocess.TimeoutExpired:
        return 0, 0, 0, 900.0
    out = proc.stdout
    last_score = None
    for m in re.finditer(r"Score of \S+ vs \S+:\s+(\d+)\s*-\s*(\d+)\s*-\s*(\d+)", out):
        last_score = m
    elapsed = time.time() - t0
    if last_score is None:
        return 0, 0, 0, elapsed
    return int(last_score.group(1)), int(last_score.group(3)), int(last_score.group(2)), elapsed


def verdict(score: float) -> str:
    if 0.40 <= score <= 0.60:
        return f"OK"
    if score < 0.40:
        return f"WEAK ({int((score-0.5)*800):+d})"
    return f"STRONG (+{int((score-0.5)*800):+d})"


def main():
    print("=" * 78)
    print("Full ELO grid calibration: Hypersion @ N vs calibrated opponent @ N")
    print("=" * 78)
    if not HYPERSION.exists() or not SF.exists():
        print("ERROR: Hypersion or Stockfish missing"); return 2

    print(f"  {'Target':>6}  {'Opponent':<32}  {'W-D-L':<10}  {'Hyp%':>5}  {'verdict':<20}")
    print(f"  {'-'*6}  {'-'*32}  {'-'*10}  {'-'*5}  {'-'*20}")

    for target_elo, kind, arg in GRID:
        opp_args = opponent_engine_args(kind, arg, target_elo)
        if not opp_args:
            print(f"  {target_elo:>6}  (opponent unavailable)")
            continue
        # opp description
        if kind == "maia":      opp_desc = f"Maia ({arg})"
        elif kind == "sf_random":
            opp_desc = f"SF skill=0 nodes={arg if arg else 1}"
        elif kind == "sf_uci_elo": opp_desc = f"SF UCI_Elo={arg}"
        else: opp_desc = "?"

        use_nodes = arg if kind == "sf_random" else None
        w, d, l, dt = run_match(target_elo, opp_args, use_nodes)
        n = w + d + l
        if n == 0:
            print(f"  {target_elo:>6}  {opp_desc:<32}  (no games)  ({dt:.0f}s)")
            continue
        score = (w + d / 2) / n
        v = verdict(score)
        print(f"  {target_elo:>6}  {opp_desc:<32}  {w:>3}-{d:<3}-{l:<3}  {score:>5.1%}  {v}  ({dt:.0f}s)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
