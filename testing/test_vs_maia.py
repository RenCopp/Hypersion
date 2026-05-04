"""Test Hypersion's UCI_LimitStrength calibration vs Maia chess bots.

Maia is trained on lichess human games at specific rating levels:
  maia-1100 -> ~1100 lichess Elo (rapid)
  maia-1500 -> ~1500 lichess Elo
  maia-1900 -> ~1900 lichess Elo

Maia plays at depth 1 (`nodes=1`) — its strength comes from the network
predicting the most-likely human move, not search. So Maia is a stable,
human-calibrated opponent at each Elo level.

We test Hypersion at the matched UCI_Elo against each Maia, and look
for ~50 % score = correct calibration.
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
CUTECHESS = Path(r"C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe")
OPENINGS  = ROOT / "testing" / "openings" / "popularpos_lichess_v3.epd"

# (target_elo, maia_weights_filename)
MATCHUPS = [
    (1100, "maia-1100.pb.gz"),
    (1500, "maia-1500.pb.gz"),
    (1900, "maia-1900.pb.gz"),
]
GAMES = 10
TC    = "30+0.3"   # 30 s + 0.3 s — Maia is depth 1 so very fast


def run_match(maia_weights: Path, target_elo: int, games: int) -> tuple[int, int, int, float]:
    """Run cutechess Hypersion@target_elo vs lc0+Maia, return (W, D, L, elapsed)."""
    cmd = [
        str(CUTECHESS),
        "-engine", "name=Hypersion", f"cmd={HYPERSION}",
            "option.OwnBook=false",
            "option.UCI_LimitStrength=true",
            f"option.UCI_Elo={target_elo}",
        "-engine", "name=Maia", f"cmd={LC0}",
            f"arg=--weights={maia_weights}",
            "arg=--threads=1",
            "arg=--backend=eigen",
            "arg=--minibatch-size=1",
            "option.OwnBook=false",
        "-each", "proto=uci", f"tc={TC}",
        "-openings", f"file={OPENINGS}", "format=epd",
            "order=sequential", "start=1", "plies=8",
        "-repeat", "-recover",
        "-concurrency", "4",
        "-games", str(games),
        "-pgnout", str(ROOT / "testing" / f"sprt_maia_{target_elo}.pgn"),
    ]
    print(f"  Running {games} games at TC {TC}...", flush=True)
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=1200)
    elapsed = time.time() - t0

    out = proc.stdout
    last_score = None
    for m in re.finditer(r"Score of \S+ vs \S+:\s+(\d+)\s*-\s*(\d+)\s*-\s*(\d+)", out):
        last_score = m
    if last_score is None:
        print(f"  ERROR (no score parsed)")
        print(f"  stderr tail: {proc.stderr[-500:] if proc.stderr else 'empty'}")
        return 0, 0, 0, elapsed
    return int(last_score.group(1)), int(last_score.group(3)), int(last_score.group(2)), elapsed


def main():
    print("=" * 72)
    print("Hypersion@N vs Maia@N calibration test")
    print("=" * 72)
    if not HYPERSION.exists():
        print(f"ERROR: Hypersion not at {HYPERSION}"); return 2
    if not LC0.exists():
        print(f"ERROR: lc0 not at {LC0}"); return 2
    if not MAIA_DIR.exists():
        print(f"ERROR: maia weights dir missing"); return 2

    print(f"  {'Target Elo':<10}  {'W-D-L':<10}  {'Hyp%':>6}  {'verdict':<30}")
    print(f"  {'-'*10}  {'-'*10}  {'-'*6}  {'-'*30}")
    for target_elo, weights_file in MATCHUPS:
        weights_path = MAIA_DIR / weights_file
        if not weights_path.exists():
            print(f"  ELO {target_elo}: weights missing ({weights_file})")
            continue
        w, d, l, dt = run_match(weights_path, target_elo, GAMES)
        n = w + d + l
        if n == 0:
            print(f"  ELO {target_elo:<6}: no games completed")
            continue
        score = (w + d / 2) / n
        # Verdict: ±10% of 50% = OK calibration
        if 0.40 <= score <= 0.60:
            verdict = "OK (calibrated)"
        elif score > 0.60:
            verdict = f"STRONG (Hypersion over target by ~{int((score-0.5)*800):+d} Elo)"
        else:
            verdict = f"WEAK (Hypersion under target by ~{int((score-0.5)*800):+d} Elo)"
        print(f"  {target_elo:<10}  {w:>3}-{d:<3}-{l:<3}  {score:>5.1%}  {verdict}  ({dt:.0f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
