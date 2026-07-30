"""Multi-move endgame conversion test.

Plays the engine against itself from a high-rule50 TB-winning position,
under bullet TC. Tracks whether the winning side converts to mate/zeroing
before the 50-move counter hits 100 (auto-draw) or the move budget is
exhausted.

Distinguishing this from the single-move diagnostic:
- single-move: did the engine choose the DTZ-optimal move? (preserves rank)
- multi-move:  did the engine actually convert? (the user's real symptom)

This is the test of record for the DTZ fix because:
- The user's PGN game 2 failed in this exact regime (multi-move conversion
  near the 50-move rule).
- A single-move-optimal but multi-move-suboptimal fix would pass the first
  test but still fail the user.

Usage:
  py testing\\test_endgame_conversion.py <engine.exe> [<engine.exe>...]
  (multiple engines run head-to-head on the same positions)
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
import time
from pathlib import Path

import chess
import chess.syzygy


ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EXE = ROOT / ("Hypersion.exe" if sys.platform == "win32" else "Hypersion")
SYZYGY = ""
WTIME  = 2000   # 2s clock each — bullet
BTIME  = 2000
INC    = 0      # no increment — flag-out is on the table
MAX_PLIES = 60  # ~30 moves of play before we give up

# (label, FEN where winning side is to move, rule50_count_start)
# All have rule50_count high — the regime where Fathom's tbRank starts
# differentiating. Without the DTZ fix, engine shuffles and gets adjudicated;
# with the fix, engine should convert.
POSITIONS = [
    ("KRvK   rule50=85 W winning",
     "8/8/8/4k3/8/4K3/8/4R3 w - - 85 50"),
    ("KQvK   rule50=85 W winning",
     "8/8/8/4k3/8/4K3/8/4Q3 w - - 85 50"),
    ("KQvK   rule50=70 W winning",
     "8/8/8/4k3/8/4K3/8/4Q3 w - - 70 50"),
    ("KRvK   rule50=70 W winning",
     "8/8/8/4k3/8/4K3/8/4R3 w - - 70 50"),
    ("KBPvK  rule50=60 W winning",
     "8/8/8/4k3/8/4K3/4P3/4B3 w - - 60 50"),
    ("KBNvK  rule50=40 W winning",
     "8/8/8/4k3/8/4K3/4N3/4B3 w - - 40 50"),
]


class EngineSession:
    def __init__(self, exe_path: Path):
        self.exe = exe_path
        self.proc = subprocess.Popen(
            [str(exe_path)],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            encoding="utf-8",
            bufsize=1,
        )
        self.write("uci")
        self.wait_for("uciok")
        self.write("setoption name Threads value 1")
        self.write("setoption name Hash value 64")
        self.write(f"setoption name SyzygyPath value {SYZYGY}")
        self.write("ucinewgame")
        self.write("isready")
        self.wait_for("readyok")

    def write(self, s: str):
        self.proc.stdin.write(s + "\n")
        self.proc.stdin.flush()

    def wait_for(self, marker: str, timeout: float = 5.0) -> str:
        """Read engine stdout until a line contains `marker`. Return all lines."""
        deadline = time.time() + timeout
        buf = []
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                break
            buf.append(line)
            if marker in line:
                return "".join(buf)
        return "".join(buf)

    def play(self, board: chess.Board, wtime_ms: int, btime_ms: int) -> chess.Move | None:
        """Set position via UCI moves from startpos / fen, ask for go."""
        fen = board.fen()
        self.write(f"position fen {fen}")
        self.write(f"go wtime {wtime_ms} btime {btime_ms} winc {INC} binc {INC}")
        # Wait for bestmove
        deadline = time.time() + 30
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            if not line:
                break
            m = re.match(r"^bestmove\s+(\S+)", line)
            if m:
                uci = m.group(1)
                if uci == "(none)" or uci == "0000":
                    return None
                try:
                    return chess.Move.from_uci(uci)
                except ValueError:
                    return None
        return None

    def close(self):
        try:
            self.write("quit")
        except Exception:
            pass
        try:
            self.proc.wait(timeout=2)
        except subprocess.TimeoutExpired:
            self.proc.kill()


def play_position(exe_path: Path, start_fen: str, tb: chess.syzygy.Tablebase) -> dict:
    """Play out one position with the engine on both sides. Track conversion.
    Returns: { converted: bool, mate_ply: int|None, final_rule50: int,
               final_fen: str, plies_played: int, end_reason: str }
    """
    eng = EngineSession(exe_path)
    board = chess.Board(start_fen)
    wtime_ms, btime_ms = WTIME, BTIME

    # Track the side that is winning per TB
    try:
        start_wdl = tb.probe_wdl(board)
        start_dtz = tb.probe_dtz(board)
    except (chess.syzygy.MissingTableError, KeyError):
        start_wdl = 0
        start_dtz = 0
    winning_color = board.turn if start_wdl > 0 else (not board.turn) if start_wdl < 0 else None
    rule50_feasible = winning_color is not None and board.halfmove_clock + abs(start_dtz) <= 100

    plies = 0
    end_reason = "?"
    while plies < MAX_PLIES:
        if board.is_game_over(claim_draw=False):
            end_reason = board.outcome().termination.name if board.outcome() else "game_over"
            break
        if board.halfmove_clock >= 100:
            end_reason = "50_move_rule_drew"
            break

        t0 = time.time()
        move = eng.play(board, wtime_ms, btime_ms)
        elapsed = int((time.time() - t0) * 1000)

        if move is None or move not in board.legal_moves:
            end_reason = f"engine_returned_invalid_move({move})"
            break

        # Update clocks
        if board.turn == chess.WHITE:
            wtime_ms = max(0, wtime_ms - elapsed)
        else:
            btime_ms = max(0, btime_ms - elapsed)
        if wtime_ms == 0 or btime_ms == 0:
            end_reason = f"flag_out_{'white' if wtime_ms == 0 else 'black'}"
            break

        board.push(move)
        plies += 1

    eng.close()

    # Did the winning side convert?
    converted = False
    mate_ply = None
    if board.is_checkmate():
        winner = not board.turn  # mated side moves last
        converted = (winner == winning_color)
        if converted:
            mate_ply = plies
        if end_reason == "?":
            end_reason = "checkmate_winner_" + ("white" if winner == chess.WHITE else "black")
    elif end_reason == "?":
        end_reason = "max_plies_reached"

    return dict(
        converted=converted,
        required=rule50_feasible,
        start_wdl=start_wdl,
        start_dtz=start_dtz,
        mate_ply=mate_ply,
        plies_played=plies,
        final_rule50=board.halfmove_clock,
        final_fen=board.fen(),
        end_reason=end_reason,
        wtime_left=wtime_ms,
        btime_left=btime_ms,
    )


def run_binary(exe_path: Path, tb: chess.syzygy.Tablebase) -> dict:
    print(f"=== {exe_path.name} ===")
    summary = []
    for label, fen in POSITIONS:
        result = play_position(exe_path, fen, tb)
        mark = "CONVERT" if result["converted"] else "FAIL" if result["required"] else "EXPECTED DRAW"
        print(f"  [{mark}] {label}")
        print(f"       plies={result['plies_played']:>3}  "
              f"rule50_end={result['final_rule50']:>3}  "
              f"start_dtz={result['start_dtz']:>3}  "
              f"mate_ply={result['mate_ply']}  "
              f"reason={result['end_reason']}")
        print(f"       wtime_left={result['wtime_left']}ms  btime_left={result['btime_left']}ms")
        summary.append((label, result))
    required_count = sum(1 for _, r in summary if r["required"])
    converted_count = sum(1 for _, r in summary if r["required"] and r["converted"])
    failures = required_count - converted_count
    print(f"  TOTAL: {converted_count}/{required_count} feasible wins converted; "
          f"{len(POSITIONS) - required_count} expected rule-50 draw(s)")
    print()
    return dict(converted=converted_count, required=required_count,
                failures=failures, summary=summary)


def main() -> int:
    global SYZYGY, WTIME, BTIME, INC, MAX_PLIES
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("engines", type=Path, nargs="*", default=[DEFAULT_EXE])
    parser.add_argument(
        "--syzygy",
        type=Path,
        default=Path(os.environ["HYPERSION_SYZYGY"]) if "HYPERSION_SYZYGY" in os.environ else None,
        help="directory containing Syzygy tablebases (or set HYPERSION_SYZYGY)",
    )
    parser.add_argument("--clock-ms", type=int, default=WTIME)
    parser.add_argument("--increment-ms", type=int, default=INC)
    parser.add_argument("--max-plies", type=int, default=MAX_PLIES)
    args = parser.parse_args()
    if args.syzygy is None or not args.syzygy.is_dir():
        parser.error("a valid --syzygy directory is required")
    if args.clock_ms < 1 or args.increment_ms < 0 or args.max_plies < 1:
        parser.error("clock/max-plies must be positive and increment non-negative")
    binaries = [path.resolve() for path in args.engines]
    missing = [str(path) for path in binaries if not path.is_file()]
    if missing:
        parser.error(f"engine not found: {missing[0]}")
    SYZYGY = str(args.syzygy.resolve())
    WTIME = BTIME = args.clock_ms
    INC = args.increment_ms
    MAX_PLIES = args.max_plies
    tb = chess.syzygy.open_tablebase(SYZYGY)
    print(f"Syzygy: {SYZYGY}")
    print(f"Clock:  wtime={WTIME}ms btime={BTIME}ms  inc={INC}ms")
    print(f"Cap:    {MAX_PLIES} plies per position")
    print("=" * 80)
    failures = 0
    for exe in binaries:
        failures += run_binary(exe, tb)["failures"]
    tb.close()
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
