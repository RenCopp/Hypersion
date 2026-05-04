"""SPRT (Sequential Probability Ratio Test) harness for Hypersion development.

Wraps cutechess-cli with a sane default config and parses output live so
each candidate vs baseline test produces a clean PASS / FAIL / INCONCLUSIVE
verdict plus the final ELO +- error.

Usage
-----
  # standard test: candidate exe vs baseline exe, SPRT [0, 5]
  py sprt.py --new dist\\Hypersion_candidate.exe --old testing\\Hypersion_baseline.exe

  # custom bounds (e.g. looking for big regression)
  py sprt.py --new ... --old ... --elo0 -10 --elo1 0

  # quick fixed-game match (no SPRT, just N games + ELO)
  py sprt.py --new ... --old ... --games 200 --no-sprt

  # null sanity test: same exe vs itself, must REJECT (LLR -> elo1)
  py sprt.py --new dist\\Hypersion.exe --old dist\\Hypersion.exe --null

Standard SPRT bounds we use throughout the project:
  elo0 = 0      (H0: no improvement)
  elo1 = 5      (H1: at least +5 ELO)
  alpha = beta = 0.05

Default time control is 10+0.1, which is fast enough to converge in 1-3
hours on a modern desktop yet long enough to be a useful proxy for play
strength at longer TCs.
"""

from __future__ import annotations

import argparse
import datetime as _dt
import os
import re
import shlex
import signal
import subprocess
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional


# --------------------------------------------------------------------------
# Paths / defaults
# --------------------------------------------------------------------------
HERE = Path(__file__).resolve().parent
ROOT = HERE.parent

DEFAULT_CUTECHESS = Path(r"C:\Engine\cutechess-bin\cutechess-1.4.0-win64\cutechess-cli.exe")
# popularpos_lichess_v3.epd is the official-stockfish/books recommended
# diverse opening source — 200,000 real lichess positions in proper EPD
# format. Older default (eco.bin polyglot) gave inconsistent measurements
# because it's a binary opening book, not a position list.
DEFAULT_OPENINGS  = HERE / "openings" / "popularpos_lichess_v3.epd"
DEFAULT_BASELINE  = ROOT / "Hypersion.exe"
DEFAULT_CANDIDATE = ROOT / "Hypersion.exe"

DEFAULT_TC          = "10+0.1"
DEFAULT_HASH_MB     = 64
DEFAULT_THREADS     = 1
DEFAULT_CONCURRENCY = 4
DEFAULT_MAX_GAMES   = 5000
DEFAULT_ELO0        = 0
DEFAULT_ELO1        = 5
DEFAULT_ALPHA       = 0.05
DEFAULT_BETA        = 0.05


# --------------------------------------------------------------------------
# cutechess-cli output parsing
# --------------------------------------------------------------------------
# Lines we care about look like one of:
#   Score of NEW vs OLD: 12 - 8 - 30  [0.534] 50
#   Elo difference: 23.7 +/- 38.4, LOS: 89.3 %, DrawRatio: 60.0 %
#   SPRT: llr 0.83 (28.0%), lbound -2.94, ubound 2.94 - H1 was accepted
RE_SCORE = re.compile(
    r"Score of (\S+) vs (\S+):\s+(\d+)\s*-\s*(\d+)\s*-\s*(\d+)"
    r"\s+\[([\d.]+)\]\s+(\d+)"
)
RE_ELO   = re.compile(r"Elo difference:\s+([\-\d.]+)\s+\+/\-\s+([\d.]+)")
RE_SPRT  = re.compile(
    r"SPRT:\s+llr\s+([\-\d.]+)\s*\([^)]+\),\s+lbound\s+([\-\d.]+),"
    r"\s+ubound\s+([\-\d.]+)"
)
RE_SPRT_ACCEPT = re.compile(r"H([01]) was accepted")


@dataclass
class SprtState:
    new_name:    str  = ""
    old_name:    str  = ""
    wins:        int  = 0
    losses:      int  = 0
    draws:       int  = 0
    games:       int  = 0
    elo:         float = 0.0
    elo_err:     float = 0.0
    llr:         float = 0.0
    lbound:      float = 0.0
    ubound:      float = 0.0
    accepted:    Optional[int] = None    # 0 or 1, or None
    last_status: str  = ""

    def line(self) -> str:
        score = f"{self.wins}-{self.losses}-{self.draws}"
        return (
            f"games={self.games:>5}  "
            f"score={score:<14}  "
            f"elo={self.elo:+7.2f} +/- {self.elo_err:5.2f}  "
            f"LLR={self.llr:+5.2f} [{self.lbound:+.2f},{self.ubound:+.2f}]"
        )


# --------------------------------------------------------------------------
# Driver
# --------------------------------------------------------------------------
def build_command(args, pgn_path: Path) -> list[str]:
    each_opts = [
        "proto=uci",
        f"tc={args.tc}",
        f"option.Hash={args.hash}",
        f"option.Threads={args.threads}",
    ]
    # Opening order: `random` introduces match-specific selection bias.
    # Chained inferences across multiple A/B tests with random-opening
    # selection are unreliable (verified empirically in this codebase).
    # Default to `ordered` so every match draws from the same prefix of
    # the opening file — that makes chain inferences valid.  `--random-
    # openings` re-enables the old behaviour for one-off matches.
    opening_order = "random" if args.random_openings else "sequential"
    cmd = [str(args.cutechess)]
    cmd += [
        "-engine", f"name=NEW", f"cmd={args.new}", "option.OwnBook=false",
        "-engine", f"name=OLD", f"cmd={args.old}", "option.OwnBook=false",
        "-each", *each_opts,
        "-openings", f"file={args.openings}", "format=epd",
        f"order={opening_order}", f"start={args.opening_start}", "plies=8",
        "-repeat",
        "-recover",
        "-concurrency", str(args.concurrency),
        "-games", str(args.games),
        "-ratinginterval", "1",
        "-pgnout", str(pgn_path),
    ]
    if not args.no_sprt:
        cmd += [
            "-sprt",
            f"elo0={args.elo0}",
            f"elo1={args.elo1}",
            f"alpha={args.alpha}",
            f"beta={args.beta}",
        ]
    return cmd


def stream_run(cmd: list[str], state: SprtState, log_path: Path) -> int:
    """Spawn cutechess-cli and stream its output, updating state inline."""
    print(f"[sprt] launching: {shlex.join(cmd)}")
    print(f"[sprt] log file:  {log_path}")
    print(f"[sprt] starting at {_dt.datetime.now().strftime('%Y-%m-%d %H:%M:%S')}")
    print()

    # Open process; cutechess-cli prints to stdout
    proc = subprocess.Popen(
        cmd,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        bufsize=1,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    log_f = log_path.open("w", encoding="utf-8")

    last_print = 0.0
    try:
        for line in proc.stdout:                    # type: ignore[union-attr]
            log_f.write(line)
            log_f.flush()

            stripped = line.rstrip()

            m = RE_SCORE.search(stripped)
            if m:
                state.new_name = m.group(1)
                state.old_name = m.group(2)
                state.wins   = int(m.group(3))
                state.losses = int(m.group(4))
                state.draws  = int(m.group(5))
                state.games  = int(m.group(7))

            m = RE_ELO.search(stripped)
            if m:
                state.elo     = float(m.group(1))
                state.elo_err = float(m.group(2))

            m = RE_SPRT.search(stripped)
            if m:
                state.llr    = float(m.group(1))
                state.lbound = float(m.group(2))
                state.ubound = float(m.group(3))

            m = RE_SPRT_ACCEPT.search(stripped)
            if m:
                state.accepted = int(m.group(1))

            # Throttle progress prints to once / sec to avoid console flood
            now = time.time()
            if now - last_print > 1.0 and state.games > 0:
                print("\r" + state.line() + "   ", end="", flush=True)
                last_print = now

        ret = proc.wait()
    except KeyboardInterrupt:
        print("\n[sprt] Ctrl-C: terminating cutechess-cli ...")
        try:
            proc.send_signal(signal.SIGINT)
            proc.wait(timeout=5)
        except Exception:
            proc.kill()
        ret = 130
    finally:
        log_f.close()
        print()  # newline after the carriage-return progress

    return ret


def verdict(args, state: SprtState) -> str:
    if state.accepted == 1:
        return f"PASS  (H1 accepted, candidate >= +{args.elo1} ELO)"
    if state.accepted == 0:
        return f"FAIL  (H0 accepted, candidate <= +{args.elo0} ELO)"
    if args.no_sprt:
        return f"FIXED-GAMES match complete: {state.games} games"
    return "INCONCLUSIVE  (game cap hit before either bound)"


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("--new", type=Path, default=DEFAULT_CANDIDATE,
                   help="Candidate engine exe (default: %(default)s)")
    p.add_argument("--old", type=Path, default=DEFAULT_BASELINE,
                   help="Baseline engine exe  (default: %(default)s)")
    p.add_argument("--cutechess", type=Path, default=DEFAULT_CUTECHESS,
                   help="cutechess-cli.exe path")
    p.add_argument("--openings", type=Path, default=DEFAULT_OPENINGS,
                   help="Opening book .epd or .pgn or polyglot .bin")

    p.add_argument("--tc",          default=DEFAULT_TC,         help="time control (default %(default)s)")
    p.add_argument("--hash",        type=int, default=DEFAULT_HASH_MB,    help="hash MB / engine")
    p.add_argument("--threads",     type=int, default=DEFAULT_THREADS,    help="threads / engine")
    p.add_argument("--concurrency", type=int, default=DEFAULT_CONCURRENCY,help="concurrent games")

    p.add_argument("--games",  type=int, default=DEFAULT_MAX_GAMES, help="max games")
    p.add_argument("--elo0",   type=float, default=DEFAULT_ELO0,    help="SPRT H0 lower (default %(default)s)")
    p.add_argument("--elo1",   type=float, default=DEFAULT_ELO1,    help="SPRT H1 upper (default %(default)s)")
    p.add_argument("--alpha",  type=float, default=DEFAULT_ALPHA)
    p.add_argument("--beta",   type=float, default=DEFAULT_BETA)
    p.add_argument("--no-sprt", action="store_true",
                   help="Run a fixed-games match instead of SPRT")
    p.add_argument("--null",   action="store_true",
                   help="Sanity null test: --old defaults to --new")

    p.add_argument("--label",  default=None,
                   help="Tag for output PGN/log filenames")
    p.add_argument("--random-openings", action="store_true",
                   help="Use cutechess `order=random` (default: sequential, reproducible)")
    p.add_argument("--opening-start", type=int, default=1,
                   help="Starting position index in the opening file (default 1)")

    args = p.parse_args(argv)

    if args.null:
        args.old = args.new

    # Validate paths
    for name, pth in [("cutechess", args.cutechess), ("new", args.new),
                      ("old", args.old), ("openings", args.openings)]:
        if not pth.exists():
            print(f"ERROR: {name} not found: {pth}", file=sys.stderr)
            return 2

    label = args.label or f"{args.new.stem}_vs_{args.old.stem}"
    label = re.sub(r"[^A-Za-z0-9_.-]+", "_", label)
    stamp = _dt.datetime.now().strftime("%Y%m%d_%H%M%S")
    pgn   = HERE / f"sprt_{label}_{stamp}.pgn"
    log   = HERE / f"sprt_{label}_{stamp}.log"

    state = SprtState()
    cmd = build_command(args, pgn)
    t0  = time.time()
    rc  = stream_run(cmd, state, log)
    dt  = time.time() - t0

    print()
    print("=" * 72)
    print(f"VERDICT: {verdict(args, state)}")
    print(f"  games:  {state.games}  (+{state.wins} ={state.draws} -{state.losses})")
    print(f"  elo:    {state.elo:+.2f} +/- {state.elo_err:.2f}")
    print(f"  LLR:    {state.llr:+.2f}  bounds [{state.lbound:+.2f}, {state.ubound:+.2f}]")
    print(f"  time:   {dt:.0f}s ({dt/60:.1f} min)")
    print(f"  pgn:    {pgn}")
    print(f"  log:    {log}")
    print("=" * 72)

    # Exit code: 0 = pass, 1 = fail, 2 = inconclusive, 3 = error
    if rc != 0 and rc != 130:
        return 3
    if state.accepted == 1: return 0
    if state.accepted == 0: return 1
    return 2 if not args.no_sprt else 0


if __name__ == "__main__":
    sys.exit(main())
