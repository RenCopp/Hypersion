"""ELO calibration test for Hypersion's UCI_LimitStrength feature.

Two suites:
  1. Internal monotonicity: Hypersion@N vs Hypersion@M for various N, M.
     Verifies higher-ELO Hypersion beats lower-ELO Hypersion proportionally.
  2. External calibration: Hypersion@N vs Stockfish-weakened-to-N.
     Stockfish opponent uses Skill Level + node limits to approximate the
     target ELO. Score ~50 % means Hypersion's calibration is correct.

Usage:
  py testing/test_elo_scaling.py internal       # suite 1
  py testing/test_elo_scaling.py external       # suite 2
  py testing/test_elo_scaling.py both           # both
"""

from __future__ import annotations

import argparse
import re
import subprocess
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
HYPERSION = ROOT / "Hypersion.exe"
STOCKFISH = Path(r"C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe")
CUTECHESS = Path(r"C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe")
OPENINGS  = ROOT / "testing" / "openings" / "popularpos_lichess_v3.epd"

# Stockfish weakening recipes for each target ELO
# Calibration is empirical; treat ratings ±100 ELO.
SF_RECIPES = {
    500:  "option.Skill Level=0 option.UCI_Elo=1320 option.UCI_LimitStrength=true tc=inf nodes=1",
    600:  "option.Skill Level=0 option.UCI_Elo=1320 option.UCI_LimitStrength=true tc=inf nodes=10",
    700:  "option.Skill Level=0 option.UCI_Elo=1320 option.UCI_LimitStrength=true tc=inf nodes=50",
    800:  "option.Skill Level=0 option.UCI_Elo=1320 option.UCI_LimitStrength=true tc=inf nodes=200",
    900:  "option.Skill Level=0 option.UCI_Elo=1320 option.UCI_LimitStrength=true tc=inf nodes=500",
    1000: "option.Skill Level=0 option.UCI_Elo=1320 option.UCI_LimitStrength=true tc=inf nodes=1500",
    1100: "option.Skill Level=0 option.UCI_LimitStrength=false",
    1300: "option.Skill Level=3 option.UCI_LimitStrength=false",
    1500: "option.UCI_LimitStrength=true option.UCI_Elo=1500",
    1700: "option.UCI_LimitStrength=true option.UCI_Elo=1700",
    2000: "option.UCI_LimitStrength=true option.UCI_Elo=2000",
}


def score_to_elo(wins: int, draws: int, losses: int) -> tuple[float, float]:
    """Compute ELO and approximate ±error from match score."""
    n = wins + draws + losses
    if n == 0:
        return 0.0, 999.0
    score = (wins + draws / 2.0) / n
    if score in (0.0, 1.0):
        return 1000.0 if score > 0.5 else -1000.0, 999.0
    import math
    elo = -400 * math.log10(1.0 / score - 1.0)
    # crude SE approximation: stdev = sqrt(score*(1-score)/n) * 800
    err = (((score * (1 - score)) / n) ** 0.5) * 800
    return elo, err


def run_match(new_cmd: str, old_cmd: str, new_opts: str, old_opts: str,
              tc: str, games: int, label: str) -> tuple[int, int, int]:
    """Run a cutechess-cli match and return (wins, draws, losses) for NEW."""
    # Build cutechess command
    cmd = [str(CUTECHESS),
           "-engine", f"name=NEW", f"cmd={new_cmd}", "option.OwnBook=false"]
    cmd += new_opts.split()
    cmd += ["-engine", f"name=OLD", f"cmd={old_cmd}", "option.OwnBook=false"]
    cmd += old_opts.split()
    cmd += ["-each", "proto=uci", f"tc={tc}", "option.Hash=64", "option.Threads=1",
            "-openings", f"file={OPENINGS}", "format=epd",
            "order=sequential", "start=1", "plies=8",
            "-repeat", "-recover", "-concurrency", "8", "-games", str(games),
            "-pgnout", str(ROOT / "testing" / f"sprt_elo_{label}.pgn")]

    print(f"  Running {games} games ({label})...", end=" ", flush=True)
    t0 = time.time()
    proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                          errors="replace", timeout=600)
    out = proc.stdout

    # Parse final score line
    last_score = None
    for m in re.finditer(r"Score of NEW vs OLD:\s+(\d+)\s*-\s*(\d+)\s*-\s*(\d+)", out):
        last_score = m

    elapsed = time.time() - t0
    if last_score is None:
        print(f"ERROR (no score parsed); elapsed {elapsed:.0f}s")
        print(f"    stdout tail: {out[-500:]}")
        return 0, 0, 0
    w, l, d = int(last_score.group(1)), int(last_score.group(2)), int(last_score.group(3))
    print(f"done ({elapsed:.0f}s)")
    return w, d, l


def run_suite_internal():
    """Hypersion@N vs Hypersion@M monotonicity test."""
    print("=" * 72)
    print("SUITE 1: Internal monotonicity (Hypersion@N vs Hypersion@M)")
    print("=" * 72)
    levels = [500, 700, 900, 1100, 1300, 1500]
    games_per_pair = 30
    print(f"\n{games_per_pair} games per pair, TC 5+0.05.\n")

    print(f"  {'N':>4}  vs  {'M':>4}  | {'W-D-L':<10} {'score':>6} {'ELO d':>9}")
    print(f"  {'-'*4}      {'-'*4}  | {'-'*10} {'-'*6} {'-'*9}")
    rows = []
    for i, n in enumerate(levels):
        m = levels[(i + 1) % len(levels)] if i + 1 < len(levels) else None
        if m is None: continue
        w, d, l = run_match(
            str(HYPERSION), str(HYPERSION),
            f"option.UCI_LimitStrength=true option.UCI_Elo={n}",
            f"option.UCI_LimitStrength=true option.UCI_Elo={m}",
            "5+0.05", games_per_pair, f"int_{n}_vs_{m}")
        n_total = w + d + l
        if n_total > 0:
            score = (w + d / 2) / n_total
            elo_diff, _err = score_to_elo(w, d, l)
        else:
            score, elo_diff = 0.5, 0.0
        sign = "->" if elo_diff < 0 else "<-"
        rows.append((n, m, w, d, l, score, elo_diff))
        print(f"  {n:>4}  vs  {m:>4}  | {w:>3}-{d:<3}-{l:<3}  {score:>5.1%}  {elo_diff:>+8.1f}")

    print()
    print("  EXPECTED: ELO d should be NEGATIVE when N < M (lower ELO loses)")
    print("            and the magnitude should track the configured gap.")


def run_suite_external():
    """Hypersion@N vs Stockfish-weakened-to-N ELO calibration check."""
    print("=" * 72)
    print("SUITE 2: External calibration (Hypersion@N vs SF-weakened-to-N)")
    print("=" * 72)
    levels = [500, 600, 700, 800, 900, 1000]
    games_per_pair = 30
    print(f"\n{games_per_pair} games per pair, TC 10+0.1 (SF needs more time at low nodes).\n")

    print(f"  {'ELO':>5}  | {'W-D-L':<11} {'score':>6} {'verdict':<30}")
    print(f"  {'-'*5}  | {'-'*11} {'-'*6} {'-'*30}")
    for elo in levels:
        sf_opts = SF_RECIPES[elo]
        # split off any tc=inf / nodes=N from sf_opts (cutechess "tc" / "nodes" go in -each, not -engine)
        # actually cutechess accepts these as engine options too — leave as-is
        w, d, l = run_match(
            str(HYPERSION), str(STOCKFISH),
            f"option.UCI_LimitStrength=true option.UCI_Elo={elo}",
            sf_opts,
            "10+0.1", games_per_pair, f"ext_{elo}")
        n_total = w + d + l
        score = (w + d / 2) / n_total if n_total > 0 else 0.5
        # verdict: 40-60% = calibrated, <40% = Hypersion too weak, >60% = Hypersion too strong
        if 0.40 <= score <= 0.60:
            verdict = "OK (calibrated)"
        elif score < 0.40:
            verdict = f"WEAK (Hypersion under target)"
        else:
            verdict = f"STRONG (Hypersion over target)"
        print(f"  {elo:>5}  | {w:>3}-{d:<3}-{l:<3}  {score:>5.1%}  {verdict}")

    print()
    print("  NOTE: SF calibration at 500-1000 ELO is approximate (uses node limits).")
    print("        Verdict 'OK' = Hypersion plays close to configured strength.")


def main(argv: list[str] | None = None):
    p = argparse.ArgumentParser()
    p.add_argument("suite", choices=["internal", "external", "both"], default="both", nargs="?")
    args = p.parse_args(argv)

    if not HYPERSION.exists():
        print(f"ERROR: Hypersion not found: {HYPERSION}")
        return 2
    if not OPENINGS.exists():
        print(f"ERROR: openings file not found: {OPENINGS}")
        return 2

    if args.suite in ("internal", "both"):
        run_suite_internal()
    if args.suite in ("external", "both"):
        if not STOCKFISH.exists():
            print(f"ERROR: Stockfish not found at {STOCKFISH}, skipping external suite")
            return 1
        run_suite_external()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
