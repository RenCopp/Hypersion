"""Find the EARLIEST move in each loss where Hypersion's eval diverged
from Stockfish ground truth — that's the real blunder, not the perceived
critical move.

For each loss:
  1. Walk every Hypersion move
  2. At each, compare Hypersion's eval (from PGN comment) to Stockfish's
     eval at depth 16 of the position BEFORE Hypersion's move
  3. Find the first move where Hypersion was 100+ cp more optimistic than
     reality (eval > sf_eval + 100)
  4. Classify what kind of position that was

Output: distribution of divergence moments + piece counts.
"""

import chess
import chess.engine
import chess.pgn
import re
import sys
from pathlib import Path
from collections import Counter

PGNS = [
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_sf2400.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_obsidian.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_alexandria.pgn",
    r"C:\Engine\Hypersion\testing\gauntlet2_vs_rubichess.pgn",
]
HYPER_NAME = "Hyp_LMR"
STOCKFISH = r"C:\Engine\stockfish\stockfish-windows-x86-64-avx2.exe"
SF_DEPTH = 16  # ground truth depth — slightly faster, still solid
DIVERGE_TH = 100  # cp threshold for "Hypersion is overoptimistic"

EVAL_RE = re.compile(r'^([+\-]?)(?:M(\d+)|(\d+(?:\.\d+)?))$')

def parse_eval(s):
    m = EVAL_RE.match(s)
    if not m:
        return None
    sign_str, mate_str, num_str = m.groups()
    sign = -1 if sign_str == '-' else 1
    if mate_str is not None:
        return sign * 1500
    if num_str is not None:
        return int(round(sign * float(num_str) * 100))
    return None

def piece_count(board):
    return chess.popcount(board.occupied)

def collect_hyper_moves(game, hyper_color):
    """Walk the game, return list of (board_before, eval_cp_hyper, move, fullmove)"""
    board = game.board()
    out = []
    for ply, node in enumerate(game.mainline()):
        side = chess.WHITE if (ply % 2 == 0) else chess.BLACK
        move = node.move
        if side == hyper_color:
            comment = node.comment or ""
            mc = re.match(r'\s*([+\-]?M?\d+(?:\.\d+)?)\s*/\s*(\d+)', comment)
            if mc:
                eval_cp = parse_eval(mc.group(1))
                if eval_cp is not None and abs(eval_cp) < 1500:  # skip mate flags
                    out.append({
                        "board": board.copy(),
                        "fen": board.fen(),
                        "eval_cp_hyper": eval_cp,
                        "move": move,
                        "san": board.san(move),
                        "fullmove": ply // 2 + 1,
                        "piece_count": piece_count(board),
                    })
        board.push(move)
    return out

def analyze_position(engine, fen, depth):
    """Return Stockfish eval from side-to-move POV at given depth."""
    board = chess.Board(fen)
    info = engine.analyse(board, chess.engine.Limit(depth=depth))
    score = info["score"].white()
    stm_sign = 1 if board.turn == chess.WHITE else -1
    return stm_sign * (score.score(mate_score=15000) or 0)

def main():
    losses = []
    for pgn_path in PGNS:
        opp = Path(pgn_path).stem.replace("gauntlet2_vs_", "")
        with open(pgn_path) as f:
            while True:
                game = chess.pgn.read_game(f)
                if game is None:
                    break
                white = game.headers.get("White", "")
                black = game.headers.get("Black", "")
                result = game.headers.get("Result", "*")
                if HYPER_NAME == white:
                    hyper_color = chess.WHITE
                elif HYPER_NAME == black:
                    hyper_color = chess.BLACK
                else:
                    continue
                hyper_lost = (result == "0-1" and hyper_color == chess.WHITE) or \
                             (result == "1-0" and hyper_color == chess.BLACK)
                if not hyper_lost:
                    continue
                hyper_moves = collect_hyper_moves(game, hyper_color)
                if len(hyper_moves) < 4:
                    continue
                losses.append({"opponent": opp, "moves": hyper_moves})

    print(f"=== {len(losses)} losses; SF depth {SF_DEPTH}, divergence threshold {DIVERGE_TH} cp ===\n")

    # For speed, sample every Hypersion move from move 5 onwards,
    # but only check until divergence is found.
    divergence_records = []
    total_queries = 0

    with chess.engine.SimpleEngine.popen_uci(STOCKFISH) as sf:
        sf.configure({"Hash": 256, "Threads": 1})
        for li, loss in enumerate(losses, 1):
            moves = loss["moves"]
            divergence = None
            # Check every other move from index 4 onward (skip opening)
            sample_indices = list(range(4, len(moves), 2))
            for mi in sample_indices:
                rec = moves[mi]
                sf_eval = analyze_position(sf, rec["fen"], SF_DEPTH)
                total_queries += 1
                hyper_eval = rec["eval_cp_hyper"]
                gap = hyper_eval - sf_eval  # positive = Hypersion thinks better than reality
                if gap >= DIVERGE_TH:
                    divergence = {
                        "fullmove": rec["fullmove"],
                        "san": rec["san"],
                        "hyper_eval": hyper_eval,
                        "sf_eval": sf_eval,
                        "gap": gap,
                        "piece_count": rec["piece_count"],
                    }
                    break
            if divergence is None:
                # Try refined sweep — every move from 4 to end
                for mi in range(4, len(moves)):
                    if mi in sample_indices:
                        continue
                    rec = moves[mi]
                    sf_eval = analyze_position(sf, rec["fen"], SF_DEPTH)
                    total_queries += 1
                    hyper_eval = rec["eval_cp_hyper"]
                    gap = hyper_eval - sf_eval
                    if gap >= DIVERGE_TH:
                        divergence = {
                            "fullmove": rec["fullmove"],
                            "san": rec["san"],
                            "hyper_eval": hyper_eval,
                            "sf_eval": sf_eval,
                            "gap": gap,
                            "piece_count": rec["piece_count"],
                        }
                        break
            divergence_records.append({"opponent": loss["opponent"], "div": divergence})
            if divergence:
                d = divergence
                print(f"  [{li:2d}] {loss['opponent']:<10} mv{d['fullmove']:3d} "
                      f"{d['san']:<6}  hyper={d['hyper_eval']:+5d} "
                      f"sf={d['sf_eval']:+5d} gap={d['gap']:+5d} pc={d['piece_count']:2d}")
            else:
                print(f"  [{li:2d}] {loss['opponent']:<10} no divergence >= {DIVERGE_TH} cp found")

    print(f"\n=== Total Stockfish queries: {total_queries} ===")

    # Aggregate
    has_div = [r for r in divergence_records if r["div"] is not None]
    print(f"\n=== Found divergence in {len(has_div)} / {len(divergence_records)} losses ===")

    print("\n--- First-divergence move number distribution ---")
    mn_buckets = Counter()
    for r in has_div:
        mv = r["div"]["fullmove"]
        if mv <= 15:        mn_buckets["1-15  (opening)"] += 1
        elif mv <= 30:      mn_buckets["16-30 (middlegame)"] += 1
        elif mv <= 45:      mn_buckets["31-45 (late MG)"] += 1
        elif mv <= 60:      mn_buckets["46-60 (early endgame)"] += 1
        elif mv <= 90:      mn_buckets["61-90 (endgame)"] += 1
        else:               mn_buckets["91+   (deep endgame)"] += 1
    for k in sorted(mn_buckets.keys()):
        print(f"  {k:<28} {mn_buckets[k]:3d}")

    print("\n--- Piece count at first-divergence ---")
    pc_buckets = Counter()
    for r in has_div:
        pc = r["div"]["piece_count"]
        if pc <= 5:        pc_buckets["1-5  (Syzygy zone)"] += 1
        elif pc <= 7:      pc_buckets["6-7  (near endgame)"] += 1
        elif pc <= 12:     pc_buckets["8-12 (early endgame)"] += 1
        elif pc <= 20:     pc_buckets["13-20 (middlegame)"] += 1
        else:              pc_buckets["21+  (early/middle)"] += 1
    for k in sorted(pc_buckets.keys()):
        print(f"  {k:<28} {pc_buckets[k]:3d}")

if __name__ == "__main__":
    main()
