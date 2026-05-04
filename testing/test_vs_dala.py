"""Test Hypersion vs dala bots — human-rated bots specifically calibrated
to 700, 900, 1100 ELO (with actual lichess Rapid ratings 881-1100ish).

Reference: github.com/hrschubert/dala-training
Lichess accounts: @dala-700, @dala-900, @dala-1100

Goal: find what UCI_Elo setting makes Hypersion play roughly even
against each dala bot.
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
DALA_DIR  = ROOT / "testing" / "dala_weights"
CUTECHESS = Path(r"C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe")
OPENINGS  = ROOT / "testing" / "openings" / "popularpos_lichess_v3.epd"

# (target_elo, weights_filename, actual_lichess_rapid)
MATCHUPS = [
    (700,  "dala-700.pb.gz",  881),
    (900,  "dala-900.pb.gz",  1000),  # estimated; not confirmed
]
GAMES = 10
TC    = "30+0.3"


def run_match(weights: Path, target_elo: int) -> tuple[int, int, int, float]:
    cmd = [
        str(CUTECHESS),
        "-engine", "name=Hypersion", f"cmd={HYPERSION}",
            "option.OwnBook=false",
            "option.UCI_LimitStrength=true",
            f"option.UCI_Elo={target_elo}",
        "-engine", "name=Dala", f"cmd={LC0}",
            f"arg=--weights={weights}",
            "arg=--threads=1",
            "arg=--backend=eigen",
            "arg=--minibatch-size=1",
            "option.OwnBook=false",
        "-each", "proto=uci", f"tc={TC}",
        "-openings", f"file={OPENINGS}", "format=epd",
            "order=sequential", "start=1", "plies=8",
        "-repeat", "-recover",
        "-concurrency", "4",
        "-games", str(GAMES),
        "-pgnout", str(ROOT / "testing" / f"sprt_dala_{target_elo}.pgn"),
    ]
    print(f"  Running {GAMES} games at TC {TC}...", flush=True)
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


def main():
    print("=" * 78)
    print("Hypersion@N vs dala-N (lichess-calibrated low-ELO bots)")
    print("=" * 78)
    print(f"  {'Target':>6} {'vs dala':<14} {'(actual)':<10} {'W-D-L':<10} {'Hyp%':>5}  verdict")
    print(f"  {'-'*6} {'-'*14} {'-'*10} {'-'*10} {'-'*5}  {'-'*30}")
    for target_elo, weights_name, actual_rapid in MATCHUPS:
        weights = DALA_DIR / weights_name
        if not weights.exists():
            print(f"  ELO {target_elo}: weights missing")
            continue
        w, d, l, dt = run_match(weights, target_elo)
        n = w + d + l
        if n == 0:
            print(f"  ELO {target_elo}: no games")
            continue
        score = (w + d / 2) / n
        if 0.40 <= score <= 0.60:
            verdict = "OK calibrated"
        elif score < 0.40:
            verdict = f"WEAK ({int((score-0.5)*800):+d})"
        else:
            verdict = f"STRONG (+{int((score-0.5)*800):+d})"
        print(f"  {target_elo:>6} dala-{target_elo:<10} (~{actual_rapid:>5}) "
              f"{w:>3}-{d:<3}-{l:<3}  {score:>5.1%}  {verdict}  ({dt:.0f}s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
